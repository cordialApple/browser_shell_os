# CLAUDE.md — working rules for this repo

This file steers AI-assisted development on `browser_shell_os`. It is
deliberately short so it can sit in context at all times.

## Session start protocol

1. Read `docs/HANDOFF.md` — current state, what's done, what's next.
2. Read the plan for the current stage: `docs/plans/stage-N.md`.
3. Skim `docs/ARCHITECTURE.md` sections relevant to that stage only.
4. Work **one step at a time**; a step is done when its checkpoint passes.
5. After completing a step: update the status board and session log in
   `docs/HANDOFF.md`, commit with a descriptive message.

Do not read every doc into context up front — that is what causes drift. The
handoff doc tells you exactly what you need for the current task.

## What this project is

A native **C++17 / Win32** Windows shell tool: a hidden coordinator window
(`HostWindow`) that keeps minimized browser windows' tab info visible as
chips in the taskbar's own empty gap area, aggregating multiple windows as
staggered card stacks and adding automation buttons in that same space. Five
stages plus a decoupled profiler workstream, defined in
`docs/ARCHITECTURE.md`; step-by-step plans in `docs/plans/`.

## Hard rules — do not drift

1. **Stay in the current step.** Implement only the step you were asked for.
   Do not scaffold future stages' components "while you're at it."
2. **Win32 only.** No Electron, no web views, no UI frameworks (Qt/WinUI/WPF),
   no third-party dependencies without explicit approval. C++17, MSVC, CMake.
3. **This code builds and runs only on Windows.** If developing from a
   non-Windows environment: write the code, keep it correct against the
   documented APIs, and state plainly that build/run verification is pending
   on Windows. Never claim runtime verification you didn't do.
4. **No AppBar registered; `ABM_GETSTATE` query-only.** Chip-rework Stage 3
   removed the AppBar dock entirely — the only `SHAppBarMessage` call left in
   `src/` is a read-only `ABM_GETSTATE` in `TaskbarOverlayWindow::IsAutoHide`.
   There is nothing to deregister, so this rule is vacuous today. If any
   future work re-registers an AppBar, the old obligation applies again:
   every exit path must `ABM_REMOVE`, or Windows leaves dead reserved screen
   space until explorer restarts.
5. **One UI thread.** The dock's message loop thread owns all windows and is
   the sole `Store` writer. Blocking work (UIA, enumeration) goes on workers
   that communicate via `PostMessage` only. `SetWinEventHook` callbacks do
   the minimum and post.
6. **Isolate fragile heuristics.** Anything depending on undocumented
   browser/explorer internals (UIA tree shapes, window-class filtering,
   taskbar geometry) lives in exactly one file (`TabReader`,
   `TaskbarOverlayWindow`) so breakage from a Windows/browser update is a
   one-file fix.
7. **Wide APIs everywhere.** `W`-suffixed Win32 functions, `std::wstring`,
   `UNICODE`/`_UNICODE` defined. No ANSI variants.
8. **No bloat in the shell; profiler stays separate.** Shell-side telemetry
   is TraceLogging (ETW) only — no logging frameworks, no log files, no
   telemetry threads. The `shell_profiler` tool under `profiler/` is a
   separate executable and build target: never link it, bundle it, or share
   code with it. The only contract is the ETW provider/event names in
   `docs/ARCHITECTURE.md` §10.

## Checkpoint protocol

After implementing a step, gate it through this burst before declaring the checkpoint passed:

1. **Inspector burst** — launch one inspector agent per lens the step touches, in parallel. Minimum lenses every step: AppBar-hygiene exit paths (every step that touches lifetime code), threading-rule violations (every step that adds async work). Add DPI correctness when the step touches painting or window sizing. Each inspector reports findings only — no verdicts.
2. **Adjudicate** — feed all inspector findings to the adjudicator. It deduplicates, dismisses false positives, and renders either `CHECKPOINT MAY PROCEED` or `CHECKPOINT BLOCKED` with a must-fix list.
3. **Fix and re-burst** — if `BLOCKED`, apply every must-fix, rebuild, re-run the affected inspector lens(es), re-adjudicate. Repeat until `MAY PROCEED`.
4. **Run checkpoint** — only after `MAY PROCEED`. Report actual results honestly; never claim runtime verification that wasn't done on Windows.

## Build & verify

```
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug
build\Debug\browser_shell_os.exe
```

Per-stage acceptance tests: `docs/ARCHITECTURE.md` §12. Run the relevant row
after every step and report results honestly.

## Where the plans live

| Doc | Purpose |
|---|---|
| `docs/HANDOFF.md` | Entry point every session: status board, next action, session log |
| `docs/ARCHITECTURE.md` | Source of truth for the design (components, stages, APIs, risks) |
| `docs/plans/stage-1.md` … `stage-5.md` | Ordered step breakdowns with checkpoints, one per stage |
| `docs/plans/profiler.md` | Observability workstream (starts after Stage 1 is accepted) |

Plans for later stages are drafts: before starting a stage, re-read its plan
against the then-current code and refine it if reality has diverged — then
follow it step by step.
