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
| Stage 2 — browser detection | ✅ Complete — all 4 steps done (§12 row 2 pending on Windows) |
| Stage 3 — single-window tabs | ⬜ blocked on Stage 2 |
| Stage 4 — multi-window stacks | ⬜ blocked on Stage 3 |
| Stage 5 — taskbar buttons | ⬜ blocked on Stage 4 |
| Profiler (parallel workstream) | ⬜ unlocked — see `docs/plans/profiler.md` |
| Deployment — permanent run ("service" goal) | ⬜ v1 (logon autostart) after Stage 1; v2 (watchdog service) after Stage 5 — see `ARCHITECTURE.md` §13 |

**Next action: Stage 3 — single-window tabs** (`docs/plans/stage-3.md`).
Stage 2 code complete; §12 row 2 acceptance pending on Windows before Stage 3 starts.

Deferred debt:
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
- 2026-07-03 — Step 1.3 done: WM_PAINT (dark fill + DPI-scaled Segoe UI 12pt
  via GetDpiForWindow+MulDiv), WM_ERASEBKGND→1, hbrBackground=nullptr,
  UpdateWindow after ShowWindow. Inspector burst (AppBar-hygiene, threading,
  DPI) → adjudicator → BLOCKED on default-font not DPI-scaled → fixed →
  re-burst → MAY PROCEED. Checkpoint: text crisp at 100%/150% — visual
  verification pending on Windows (hard rule 3). Checkpoint protocol added to
  CLAUDE.md. Next: 1.4.
