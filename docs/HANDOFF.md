# HANDOFF — read this first, every session

Entry point for any fresh Claude session (or human) working on this repo.
Purpose: give full project direction with minimal context, so work starts at
the right step with no drift.

## Project in three sentences

`browser_shell_os` is a native C++17/Win32 Windows shell tool: an AppBar dock
that reserves a strip directly above the taskbar. Minimized browser windows
leave their tab info visible there — eventually as staggered card stacks
aggregating multiple windows — plus automation buttons in the taskbar's empty
space. A separate, never-bundled `shell_profiler` observes the shell's
performance over ETW.

## How to work here

1. Obey `CLAUDE.md` (working rules — short, read it whole).
2. Find the next action on the status board below.
3. Open ONLY that stage's plan in `docs/plans/` and the matching
   `docs/ARCHITECTURE.md` section. Do not load everything.
4. Implement exactly one step; pass its checkpoint; update this file
   (status board + session log); commit.
5. Building/running requires Windows (MSVC + CMake). From a non-Windows
   session: write the code, say clearly that runtime verification is pending.

## Status board

| Workstream | State |
|---|---|
| Docs & plans | ✅ Complete (architecture + per-stage plans) |
| Stage 1 — AppBar dock | ✅ Complete — all 7 steps + acceptance row passed on Win11 |
| Stage 2 — browser detection | ✅ Complete — all 4 steps + §12 row 2 accepted on Win11 |
| Stage 3 — single-window tabs | ✅ Complete — tabs render per-window on minimize, accepted on Win11 |
| Stage 4 — multi-window stacks | 🟡 code complete (4.1–4.5 + 4.5a) — §12 row 4 acceptance pending on Windows |
| Stage 5 — taskbar buttons | ✅ ACCEPTED on Win11 — 5a dock buttons + 5b gap overlay (pills-in-gap, event re-measure, single-host dock fallback); all 5 visual checks pass |
| Profiler (parallel workstream) | 🟡 consumer P.2–P.4 code complete + builds green; P.1 shell emit not done — see `docs/plans/profiler.md` |
| Deployment — permanent run ("service" goal) | ⬜ v1 (logon autostart) after Stage 1; v2 (watchdog service) after Stage 5 — see `ARCHITECTURE.md` §13 |

**Next action: user visual check of Phase 5b, then re-verify Stage 1–4 acceptance (§12) + close Stage 5.**
Phase 5b is CODE COMPLETE (5b.1 accepted; 5b.2 pills-in-gap; 5b.3 event-driven re-measure + single-host
dock fallback). **Windows visual check pending** for 5b.3: open many apps → gap shrinks, pills drop, dock
strip stays empty; close apps → pills grow back in the gap; make the gap fail (too small) → pills reappear
in the dock strip (fallback); edit config while gap-active → pills update in-place; empty-gap right-click
still opens taskbar menu. Then run §12 acceptance rows (esp. row 5b) + re-check Stage 1–4 rows on Windows.
Geometry + UIA element reference: `docs/research/win11-taskbar-geometry.md`. `TaskbarOverlayWindow.{h,cpp}`
isolates ALL taskbar heuristics (hard rule 6). Carried debt: host-handoff paints one doubled frame ~tens
of ms at startup/reload (transient); overflow-chevron class unconfirmed (live overflowed taskbar); fence
`ABM_GETSTATE`/worker-join vs hung explorer at shutdown; bounded one-`Gap` shutdown leak; RoundRect
radius-vs-diameter (cosmetic). Also still pending 5a check: dock grows on minimize; config hot-reload ~1s
(%LOCALAPPDATA%\browser_shell_os\config.txt). Profiler workstream still unstarted (see profiler.md).

Deferred debt:
- [F-02 activate-com-hang] ✅ RESOLVED 2026-07-04 — ~TabReader now bounded-joins (2s wait on a
  lifetime-safe shared ExitSignal), then join()s a clean exit or detach()es a worker wedged in an
  uninterruptible cross-process UIA/COM call (ActivateTab Select/SetFocus/DoDefaultAction OR
  SnapshotTabs FindAll). WM_DESTROY → AppBarRemove can no longer stall (hard rule 4). Residual: a
  detached worker touching freed members is shutdown-only UB, unobservable (process exiting) — documented
  at the fix site. Burst (AppBar-hygiene + threading) → adjudicator MAY PROCEED. Isolated in TabReader.
- [renderer-tiny-card] Very narrow cards (rowW < ~48px, i.e. many minimized windows) drop the
  "+N" overflow indicator silently. Degenerate many-window case; revisit if window count grows.
- [tabreader-locale] CleanTabTitle strips English suffixes only (" - Sleeping", " - Pinned",
  " - Memory usage - N MB"). Non-English Chrome/Edge won't strip. Isolated in TabReader (rule 6 OK).
- [F-01 threading] g_dockHwnd non-atomic; CrashFilter reads from faulting thread. HARD GATE:
  must fix (std::atomic<HWND>) before Step 2.3 checkpoint (first worker thread). Also marshal
  SHAppBarMessage call to UI thread.
- [DPI] Mixed-DPI AppBarSetPos monitor/DPI-source mismatch — defer to multi-monitor stage.

**Build note (this machine):** VS2022 Pro's C++ install now works — the
canonical CLAUDE.md commands (`cmake -B build -G "Visual Studio 17 2022"`,
`cmake --build build --config Debug`) succeed. Old NMake/VS2019 workaround
obsolete (2019 BuildTools partially removed). `C:\Program Files\CMake\bin`
still needs adding to PATH.

## Doc map

| Doc | What it is | When to read |
|---|---|---|
| `CLAUDE.md` | Hard rules + session protocol | Always |
| `docs/HANDOFF.md` | This file: state + next action | Always, first |
| `docs/plans/stage-N.md` | Step breakdown w/ checkpoints for stage N | Only the current stage |
| `docs/plans/profiler.md` | Observability workstream steps | Only when working it |
| `docs/ARCHITECTURE.md` | Full design: components, APIs, risks, acceptance tests (§12) | The section for the current stage; §12 row after every step |

## Key invariants (details in CLAUDE.md — these are the expensive ones)

- `ABM_REMOVE` on EVERY exit path, or dead screen space is leaked.
- One UI thread; workers talk via `PostMessage` only.
- Win32 only; no frameworks; no third-party deps without approval.
- Profiler is separate software; shell telemetry is ETW TraceLogging only.
- Never claim runtime verification that wasn't done on Windows.

## Update protocol for this file

After each completed step: flip the status board entry (⬜→✅ for finished
stages, move the **NEXT** marker), rewrite the "Next action" line, and append
one line to the session log. Keep this file short — prune, don't accumulate.

## Session log (append one line per work session)

- 2026-07-04 — Gap-pill (Stage 5b overlay) stability fixes — worked w/ win32-scout advisor. User reported 3
  bugs: (1) YouTube/browser fullscreen didn't hide pills; (2) Start-click → pills stuck hidden; (3) intermittent
  flapping. Advisor root causes: (1) ABN_FULLSCREENAPP only moves the DOCK — overlay is a separate topmost
  window w/ no fullscreen path; (2) Start/Search use DWM cloaking (StartMenuExperienceHost.exe/SearchHost.exe)
  → no taskbar geometry event on close → measure-driven overlay never recovers; (3) task-button UIA rects read
  {0,0,0,0} mid-animation → transient invalid → hide/show churn (PID-scoping already handled cursor noise).
  Fixes (isolated in TaskbarOverlayWindow + dock handlers, rule 6): overlay SetSuppressed(force-hide) + ApplyGap
  hysteresis (one 300ms kRetryTimer before hiding a healthy overlay); dock UpdateOverlaySuppression = m_flyoutOpen
  (foreground proc classify) || FullscreenOnDockMonitor (fg rect==dock-monitor rcMonitor ±2px, NOT a latch),
  wired into existing EVENT_SYSTEM_FOREGROUND + ABN_FULLSCREENAPP; periodic kSafetyTimer(1500ms) self-heals any
  missed clearing event (re-derive while suppressed, re-measure while hidden — gated !m_gapActive so no
  steady-state churn). Burst: threading + DPI + stuck-state (3 inspectors) → 4 stuck-state regressions found →
  fixed (dropped m_fullscreenApp latch, added safety timer) → re-burst stuck-state CLOSED → adjudicator
  MAY PROCEED (residuals: OpenProcess AV-stall bounded; flag-divergence unreachable; both nits addressed —
  ±2px tol, safety churn gate). AppBar invariant verified (kSafetyTimer killed in WM_DESTROY + WM_ENDSESSION).
  Build green. ⇒ VISUAL CHECK PENDING on Win11: open/close Start+Search → pills return; drag windows → no
  flapping; YouTube fullscreen in → pills vanish, exit → pills return; multi-monitor fullscreen (edge).
- 2026-07-04 — Carried-debt polish: F-02 (activate-com-hang) RESOLVED. Root cause: ~TabReader's
  unconditional join() could block forever on a worker wedged in an uninterruptible cross-process
  UIA/COM call; since WM_DESTROY resets TabReader (line 702) BEFORE AppBarRemove (line 715), that
  meant ABM_REMOVE never ran → dead strip (hard rule 4). Fix (isolated in TabReader, rule 6):
  bounded-join — set m_stop/notify, wait_for(2s) on a shared_ptr<ExitSignal> the worker sets after
  WorkerLoop returns, then join() clean or detach() wedged. WM_ENDSESSION path already AppBarRemove's
  first (line 441), so only the normal-quit path was exposed. Compile clean (link blocked only by the
  running dock PID 29992 holding the exe lock — not killed). Burst (AppBar-hygiene + threading) →
  adjudicator MAY PROCEED; applied both warnings (comment now names SnapshotTabs path + corrects the
  race-window claim). Residual detach UAF is shutdown-only + unobservable, documented at fix site.
  Simplifier skipped (small vetted threading fix). Runtime verify pending on Windows (close running
  instance to relink). Remaining open debt: renderer-tiny-card, tabreader-locale, DPI multi-monitor.
- 2026-07-04 — Interactive-fan post-accept fixes (user tested steps 1-5, all 3 behaviors work). (A) Multi-window
  fan-nav bug: vertical card stacking means moving from a lower card up to its fan transits the cards above it;
  Step-4 instant-switch hijacked the fan mid-transit. Fix: reverted to DELAYED switch — a newly hovered card
  always (re)starts kHoverTimer(250ms)→ShowFanFor; fast transit to the fan no longer switches, and WM_MOUSELEAVE
  →BeginHoverGrace KillTimer(kHoverTimer) cancels the pending switch as the cursor enters the fan. (B) Gmail/
  GitHub gap pills 16% narrower: GapButtonLayout pillW ScalePx(84)→71. (Tried dock-fallback ButtonLayout 48→40
  too but 40 clips "GitHub" → reverted to 48; user only sees the gap pills.) Burst (visual/layout + threading/
  interaction) → adjudicator MAY PROCEED (3 threading findings all cleared: CancelGrace=KillTimer no-op; ShowFanFor
  same-card re-show idempotent + not a regression; WM_DESTROY order pre-existing+safe). Applied optional hygiene:
  hoisted KillTimer(kHoverTimer) above m_fanPopup.reset() in WM_DESTROY. Build clean. USER-CONFIRMED WORKING;
  these two are refinements (visual re-check optional).
- 2026-07-04 — Interactive-fan Step 5 done → FEATURE CODE-COMPLETE (all 5 steps). Empty-state option (a):
  Renderer::Paint dropped the "no minimized browsers" placeholder — empty dock now paints only bg fill +
  fallback buttons (card loop unconditional, iterates 0×). Paint-only: DockHeightPx still clamps to ≥1 idle
  band (no AppBar churn, no ABM_*/height change). Burst (paint/DPI/GDI-hygiene + AppBar-no-churn) → adjudicator
  MAY PROCEED (one AppBar finding was git-HEAD-vs-working-tree confusion, dismissed; substance clean: 0 AppBar
  calls, exit paths intact, no leaked HFONT). Trivial deletion → simplifier skipped. Build clean.
  ⇒ CONSOLIDATED VISUAL CHECK PENDING on Win11 for the whole feature (steps 1-5): (1) minimize browser → hover
  card → fan shows tabs → slide up into fan (must not vanish) → click tab → window restores + that tab
  activates (~560ms, perceivable lag OK); (2) move off fan → closes ~200ms; hover different card → switches
  instantly; (3) close all browsers → dock empty, NO placeholder text, fallback buttons still show, dock strip
  still reserved. Debt still open: [F-02 activate-com-hang] (see Deferred debt). Throwaway spike at
  scratchpad/spike_activate.{cpp,exe} (SPIKE A/B confirmed) can be deleted.
- 2026-07-04 — Interactive-fan Step 4 done → fan now REACHABLE (hover-bridge). Was a real bug: dock
  WM_MOUSELEAVE→ClearHover hid the fan instantly, so the cursor crossing the card→fan seam killed it before
  a click (Step 3 unreachable without this). Fix: FanPopup grace timer (kGraceTimer, 200ms) + BeginGrace/
  CancelGrace. Fan WM_MOUSEMOVE→CancelGrace (+TME_LEAVE, m_fanTracking-guarded); fan WM_MOUSELEAVE→BeginGrace;
  fan WM_TIMER→Hide. Dock WM_MOUSEMOVE: CancelGrace over a card, instant ShowFanFor on card-switch when fan
  already visible, BeginHoverGrace over empty dock; WM_MOUSELEAVE→BeginHoverGrace (defer, not hide). Grace
  fires only when cursor is on neither window (each window's mouse-move cancels it) → close. Timer killed on
  Hide/Destroy/WM_TIMER (all teardown paths). Burst (threading + AppBar-timer-teardown + DPI) → adjudicator
  MAY PROCEED (only nit: benign redundant KillTimer, idempotent-teardown, left as-is). Simplifier extracted
  BeginHoverGrace() helper (symmetric to ClearHover). Build clean. FEATURE NOW FULLY INTERACTIVE.
  ⇒ VISUAL CHECK PENDING on Win11 (Steps 3+4 together): minimize a browser → hover its card → fan shows tabs
  → slide up into the fan (must NOT vanish) → click a tab → window restores + that tab activates; also move
  off → fan closes after ~200ms; hover a different card → fan switches instantly. Remaining: Step 5 empty-state
  (paint-only, no AppBar churn; lenses DPI/paint + AppBar-assert-no-setpos).
- 2026-07-04 — Interactive-fan Step 3 done → feature now END-TO-END (hover card → fan → click row → tab
  activates). FanPopup: Create(+ownerHwnd,+activateMsg), Show(+targetHwnd), RowAt hit-test (shares Paint's
  ScalePx(6/24) geometry; "+N more" row → -1), WM_LBUTTONDOWN → PostMessageW(kFanActivateMsg, target, idx)
  when idx>=0; MA_NOACTIVATE kept. Dock passes hwnd+kFanActivateMsg to Create, card hwnd to Show. Displayed
  row == original Store tab index (tabs shown contiguous from front) so idx indexes Store's full vector
  directly. Burst (DPI + threading) BOTH CLEAN → adjudicator MAY PROCEED (m_targetHwnd null-guard ruled
  unnecessary: idx>=0 ⟹ m_tabs non-empty ⟹ Show ran ⟹ target set; dock also re-guards via Store). Simplifier:
  no churn. Build clean. VISUAL CHECK PENDING on Win11: hover a minimized browser's card → fan shows tabs →
  click a tab → that window restores & the clicked tab activates. Remaining: Step 4 (hover-bridge grace so
  the fan survives the card→fan gap; lenses threading/DPI/AppBar-timer) + Step 5 (empty-state paint-only;
  lenses DPI/AppBar-assert-no-setpos).
- 2026-07-04 — Interactive-fan Step 2 done + SPIKE A/B CONFIRMED on Win11. Spike run: SetForegroundWindow
  ret=1 no ASFW → tab switched (R1 recipe good; GetForegroundWindow!=target right-after is just async
  restore, harmless since ret=1 skips flash); Select hr=0 confirmed IsSelected=1; tab-tree ready ~330ms
  (single UIA walk latency, NOT retries) → 2.3 constants UNCHANGED (50ms poll / 3s ceilings / 60ms settle).
  ~560ms total = perceivable lag (window-forward→330ms re-walk→confirm); acceptable v1, tree-walk is the
  future optimization target. Step 2 code: DockWindow kFanActivateMsg=WM_APP+7 handler — resolve wanted title
  from Store (pre-restore) → RestoreWindow (restore+SFW FIRST) → RequestActivate(target,title,idx) → fan
  Hide() (close-on-click, user UX). Burst (threading + AppBar-hygiene) BOTH CLEAN → adjudicator MAY PROCEED.
  Folded the one debt it surfaced: IsWindow(hwnd) guard on ActivateTab gate-1 so a click racing a close
  fails fast instead of spinning 3s. Build clean. Next: Step 3 — FanPopup RowAt + row→original-index map +
  POST kFanActivateMsg on WM_LBUTTONDOWN (the wiring that actually fires the handler); lenses DPI + threading.
- 2026-07-04 — Interactive-fan FEATURE Step 1 done (TabReader Activate path). Built combined throwaway
  SPIKE (scratchpad/spike_activate.exe, VS2026 vcvars) reproducing the full flow (NOACTIVATE fan click →
  UI SetForegroundWindow [SPIKE A] → worker UIA re-walk w/ readiness+tree retry → Select → confirm [SPIKE
  B]) — ready to run, PENDING USER on a live browser to confirm R1 recipe + set 2.3 constants. Step 1 real
  code: TabReader.h ActivateOutcome/TabActivateResult/Request(ReqKind Snapshot|Activate), ctor +activateMsg;
  TabReader.cpp CancelableSleep + FindLiveTabItems (keeps live element array, index-parity w/ Tab vec) +
  IsItemSelected + ActivateTab (readiness gate → tree gate → title-first match w/ fallbackIndex tiebreak →
  Select → confirm-via-reread → SetFocus→LegacyIAccessible fallback, NOT Invoke); WorkerLoop Snapshot/Activate
  dispatch; RequestActivate (no de-dupe). DockWindow: kTabActivateResultMsg=WM_APP+8 handler (_DEBUG log +
  UI-thread Store refresh from freshTabs + delete). Burst (threading + COM/resource) → adjudicator BLOCKED
  F-01 (poll sleeps ignored m_stop → join could freeze UI ~6s) → fixed (CancelableSleep threads m_stop into
  every sleep) → re-burst threading CLEAN → MAY PROCEED. F-02 (unbounded COM into hung provider) tracked as
  debt. Simplifier: reused IsItemSelected, markSelected lambda, dropped matchCount. Build clean (shell+profiler).
  Next: user runs spike + reports numbers; then Step 2 (dock kFanActivateMsg + restore-first) — kFanActivateMsg
  reserved WM_APP+7. kFanActivateMsg NOT yet added (step 2).
- 2026-07-04 — Feature DESIGN (parallel thread): `docs/plans/feature-interactive-fan.md` — interactive fan
  (click a fan row → restore+foreground→worker re-snapshot→title-match→SelectionItemPattern.Select) +
  empty-state "no cards, keep fallback buttons" (option a, no AppBar churn). Fable steered (UIA Select not
  synthetic input; staleness = re-snapshot at click, never wrong tab; hover-bridge union+grace; empty-state
  paint-only). win32-scout resolved R1 (NOACTIVATE click → SetForegroundWindow works, no ASFW; MA_NOACTIVATE
  mandatory) + R2 (SelectionItemPattern supported; gate on IsWindowVisible&&!IsIconic + TabControl retry, no
  fixed sleep, Select S_OK-silent-fails early; fallback SetFocus→LegacyIAccessible, NOT Invoke). Design only,
  no src/ changes. Open UX call: close fan on click vs on confirm. Ready to implement (spikes→5 steps).
- 2026-07-04 — Profiler consumer P.2–P.4 built (separate `shell_profiler` target under `profiler/`,
  own CMakeLists, zero shared shell code — hard rule 8). EtwSession: name-derived provider GUID
  (SHA-1/EventSource algo, runtime — no hardcoded GUID; verified `{C943A625-2D01-532A-B9E9-19613974D9AD}`
  == .NET reference), StartTraceW real-time + stale-session recovery (ERROR_ALREADY_EXISTS→STOP→retry),
  EnableTraceEx2, OpenTraceW/ProcessTrace on consumer thread, TDH decode via TdhFormatProperty; Stop()
  never leaks the session (CloseTrace+ControlTrace STOP, idempotent, dtor-guarded). MetricsView:
  per-event count/rate/p50/p95/max(duration_us) + shell CPU%/WS/handle sampling; `--csv` per-interval.
  main: `--raw|--csv|--image|--provider`, Ctrl+C clean stop. Both targets build green (VS2022 Debug);
  shell target unaffected. Wrote `docs/profiler-project-structure.md` (one repo/two targets rationale).
  P.1 (shell-side Trace.h + call sites) NOT done — stayed out of src/ per task; shell emits no events yet,
  so live capture + §12 row P are pending P.1 + elevated run. profiler/README.md documents elevation.
- 2026-07-04 — 5b debt polish (5b.4). (A) MeasureGap gap-left by geometry (leftKnown + chevron extension)
  → overflow chevron can't be overlapped; Widgets by aid (>=, rect-fail→kInvalid). (C) pill corner radius
  (RoundRect = ellipse diameter). (D) killed startup/reload double-frame: dock defers strip until first
  host verdict (m_gapResolved) + overlay posts on first measure (m_statePosted) + Create one-shot backstop
  timer. (B) tried AppBarRemove-first, REVERTED — ABM_REMOVE is itself a blocking SendMessage so it can't
  outrun a hung explorer (inherent, no app fix). 2 burst rounds (AppBar/threading/visual-geom): r1 MAY
  PROCEED w/ warnings→applied; r2 fixed Widgets-rect-fail overlap + dead push → final MAY PROCEED, no
  residual debt. Simplifier: pending. Build clean. Stage 5 fully done incl. debt.
- 2026-07-04 — Stage 5 ACCEPTED on Win11: all 5 5b visual checks pass (pills in gap; open apps → gap
  shrinks/pills drop; close → grow back; gap fail → dock-strip fallback; empty-gap right-click → taskbar
  menu; config edit → live pill update). All 5 stages now accepted. Next: profiler or deployment workstream.
- 2026-07-04 — 5b.3 done → Phase 5b CODE COMPLETE. Event-driven re-measure + single-host fallback. Dropped
  500ms poll: explorer-PID-scoped EVENT_OBJECT_LOCATIONCHANGE hook (OBJID_WINDOW|CLIENT) → kRemeasureMsg →
  200ms one-shot debounce → RequestMeasure; also WM_POWERBROADCAST resume (500ms delayed). Overlay posts
  kGapStateMsg(1/0) to dock on host flip (ApplyGap); DockButtons()=m_gapActive?none:launcher.Buttons() gates
  dock Paint+ButtonAt → single host (gap active hides dock strip; measure fail → dock fallback). Overlay
  Create+(dockHwnd,stateMsg); config reload→RequestMeasure (re-eval fit); Refresh() removed; WorkerLoop
  split-try posts invalid gap on any failure/throw. Bursts (threading/AppBar/fallback): AppBar clean; r1
  BLOCKED F-02 (reload didn't re-eval fit) → fixed + F-03 (invalid-gap-on-throw) + T-1 (LOCATIONCHANGE obj
  filter); r2 re-burst → MAY PROCEED. Simplifier: named kResumeRemeasureMs. Build clean. Debt: host-handoff
  1-frame both-show at startup/reload (transient). Visual check pending. Next: user check 5b + §12 rows.
- 2026-07-04 — 5b.1 ACCEPTED on Win11 (green outline hugs [Postman,Widgets]); 5b.2 buttons-in-gap code done,
  MAY PROCEED. Overlay now hosts the automation pills: dropped WS_EX_TRANSPARENT, WM_NCHITTEST→HTCLIENT on
  pill / HTTRANSPARENT elsewhere (empty-gap + right-click menu still reach taskbar), WM_LBUTTONUP→Launcher::
  Execute. Renderer: DrawButton promoted public + new GapButtonLayout (left-anchored, drops overflow =
  auto-downsize). ButtonAt hit-tests same layout Paint draws. ApplyGap shows only when ≥1 pill fits. Config
  reload→Refresh. Overlay holds const Launcher* (UI thread; worker never touches it). Burst (threading/DPI/
  visual)→MAY PROCEED; applied T3 (m_taskbarOverlay declared after m_launcher → destructs first) + V2 (hide
  when no pill fits). Simplifier: pending. Build clean. Visual pending. Both dock+gap show buttons till 5b.3.
- 2026-07-04 — Phase 5b started; 5b.1 gap-measurement + debug outline code done, MAY PROCEED. First tried a
  pure-HWND path but USER SCREENSHOT showed the outline far too wide. Live-probed the taskbar: on Win11 the
  MSTaskSwWClass HWND rect is a legacy STUB (45..577) that doesn't match the XAML layout (app icons render
  to ~1418). PIVOTED to UIA. `TaskbarOverlayWindow.{h,cpp}`: MeasureGap = ElementFromHandle(Shell_TrayWnd) →
  find TaskbarFrame → walk children: gap.left = max right over task buttons (Taskbar.TaskListButtonAutomation
  Peer + Start/Search/TaskView), gap.right = WidgetsButton left (when in-gap) else TrayNotifyWnd left. Win10
  fallback = legacy MSTaskListWClass/TrayNotifyWnd HWND path. UIA is blocking → WORKER thread (mirrors
  TabReader): CoInit MTA + CLSID_CUIAutomation, posts heap Gap* → UI thread SetWindowPos's a click-through
  layered outline (WS_EX_TRANSPARENT|LAYERED, LWA_COLORKEY, green frame). FindTaskbar verifies explorer.exe
  owner. Guards→kInvalid: auto-hide, null tray, GetDpiForWindow(tray)==0, no-task-button. DockWindow owns it;
  RequestMeasure (signals worker) on Create + 500ms kOverlayTimer + ABN_POSCHANGED/DISPLAYCHANGE/DPICHANGED;
  WM_DESTROY joins worker before AppBarRemove + WM_ENDSESSION KillTimer. No AppBar → no ABM_REMOVE. Two burst
  rounds (AppBar/threading/DPI/visual): r1 fixed CoUninitialize-before-ComPtr-release (BLOCKING) + null-tray/
  dpi-0/no-button guards + CoInit-HRESULT balance; r2 re-burst (threading+visual) clean, [1418,1948] traced.
  Simplifier: no churn. Live UIA layout saved to research doc. Build clean. Visual check pending on Windows.
  Debt→5b.3: overflow-chevron class (live), worker-join vs hung explorer at shutdown, one-Gap shutdown leak.
- 2026-07-04 — Stage 5a.4 done + Phase 5a complete: config hot-reload. ConfigWatcher.{h,cpp} worker
  (overlapped ReadDirectoryChangesW + stop-event; pending-flag drain teardown so no break path deadlocks
  or leaks). DockWindow: kConfigChangedMsg → 300ms kConfigTimer debounce → Launcher.Load()+repaint;
  Create makes dir + starts watcher; WM_DESTROY joins before AppBarRemove. Launcher split into
  ConfigDir/ConfigFileName/ConfigPath. Burst (threading+AppBar+resource): fixed BLOCKING undrained
  overlapped-IO teardown (CancelIo→GetOverlappedResult(TRUE), guarded by pending) → re-burst →
  MAY PROCEED. Also this session: button pills halved (user), Search→Gmail config. Next: Phase 5b overlay.
- 2026-07-04 — Stage 5a.3 done (awaiting user visual OK): button strip. User chose right-column overlay
  (pills top-right over cards). Renderer::ButtonLayout (single paint+hit-test source) + DrawButton
  (RoundRect light kButtonBg pill, radius clamp, ellipsized label). Paint draws buttons last. DockWindow
  ButtonAt + WM_LBUTTONUP button-first→card routing + WM_MOUSEMOVE fan-suppress; dropped 5a.2 debug
  middle-click. Burst (DPI/visual/GDI+hit-test): fixed BLOCKING pill invisibility over dark cards
  (→light kButtonBg+dark text), unscaled pen, unclamped radius → re-burst → MAY PROCEED. Icon-image
  render deferred (icon style → labeled pill). Simplifier: no churn. Build clean. Next: 5a.4 hot-reload.
- 2026-07-04 — Stage 5a.2 done: Launcher::Execute — detached MTA worker (pump-less fire-and-forget;
  STA would need a pump) → ShellExecuteW (url/shortcut) / CreateProcessW (command, handles closed);
  CoUninitialize only on SUCCEEDED(CoInitializeEx). #ifdef _DEBUG WM_MBUTTONUP cycles buttons as the
  trigger. Two bursts (threading + resource): fixed unconditional-CoUninitialize (BLOCKING), STA→MTA,
  ConfigPath free-of-garbage guard, MBTWC guard → re-burst clean → MAY PROCEED. Simplifier: no churn.
  Build clean (had to nuke a VS2019-reverted CMake cache + regen VS2022 mid-step). Next: 5a.3 (visual).
- 2026-07-04 — Stage 5a.1 done: Launcher.{h,cpp} line-based config load (config.txt,
  style|label|action|target|iconPath, BOM/UTF-8 decode, malformed→skip+debug, missing→zero). Chose
  line-based over JSON per hard rule 2. Loaded in DockWindow::Create. Two bursts (parsing + resource
  hygiene): fixed swprintf_s process-kill (→_snwprintf_s/_TRUNCATE), misaligned-wchar_t UB (→memcpy),
  unchecked MBTWC guard, dropped redundant link pragma → re-burst clean → MAY PROCEED. Simplifier
  folded a DebugPrintf helper. Build clean. Next: 5a.2 actions.
- 2026-07-04 — Stage 4 refinement (4.5b): real vertical window stacking. CardLayout stacks full-width
  bands top→bottom (was side-by-side); dock height now DYNAMIC (DockHeightPx: one kBandHeightDip=34
  band per minimized window + pad, clamped 1..kMaxBands=4), AppBarSetPos re-negotiated on minimize
  events + validation timer. Colors: kBgColor #00A2ED, kChipActiveBg #f87e73, kTextActive dark,
  new kTextOnBg for empty-state. Two inspector bursts: first flagged 4 blockers (band inversion N≥4,
  empty-state text invisible on blue, WM_DPICHANGED no-invalidate stale paint, AppBarSetPos-after-
  ABM_REMOVE on ENDSESSION drain) + 2 suspicious (chip-text inversion, WM_DISPLAYCHANGE invalidate).
  All fixed (dynamic height, kTextOnBg, InvalidateRect ×2, m_appBarRegistered guard, txt-rect guard);
  re-burst (AppBar/DPI/visual) clean → adjudicator MAY PROCEED. Simplifier: comment fixes only. Build
  clean. Runtime/visual check pending on Windows. Next: Stage 5a.1.
- 2026-07-04 — Step 4.5 (snapshot debounce) + 4.5a (drop card header) done in one run. Debounce:
  RequestSnapshotDebounced coalesces MINIMIZESTART/NAMECHANGE bursts into one 150ms-quiet flush to
  TabReader; FOREGROUND pre-warm stays immediate (must beat UIA strip); store/paint stay immediate;
  KillTimer(kSnapshotTimer) in WM_DESTROY. 4.5a: removed per-card title + "N tabs" header (echoed
  active tab → no info); chips now fill full card height. Inspector burst (threading/AppBar/DPI clean;
  visual F1 tall-slab font = user taste call) → adjudicator → MAY PROCEED. Simplifier: no churn. Build
  clean. Runtime/visual check pending on Windows. Stage 4 code complete; §12 row 4 acceptance next.
- 2026-07-03 — Step 4.4 done: click-to-restore. WM_LBUTTONUP → CardAt hit-test (shared Renderer::
  CardLayout, client coords) → RestoreWindow. Card removal driven solely by EVENT_SYSTEM_MINIMIZEEND
  (Renderer filters minimized-only), no optimistic store write. Inspector burst (threading, AppBar,
  click-restore correctness) → adjudicator → BLOCKED (rule-5 pump-block on hung target). Fixed:
  ShowWindow→ShowWindowAsync (M1), dropped undocumented SwitchToThisWindow for FlashWindowEx fallback
  (M2, also clears rule-6), event-driven removal (M3, no orphaned card). Re-burst (threading+AppBar) →
  re-adjudicate → MAY PROCEED (SetForegroundWindow ruled non-blocking). Simplifier extracted ClearHover
  helper. Build clean. Runtime/visual check pending with user. Next: 4.5 snapshot debounce.
- 2026-07-03 — fanpopup: dropped window-title header (duplicated active-tab row); fan opens straight
  into the tab list. User-requested tweak. Build clean.
- 2026-07-03 — Step 4.3 done: per-window hover-fan. FanPopup.{h,cpp} (transient WS_POPUP, NOACTIVATE,
  grows upward from strip top, monitor-clamped, DPI-scaled rows + "+N more" overflow). PaintUtil.h
  extracts shared palette/ScalePx/MakeFont. Renderer exposes CardLayout (single source of card rects,
  shared with hit-test). DockWindow owns FanPopup: TrackMouseEvent(TME_LEAVE) + 250ms kHoverTimer
  hover-intent, ShowFanFor maps card→screen anchor, WM_MOUSELEAVE/WM_DESTROY tear down. Inspector burst
  (AppBar clean, threading clean, DPI 2 deferred, visual/layout V1/V2/V3) → adjudicator → MAY PROCEED.
  Applied F-01 top-clamp (height≤avail), F-02 empty-tabs guard (Show→Hide), F-03 +N padding align.
  Simplifier: no churn. Build clean. Runtime/visual check pending with user. Next: 4.4 click-restore.
- 2026-07-03 — Step 4.2a done: TabReader caches UIA_SelectionItemIsSelectedPropertyId → Tab.active
  (clamped to one active tab max at source); Renderer promotes active chip to front + highlights it
  (kChipActiveBg/kTextActive). Inspector burst (visual/layout, DPI, threading) → adjudicator →
  MAY PROCEED (F-01 split-highlight fixed at source in TabReader; F-02 unconditional VariantClear
  applied; F-03 weak active-chip contrast = user visual call). Build clean. Runtime/visual check on
  active-tab highlight pending with user. Next: 4.3 hover-fan.
- 2026-07-02 — Docs bootstrapped: architecture (stages 1–5 + observability),
  per-stage plans, CLAUDE.md rules, this handoff. No code yet. Next: 1.1.
- 2026-07-02 — Step 1.1 done: CMakeLists.txt, stub `wWinMain`, .gitignore.
  Built + ran on Win11 (exit 0) via VS2019 BuildTools + NMake (VS2022 C++
  install broken on this machine — see build note above). 4-reviewer
  adversarial burst + verifier: PASS, no code findings; DPI items deferred
  to 1.2. Next: 1.2.
- 2026-07-03 — Step 1.2 done: PMv2 DPI call (abort on FALSE), SDK-version
  pins in CMake, DockWindow (WS_POPUP, TOOLWINDOW|TOPMOST|NOACTIVATE),
  message loop, right-click quit. Built + checkpoint-verified on Win11
  (window visible, PMv2 confirmed, not in taskbar/Alt-Tab by style, exit 0)
  via VS2022 (install fixed itself — build note updated). 4-reviewer burst +
  Opus verifier: 2 fixes applied (WS_EX_NOACTIVATE, rationale comments),
  5 deferred as carry-overs above, 5 rejected. 3 research agents' Win32
  edge-case findings saved to docs/research/win32-edge-cases.md. Next: 1.3.
- 2026-07-03 — Step 1.5 done: AppBarSetPos() — MonitorFromWindow+GetMonitorInfoW
  (rcMonitor), MulDiv(64,dpi,96) dock height, ABM_QUERYPOS→re-anchor→ABM_SETPOS→
  SetWindowPos. kCallbackMsg/WM_DISPLAYCHANGE/WM_DPICHANGED all call AppBarSetPos.
  ABM_NEW moved before ShowWindow so position negotiated before first paint.
  Inspector burst (AppBar-hygiene clean, DPI 2 suspicious dismissed, threading
  2 suspicious dismissed) → adjudicator → MAY PROCEED. Checkpoint on Windows:
  dock flush above taskbar full-width ✅, Notepad maximize stops at dock edge ✅,
  exit→maximize→no gap ✅. Next: 1.6.
- 2026-07-03 — Stage 1 ACCEPTED on Win11: dock visible ✅, Notepad stops at dock edge ✅,
  exit releases strip ✅, ABN_POSCHANGED (auto-hide toggle) renegotiates ✅ (taskbar-to-left
  not testable on Win11 — taskbar locked to bottom), second instance exits immediately ✅,
  fullscreen video dock steps aside ✅. Stage 2 + profiler workstream unlocked.
- 2026-07-03 — Step 1.7 done: single-instance named mutex (CreateMutexW/ERROR_ALREADY_EXISTS),
  WM_QUERYENDSESSION→TRUE + WM_ENDSESSION(wParam!=0)→AppBarRemove+PostQuitMessage,
  SetUnhandledExceptionFilter crash filter (best-effort ABM_REMOVE, EXCEPTION_CONTINUE_SEARCH).
  Inspector burst (AppBar-hygiene: narrow-gap A1 dismissed as best-effort design contract;
  threading: F-01 crash-filter-off-UI-thread deferred to Stage 2 first worker step) →
  adjudicator → MAY PROCEED. Build clean; Stage 1 acceptance row (§12) pending on Windows.
- 2026-07-03 — Step 1.6 done: ABN_FULLSCREENAPP handler (SetWindowPos HWND_BOTTOM/HWND_TOPMOST on
  lparam toggle) + GetDpiForWindow==0 guards in AppBarSetPos and WM_PAINT. Inspector burst
  (AppBar-hygiene clean, threading clean, DPI: F-01 pre-existing deferred, F-02/F-03 nits) →
  adjudicator → MAY PROCEED. Build verified; runtime checkpoint (taskbar-move, DPI-change,
  fullscreen-video) pending on Windows. Next: 1.7.
- 2026-07-03 — Step 1.4 done: ABM_NEW after ShowWindow (kCallbackMsg=WM_APP+1),
  AppBarRemove() helper with m_appBarRegistered guard, ABM_REMOVE in WM_DESTROY
  before PostQuitMessage, ~DockWindow covers GetMessage==-1 path, WS_EX_TOPMOST
  kept. Inspector burst (AppBar-hygiene, threading) → adjudicator → MAY PROCEED
  (all findings informational). ABM_NEW confirmed working (window visible); gap/
  kill checkpoint tests not formally observed (user moved on; Task Manager kill
  behavior deferred to 1.7 doc). Debug rect adjusted to (160,1645) for testing.
  Next: 1.5.
- 2026-07-03 — Stage 2 ACCEPTED on Win11: indicator shows Edge title + "(+1)" for Chrome,
  "browser: none" when closed, live tracking works within ~1s, CPU ~0% idle.
- 2026-07-03 — Step 2.4 done: debounce via m_pendingValidation + 200ms SetTimer. kWindowEventMsg
  deduplicates HWND into pending list + restarts timer. WM_TIMER drains list, validates each
  HWND with IsBrowserFrame, updates m_browsers, repaints. KillTimer in WM_DESTROY. --scan
  already #ifdef _DEBUG. Inspector burst (AppBar: clean; threading: F-T1 unbounded-pending
  informational/no-fix; F-T2 dismissed) → adjudicator → MAY PROCEED. Stage 2 code complete;
  §12 row 2 runtime acceptance pending on Windows. Next: Stage 3.
- 2026-07-03 — Step 2.3 done: WinEventHook (EVENT_OBJECT_CREATE..HIDE, OUTOFCONTEXT,
  system-wide). WinEventProc pre-filters idObject==OBJID_WINDOW then PostMessage.
  kWindowEventMsg handler: IsBrowserFrame re-validate, add/remove m_browsers,
  InvalidateRect. Unhook in WM_DESTROY before AppBarRemove. F-01 fix: g_dockHwnd
  std::atomic<HWND> + exchange(nullptr) in CrashFilter + clear after message loop.
  Inspector burst → adjudicator → MAY PROCEED (F-T1 class-gated pump call accepted;
  F-A1/A2 infos applied). Build/runtime pending. Next: 2.4.
- 2026-07-03 — Step 2.2 done: Renderer.{h,cpp} with Renderer::Paint (dark bg + DPI-scaled
  indicator: "browser: none" vs "browser: <title> (+N)"). ScanBrowserFrames() called in
  Create() before ShowWindow to populate m_browsers. EnumProc made noexcept+try/catch
  (F-A1 warning fix). Inspector burst (AppBar: F-A1 fixed; threading: F-T1/F-T2 dismissed) →
  adjudicator → MAY PROCEED. Build/runtime pending on Windows. Next: 2.3.
- 2026-07-03 — Step 2.1 done: WindowMonitor.{h,cpp} with IsBrowserFrame (Chrome_WidgetWin_1,
  visible, unowned, non-empty title, chrome.exe/msedge.exe via QueryFullProcessImageNameW) +
  ScanBrowserFrames. --scan debug flag in #ifdef _DEBUG. Inspector burst (AppBar: clean;
  threading: F-01 pre-existing deferred to 2.3, comment tightened) → adjudicator → MAY PROCEED.
  Build pending on Windows. Next: 2.2.
- 2026-07-03 — Fix: pre-warm UIA on EVENT_SYSTEM_FOREGROUND (fourth hook). Tab tree stripped on minimized window → snapshot while still visible. Inspector burst (AppBar: F-A1/A2 dismissed nits; threading: F-T1 informational narrow-lock watch item) → adjudicator → MAY PROCEED. Build clean; runtime re-test needed.
- 2026-07-03 — Stage 4 direction set to variant D (side-by-side minimized-only cards + per-window
  hover-fan; drop many-window overflow — user has 2-3 windows × 10+ tabs). Plan rewritten in
  stage-4.md. Step 4.1 done (Paint shows only minimized windows; open windows hidden). Step 4.2
  done (N tabs badge + legible ~110px chips stretched to fill + guards for narrow cards/badge-fit).
  Inspector bursts (DPI + visual/layout) + adjudicate on each. Loop disabled per user; run
  autonomously (see memory). Next: 4.2a selected-tab state, then 4.3 hover-fan.
- 2026-07-03 — Stage 3 ACCEPTED on Win11: minimize browser → per-window card shows real tab
  titles. Root-caused the empty-tabs bug via a temp UIA tree dump (browser TabItems nest two
  panes deep under the "Tab bar" control → TreeScope_Children found nothing; fixed to
  TreeScope_Descendants). Also: /utf-8 flag fixed middle-dot corruption; EVENT_SYSTEM_FOREGROUND
  pre-warm snapshot; IsInsideDocument skips web-content tablists; CleanTabTitle strips status
  suffixes; per-tab chips fill full card width with +N overflow; stale marker dropped. Inspector
  bursts (DPI + visual/layout added when Renderer changes) + adjudicator on each. Stage 4 unlocked.
- 2026-07-03 — Step 3.5 done: staleness handling — TabSnapshot::failed flag (set when tabs.empty()); kTabSnapshotMsg branches: failure→MarkTabsStale (keeps prior tabs), success→SetTabs (clears stale); DrawCard appends "(stale)" to tab line. Inspector burst (AppBar: clean; threading: clean) → adjudicator → MAY PROCEED. Build clean; §12 row 3 runtime acceptance pending on Windows. Stage 3 code complete.
- 2026-07-03 — Step 3.4 done: Renderer draws tab card for minimized windows — title header (FW_SEMIBOLD 10pt) + tab line (FW_NORMAL 9pt, " · " separator, DT_END_ELLIPSIS) from Store::tabs; DrawCard helper; active-window path unchanged. Pre-existing TabReader ComPtr/.Get() compile bug fixed. Inspector burst (AppBar: clean; threading: clean; DPI: F-D1 nit fixed inline, F-D2 informational pre-existing) → adjudicator → MAY PROCEED. Build clean; §12 row 3 runtime acceptance pending on Windows. Next: 3.5.
- 2026-07-03 — Step 3.3 done: TabReader.{h,cpp} — worker thread (COINIT_MULTITHREADED), CoCreateInstance IUIAutomation, SnapshotTabs (ElementFromHandle→TabControl→TabItems→CachedName), PostMessage kTabSnapshotMsg heap payload. Third WinEvent hook (NAMECHANGE). RequestSnapshot on MINIMIZESTART + NAMECHANGE. kTabSnapshotMsg handler: SetTabs+delete+invalidate. Debug OutputDebugString dump. F-T1 fix: worker exits immediately on m_stop (no drain-then-stop). Inspector burst (AppBar: clean; threading: F-T2 MTA/UIA dismissed — MTA is correct; F-T1 fixed inline) → adjudicator → MAY PROCEED. Build clean; runtime checkpoint pending on Windows. Next: 3.4.
- 2026-07-03 — Step 3.2 done: second WinEvent hook (MINIMIZESTART..END); Store::SetMinimized; kWindowEventMsg fast-paths minimize events (bypass debounce); IsIconic check on initial scan; Renderer shows "<title> — minimized". Inspector burst (AppBar: F-A2 WM_ENDSESSION no-unhook — informational, ABM_REMOVE confirmed; threading: clean) → adjudicator → MAY PROCEED. Build clean; runtime checkpoint pending on Windows. Next: 3.3.
- 2026-07-03 — Step 3.1 done: Store.{h,cpp} (Tab/TrackedWindow structs, HWND-keyed map, UI-thread-only). Migrated DockWindow::m_browsers→m_store:Store; Renderer now takes const Store&. Simplifier extracted StoreWindow helper. Inspector burst (AppBar: F-A1 pre-existing double-PostQuitMessage — safe, no double ABM_REMOVE; threading: F-T1 comment nit, F-T2 ordering note — both pre-existing) → adjudicator → MAY PROCEED. Build clean; runtime checkpoint pending on Windows. Next: 3.2.
- 2026-07-03 — Step 1.3 done: WM_PAINT (dark fill + DPI-scaled Segoe UI 12pt
  via GetDpiForWindow+MulDiv), WM_ERASEBKGND→1, hbrBackground=nullptr,
  UpdateWindow after ShowWindow. Inspector burst (AppBar-hygiene, threading,
  DPI) → adjudicator → BLOCKED on default-font not DPI-scaled → fixed →
  re-burst → MAY PROCEED. Checkpoint: text crisp at 100%/150% — visual
  verification pending on Windows (hard rule 3). Checkpoint protocol added to
  CLAUDE.md. Next: 1.4.
