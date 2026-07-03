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
| Stage 1 — AppBar dock | 🔶 In progress — 1.1 ✅ 1.2 ✅ 1.3 ✅ 1.4 ✅, **NEXT → Step 1.5** (`docs/plans/stage-1.md`) |
| Stage 2 — browser detection | ⬜ blocked on Stage 1 |
| Stage 3 — single-window tabs | ⬜ blocked on Stage 2 |
| Stage 4 — multi-window stacks | ⬜ blocked on Stage 3 |
| Stage 5 — taskbar buttons | ⬜ blocked on Stage 4 |
| Profiler (parallel workstream) | ⬜ unlocks when Stage 1 accepted (`docs/plans/profiler.md`) |
| Deployment — permanent run ("service" goal) | ⬜ v1 (logon autostart) after Stage 1; v2 (watchdog service) after Stage 5 — see `ARCHITECTURE.md` §13 |

**Next action: Stage 1, Step 1.5 — Position negotiation (the reserved strip).** See
`docs/plans/stage-1.md`. Notes going into 1.5:
- Debug rect now at (160, 1645) — replace entirely with ABM_QUERYPOS/SETPOS logic.
- WS_EX_TOPMOST kept; Step 1.6 drops it for fullscreen apps only.
- Task Manager kill leaves dead reserved space (expected — motivates Step 1.7); not
  formally observed this session (user moved on), document in 1.7 checkpoint.
- Optional debt: guard `GetDpiForWindow==0` → default 96 before MulDiv.
- Edge-case research: `docs/research/win32-edge-cases.md`.

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
- 2026-07-03 — Step 1.4 done: ABM_NEW after ShowWindow (kCallbackMsg=WM_APP+1),
  AppBarRemove() helper with m_appBarRegistered guard, ABM_REMOVE in WM_DESTROY
  before PostQuitMessage, ~DockWindow covers GetMessage==-1 path, WS_EX_TOPMOST
  kept. Inspector burst (AppBar-hygiene, threading) → adjudicator → MAY PROCEED
  (all findings informational). ABM_NEW confirmed working (window visible); gap/
  kill checkpoint tests not formally observed (user moved on; Task Manager kill
  behavior deferred to 1.7 doc). Debug rect adjusted to (160,1645) for testing.
  Next: 1.5.
- 2026-07-03 — Step 1.3 done: WM_PAINT (dark fill + DPI-scaled Segoe UI 12pt
  via GetDpiForWindow+MulDiv), WM_ERASEBKGND→1, hbrBackground=nullptr,
  UpdateWindow after ShowWindow. Inspector burst (AppBar-hygiene, threading,
  DPI) → adjudicator → BLOCKED on default-font not DPI-scaled → fixed →
  re-burst → MAY PROCEED. Checkpoint: text crisp at 100%/150% — visual
  verification pending on Windows (hard rule 3). Checkpoint protocol added to
  CLAUDE.md. Next: 1.4.
