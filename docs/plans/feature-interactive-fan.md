# Feature plan — interactive fan (tab activation) + empty-state cleanup

Status: DRAFT (design only). Refine against current code before starting.
Touches: FanPopup, TabReader, DockWindow, Renderer, Store. No AppBar
register/height churn (see Part 2). All UIA stays on the TabReader worker.

Grounded against: CLAUDE.md rules 4/5/6/7, FanPopup.{h,cpp}, TabReader.{h,cpp},
Store.{h,cpp}, DockWindow.{h,cpp}, Renderer.{h,cpp} as they stand on 2026-07-04.
Steering from a Fable consult + two win32-scout spikes (R1/R2) fold in at the seams.

## Goals

Part 1 — Make fan rows clickable to activate a specific tab in a minimized
browser window. Restore + foreground FIRST, then UIA `SelectionItemPattern.Select`
on a RE-SNAPSHOTTED element matched by title (index fallback). Never select a
wrong tab. Keep the fan alive across the card→fan gap (hover-bridge).

Part 2 — When zero windows are minimized: draw no browser cards and no
"no minimized browsers" placeholder, WITHOUT any AppBar height/register change,
and WITHOUT dropping the fallback automation buttons the dock hosts (Stage 5b
fallback). Ship option (a) "paint nothing" now; defer zero-space collapse.

## Non-goals / deferred

- Zero-space AppBar collapse (would be an ABM_SETPOS height-shrink later, NEVER
  an ABM_REMOVE/ABM_NEW cycle per minimize/restore). Explicitly deferred.
- Synthetic input (Ctrl+click / keyboard) for tab switching — rejected as
  fragile / focus-racy. UIA Select only.
- Retaining live UIA element pointers in the snapshot — rejected; elements are
  not durable across restore. Identity is by value (title + original index).

---

## Message + thread contract (new)

Existing WM_APP slots in use: +1 kCallbackMsg, +2 kWindowEventMsg,
+3 kTabSnapshotMsg, +4 kConfigChangedMsg, +5 kRemeasureMsg, +6 kGapStateMsg.

New:

- `kFanActivateMsg = WM_APP + 7`  FanPopup(UI) → Dock(UI). A fan row was
  clicked. wparam = (WPARAM)targetHwnd (the browser HWND the fan is showing),
  lparam = row index (int, 0-based into the fan's displayed rows). Posted, not
  called, to avoid reentrancy into FanPopup's wndproc.

- `kTabActivateResultMsg = WM_APP + 8`  TabReader worker → Dock(UI). Outcome of
  an ActivateTab request. wparam = (WPARAM)targetHwnd. lparam = (LPARAM)payload*
  (`new TabActivateResult`), deleted by the UI handler exactly like
  kTabSnapshotMsg deletes TabSnapshot. If PostMessage fails, worker deletes it.

No other thread ever writes Store or touches a window. UI thread remains the
sole Store writer and window owner (rule 5). UIA remains worker-only (rule 6).

### TabActivateResult payload (TabReader.h)

    enum class ActivateOutcome { Selected, NoMatch, PatternUnavailable, Failed };
    struct TabActivateResult {
        HWND            hwnd;
        ActivateOutcome outcome;
        int             matchedIndex = -1;   // resolved tab index, or -1
        std::vector<Tab> freshTabs;          // the re-snapshot, for Store refresh
    };

Carrying `freshTabs` lets the UI refresh Store.tabs from the same re-snapshot the
worker already did (fresh at restore time), avoiding a second UIA pass and keeping
the card/fan in sync after activation.

---

## Part 1 — clickable fan → tab activation

### 1.1 Fan row → live tab identity

The snapshot (`std::vector<Tab>`) holds only {title, active}; no UIA element is
retained (correct — elements don't survive restore). Row identity is by VALUE:

- primary key: `title` (already CleanTabTitle-normalized in TabReader).
- fallback key: original index in the window's tab vector at display time.

FanPopup truncates the displayed list to `shown` rows and may append a "+N more"
pseudo-row. The clicked row index must map back to a REAL tab index, so FanPopup
remembers, per shown row, the original index into the full tab vector it was
handed. The "+N more" row maps to -1 (non-clickable).

### 1.2 End-to-end sequence (the staleness-safe flow)

1. (FanPopup, UI) WM_LBUTTONDOWN → hit-test row. If it's the "+N more" row or
   outside any row, do nothing. Otherwise Post `kFanActivateMsg` to the dock
   with (targetHwnd, rowIndex). Do NOT close yet.

2. (Dock, UI) `kFanActivateMsg`:
   a. Resolve `wantedTitle` (stored title for that row) + `fallbackIndex`
      (original tab index).
   b. RESTORE + FOREGROUND FIRST (patterns on a minimized/background Chrome are
      unreliable). Reuse `RestoreWindow(targetHwnd)` (SW_RESTORE +
      SetForegroundWindow, FlashWindowEx fallback). Per R1 (scout): the fan click
      makes the dock the "last input recipient", so `SetForegroundWindow` works
      directly — NO `AllowSetForegroundWindow` needed (that only delegates rights
      to another process). The fan already returns `MA_NOACTIVATE` from
      WM_MOUSEACTIVATE, which is REQUIRED to avoid the hover-activation trap. See
      Part 8.
   c. Fire the worker: `m_tabReader->RequestActivate(targetHwnd, wantedTitle,
      fallbackIndex)`.
   d. Close the fan now (activation committed; the window coming forward is the
      feedback). UX toggle: move Hide() to the outcome handler if "keep fan until
      confirmed" is preferred — one-line change.

3. (TabReader worker) after the restore, a brief bounded settle (R2), then:
   a. RE-WALK to get fresh TabItem ELEMENTS (variant of SnapshotTabs that KEEPS
      the element array). Reuse tree-shape logic: TabControl via
      FindAll(TreeScope_Descendants), IsInsideDocument filter, TabItem via
      FindAllBuildCache(Descendants).
   b. MATCH title-first: collect indices whose cleaned name == wantedTitle.
      0 → NoMatch (no Select). 1 → that index. >1 → fallbackIndex if among them
      else first match. A bare index match NEVER selects alone (never a wrong tab).
   c. SELECT: GetCurrentPattern(UIA_SelectionItemPatternId) →
      IUIAutomationSelectionItemPattern::Select(). Null/failed pattern →
      PatternUnavailable.
   d. Post `kTabActivateResultMsg` with outcome + matchedIndex + freshTabs.

4. (Dock, UI) `kTabActivateResultMsg`: delete payload; if freshTabs non-empty →
   `m_store.SetTabs(hwnd, freshTabs)` + InvalidateRect. NoMatch/PatternUnavailable/
   Failed → window already restored (graceful degrade, no error UI).

Everything crossing threads is PostMessage; Store touched only in step 4 on UI.

### 1.3 Row→index mapping detail

Add to FanPopup `std::vector<int> m_rowTabIndex` filled in Show(): per displayed
row, the index into the ORIGINAL tabs vector (Store order); "+N more" → -1.
FanPopup also remembers `m_targetHwnd` (whose tabs it shows) and `m_ownerHwnd`
(dock, to post to). ShowFanFor passes the card HWND into Show().

---

## Part 2 — TabReader additions (ActivateTab)

### 2.1 Public API

    void RequestActivate(HWND hwnd, std::wstring wantedTitle, int fallbackIndex);

Queue carries two request kinds. Replace `std::deque<HWND>` with:

    enum class ReqKind { Snapshot, Activate };
    struct Request { ReqKind kind; HWND hwnd; std::wstring wantedTitle; int fallbackIndex; };
    std::deque<Request> m_queue;

RequestSnapshot pushes {Snapshot,hwnd,{},0}; RequestActivate pushes
{Activate,hwnd,title,idx}. Keep the snapshot de-dupe; do NOT de-dupe Activate.
Ctor takes (dockHwnd, snapshotMsg, activateMsg).

### 2.2 Worker: ActivateTab (COM spec)

New free fn in TabReader.cpp anon namespace, reusing SnapshotTabs' tree logic:

1. ElementFromHandle(hwnd) → root; fail → {Failed,-1,{}}.
2. CacheRequest: Name + SelectionItemIsSelected (+ AddPattern
   UIA_SelectionItemPatternId as a cheap availability hint).
3. Find TabControls (ControlType==TabControl, Descendants); skip IsInsideDocument.
4. First TabControl yielding TabItems: FindAllBuildCache(Descendants, TabItem);
   build vector<Tab> AND keep the element array (do NOT discard like SnapshotTabs).
5. MATCH (title-first, index tiebreak) as in 1.2b.
6. SELECT on the matched element via GetCurrentPattern (LIVE, not cached — the
   action must run against the live provider) → As<IUIAutomationSelectionItem
   Pattern> → Select(). Null → PatternUnavailable; FAILED(Select) → Failed; else
   Selected.
7. freshTabs = built vector<Tab> (mark matchedIndex.active on Selected).

### 2.3 Settle / retry (R2 — scout-resolved)

Activate branch only, on the WORKER (never blocks UI). Two-stage gate per R2:
1. GATE on window readiness: `IsWindowVisible(hwnd) && !IsIconic(hwnd)` — poll
   ~50ms until true, timeout ~3s. Restore is async; Select on a still-minimized/
   mid-animation window returns S_OK but SILENTLY no-ops (the worst failure mode).
2. Then FindFirst TabControl with TabItems; retry ~50ms if not found (tree
   rebuilds async after restore, 100–500ms typical), same 3s ceiling.
Only after both pass → Select(). Constants (poll interval, timeout) named at the
top of TabReader.cpp. NO fixed sleep (too short on slow machines, wasteful on
fast). Confirm success by re-reading SelectionItemIsSelected on the target after
Select() (Select returns S_OK even on silent failure) → outcome Selected only if
confirmed, else Failed.

Alternative to polling (deferred option): the dock already has an
EVENT_SYSTEM_FOREGROUND hook — it could drive the Select once the browser fires
foreground, instead of the worker poll. Poll is simpler + self-contained; keep it
for v1.

### 2.4 Worker loop dispatch

Switch on Request.kind: Snapshot → existing path; Activate → ActivateTab + new
TabActivateResult + PostMessageW(kTabActivateResultMsg); delete on post-failure.
COM stays MTA; Select() is safe from the MTA worker.

---

## Part 3 — FanPopup: hover-bridge + click

### 3.1 State machine

`enum class FanState { Hidden, HoveringCard, HoveringFan, ClosingGrace }`.
"Alive region" = union of the originating card's screen rect and the fan window's
screen rect. Inside union → stay shown. Leaving union → ClosingGrace + short grace
timer (~180–250ms). Re-enter before it fires → Hovering*; timer fires while
outside → Hide. Click elsewhere / foreground change → Hide immediately.

The card→fan GAP is the classic transient-popup killer (fan bottom anchored at
stripTopScreen-1, adjacent to the strip top). The union must include the 1px seam
inflated a couple px so a fast diagonal move doesn't fall between them.

### 3.2 Two mouse sources

FanPopup keeps WS_EX_NOACTIVATE (still receives mouse messages):
- Fan WM_MOUSEMOVE: TrackMouseEvent(TME_LEAVE); state HoveringFan; kill grace.
- Fan WM_MOUSELEAVE: ClosingGrace + start grace timer (owned by FanPopup on its
  own HWND).
- Dock WM_MOUSEMOVE (existing): if hovered card == fan origin → CancelGrace
  (HoveringCard); different card → existing ShowFanFor re-Show; no card & fan not
  hovered → let grace run.

Grace timer lives in FanPopup; torn down in Hide()/Destroy(). Dock's open-delay
kHoverTimer unchanged.

### 3.3 Row hit-test + click

`int RowAt(POINT ptClient) const` using the SAME geometry Paint() uses (factor
row geometry into a shared helper to avoid drift). "+N more" → -1. On
WM_LBUTTONDOWN: r = RowAt; if r>=0 && m_rowTabIndex[r]>=0 → Post kFanActivateMsg
to m_ownerHwnd with (m_targetHwnd, m_rowTabIndex[r]). Keep MA_NOACTIVATE.

### 3.4 New FanPopup API

    bool Create(HINSTANCE, HWND ownerHwnd);
    void Show(HWND targetHwnd, const std::vector<Tab>& tabs,
              const RECT& cardScreenRect, int stripTopScreen, UINT dpi);
    void CancelGrace();

New members: state, m_targetHwnd, m_ownerHwnd, m_cardScreenRect, m_rowTabIndex,
m_rowBounds, grace timer id. Hide() → Hidden + kill grace. Destroy() kills grace
before DestroyWindow.

---

## Part 4 — Empty-state (Renderer + DockWindow), option (a)

`Renderer::Paint` currently draws "no minimized browsers" when cards.empty().
Change: delete that placeholder branch — when cards.empty() draw NOTHING for the
cards region (bg fill already happened). The buttons-overlay loop STILL runs, so
the dock keeps painting the Stage-5b fallback automation buttons when
`DockButtons()` returns them. "Paint nothing" = no CARDS and no PLACEHOLDER, NOT
"paint nothing at all" — the fallback button host survives.

No change to DockHeightPx / AppBarSetPos: dock still reserves one band when idle
(hosts fallback buttons + a strip to fan from). NO ABM_REMOVE/ABM_NEW, NO height
shrink. Paint-only, zero AppBar churn (option a). Collapse deferred (would be a
single debounced ABM_SETPOS height change, never per-minimize toggling).

---

## Part 5 — Threading / AppBar / DPI / teardown

- Threading: one new worker request kind (Activate) on the EXISTING TabReader
  thread — no new thread. One worker→UI msg (kTabActivateResultMsg), one UI→UI
  post (kFanActivateMsg). Store on UI only; UIA (incl. Select) on worker only.
- AppBar: no new register/remove, no height renegotiation. Part 4 paint-only.
  Every existing ABM_REMOVE exit path untouched.
- DPI: FanPopup RowAt must scale pad/rowH by m_dpi exactly as Paint (shared
  geometry helper). Union-rect math in physical px (already DPI-correct).
- Teardown: FanPopup grace timer killed in Hide()/Destroy(). TabReader ~dtor
  joins the worker (so no late kTabActivateResultMsg touches a dead Store);
  TabActivateResult deleted by UI handler or worker-on-post-failure. WM_DESTROY
  order (fanPopup.reset → tabReader.reset(join) → unhook → AppBarRemove) stays
  correct; a late result post to a destroyed hwnd fails → worker deletes payload.

---

## Part 6 — Step ordering (each gated by inspector-burst + adjudicator)

Two throwaway SPIKES first (reported, not merged):

- SPIKE A (R1) — Foreground rights from a NOACTIVATE popup click: does
  click→Post→dock→SetForegroundWindow(target) bring it forward, with/without
  AllowSetForegroundWindow(ASFW_ANY)? Output: the exact foreground recipe for 2b.
- SPIKE B (R2) — Edge/Chrome TabItem SelectionItemPattern availability right after
  SW_RESTORE: how many retries / how much settle before Select() works? Sets 2.3's
  constants.

Feature steps:

1. TabReader request-kind refactor + ActivateTab (worker-only; dock logs outcome
   under _DEBUG). Lenses: threading, COM/resource hygiene.
2. Dock handlers kFanActivateMsg + kTabActivateResultMsg; restore+foreground-first
   (R1 recipe); Store refresh. Lenses: threading, AppBar-hygiene.
3. FanPopup click + RowAt + row→index (replaces debug trigger). Lenses: DPI,
   threading.
4. FanPopup hover-bridge state machine + grace timer. Lenses: threading, DPI,
   AppBar-hygiene (timer teardown).
5. Renderer/DockWindow empty-state (a). Lenses: DPI/paint, AppBar-hygiene (assert
   no AppBarSetPos from this change).

Any prefix can ship independently.

---

## Part 7 — Open risks / clean seams

- R1 (SPIKE A): foreground rights from a NOACTIVATE-popup click. Seam: one
  `bool BringForward(HWND)` helper (AllowSetForegroundWindow + RestoreWindow +
  verify GetForegroundWindow). FlashWindowEx fallback already exists.
- R2 (SPIKE B): SelectionItemPattern availability post-restore. Seam: named retry
  constants at top of TabReader.cpp; degrades to PatternUnavailable → restored.
- Duplicate tab titles → resolved by fallbackIndex tiebreak among title matches;
  bare index never selects alone.
- Stale fan tabs → mitigated by re-snapshot at click; freshTabs refreshes the card.
- Grace-timer vs new-card hover → Show() must cancel any pending grace for the
  previous card.

## Part 8 — Scout findings (R1/R2 RESOLVED, 2026-07-04)

Full report: win32-scout (see also `docs/research/` if a file was saved). Both
seams are now settled; the spikes in Part 6 become CONFIRMATION tests, not open
research.

### R1 — foreground from a WS_EX_NOACTIVATE fan click: WORKS, no ASFW
- `WS_EX_NOACTIVATE` suppresses activation, NOT input delivery. The click still
  posts WM_LBUTTONDOWN to the fan's queue → the dock process becomes the "last
  input recipient" → `SetForegroundWindow(browser)` is permitted. Direct call;
  `AllowSetForegroundWindow` is NOT needed (it only delegates rights to another
  process).
- Recipe (UI thread, in the kFanActivateMsg handler): SW_RESTORE →
  `SetForegroundWindow(browser)` → on FALSE, `FlashWindowEx` (existing fallback).
  Our `RestoreWindow` already does restore + SFW + Flash — reuse it.
- MANDATORY: the fan must return `MA_NOACTIVATE` from WM_MOUSEACTIVATE (it
  already does). Without it, "activate window by hovering" (if the user enabled
  it) steals activation to the fan on hover — Raymond Chen confirms NOACTIVATE
  does NOT block that path.
- Caveats: `LockSetForegroundWindow` (some fullscreen games) blocks all SFW →
  FlashWindowEx is the correct degraded behavior. The inference is architectural,
  not spelled out in one MSDN line → SPIKE A stays as a quick confirm, but the
  FlashWindowEx net makes a wrong guess harmless.
- Threading: SFW on the UI thread (owner of the fan queue). Restore stays async
  (`ShowWindowAsync`) so a hung browser can't block our pump; the worker's R2
  gate (`IsWindowVisible && !IsIconic`) absorbs the async delay — so we do NOT
  need the scout's synchronous `ShowWindow` for ordering.

### R2 — SelectionItemPattern on browser tabs: SUPPORTED, gate on readiness
- SelectionItemPattern is REQUIRED on TabItem; `Select()` → Chromium `kDoDefault`
  → switches the tab. Chrome 138+/current Edge = native UIA (reliable); pre-138
  used an MSAA→UIA proxy (unreliable) — our continuous UIA polling keeps the a11y
  tree alive either way.
- `InvokePattern` is NOT supported on TabItem (spec) — do NOT use it as a
  fallback. Fallback chain if Select is unavailable/unconfirmed:
  `IUIAutomationElement::SetFocus()` on the TabItem (Chrome switches on focus) →
  `LegacyIAccessiblePattern::DoDefaultAction()` → (last resort) SendInput click at
  BoundingRectangle center. Reflect this in 2.2 (replaces any Invoke fallback).
- Disambiguation confirmed: browser tabs = the TabControl NOT inside a Document;
  web-page ARIA tabs live under Document. Our existing `IsInsideDocument` filter
  is exactly right — reuse verbatim.
- The silent-S_OK-on-early-call gotcha is the whole reason for the R2 gate +
  post-Select confirmation (2.3). Without it the feature passes every dev test
  (window already visible) and fails only in production (restore-from-minimized).

### Net design deltas applied
- Dropped `AllowSetForegroundWindow` from 1.2b.
- 2.3 gate rewritten: `IsWindowVisible && !IsIconic` + TabControl-found retry +
  post-Select confirmation; no fixed sleep.
- Fallback chain = SetFocus → LegacyIAccessible.DoDefaultAction (NOT Invoke).
- SPIKE A/B downgraded from research to quick on-Windows confirmation.

## Decision needed from user

- Close the fan immediately on click (current default) vs keep it until the
  outcome confirms (one-line move of Hide()).
