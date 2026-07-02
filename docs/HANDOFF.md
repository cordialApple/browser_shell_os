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
| Stage 1 — AppBar dock | ⬜ **NEXT → Step 1.1** (`docs/plans/stage-1.md`) |
| Stage 2 — browser detection | ⬜ blocked on Stage 1 |
| Stage 3 — single-window tabs | ⬜ blocked on Stage 2 |
| Stage 4 — multi-window stacks | ⬜ blocked on Stage 3 |
| Stage 5 — taskbar buttons | ⬜ blocked on Stage 4 |
| Profiler (parallel workstream) | ⬜ unlocks when Stage 1 accepted (`docs/plans/profiler.md`) |
| Deployment — permanent run ("service" goal) | ⬜ v1 (logon autostart) after Stage 1; v2 (watchdog service) after Stage 5 — see `ARCHITECTURE.md` §13 |

**Next action: Stage 1, Step 1.1 — project scaffolding** (CMakeLists.txt,
empty `wWinMain`, .gitignore). See `docs/plans/stage-1.md`.

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
