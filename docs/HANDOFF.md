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
| Stage 5 — taskbar buttons | 🟡 in progress — 5a.1–5a.3 built (dock-hosted buttons); 5a.3 awaiting user visual OK; next 5a.4 hot-reload |
| Profiler (parallel workstream) | ⬜ unlocked — see `docs/plans/profiler.md` |
| Deployment — permanent run ("service" goal) | ⬜ v1 (logon autostart) after Stage 1; v2 (watchdog service) after Stage 5 — see `ARCHITECTURE.md` §13 |

**Next action: user visual acceptance of 5a.3 button strip, then Stage 5a.4 — config hot-reload**
(ReadDirectoryChangesW on a worker → post to dock → reload+repaint; non-visual). See `stage-5.md`.
Done this session: Stage 4 complete incl. 4.5b vertical-stack (dynamic dock height, cap 4) + recolor
(bg #00A2ED, active #f87e73); Stage 5a.1 config load + 5a.2 actions + 5a.3 button strip (right-column
overlay pills, left-click → Launcher::Execute). Pending user runtime/visual check: dock grows as windows
minimize; colors; pill buttons top-right; click a pill opens its url/shortcut. Sample config.txt (2 url
buttons) already written to %LOCALAPPDATA%\browser_shell_os\.

Deferred debt:
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
