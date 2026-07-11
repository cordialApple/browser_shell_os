# Peekbar

A native Windows shell tool that keeps your browser's tabs visible when you
minimize the window.

Minimizing a browser window normally collapses it into a flat taskbar button,
and everything you were looking at disappears. `Peekbar` adds a
**dock strip in the taskbar's own empty gap area**, where minimized browser
windows leave their tabs behind as hoverable chips. Multiple minimized
windows stack up as staggered cards, plus one-click launcher buttons for
sites and shortcuts you use often.

> **Status:** working on Windows 11. Core dock, multi-window tab tracking,
> and taskbar launcher buttons are implemented and accepted end-to-end.
> Actively developed.

<p align="center">
  <img src="docs/media/hero.gif" alt="Minimized browser tabs shown as chips in the taskbar's empty gap" width="100%"><br>
  <em>Minimize a browser and its tabs stay glance-able as chips in the taskbar's own empty gap.</em>
</p>

<p align="center">
  <img src="docs/media/multi-window.gif" alt="Two minimized browser windows aggregated as staggered cards with per-tab chips" width="100%"><br>
  <em>Multiple minimized windows aggregate as staggered cards, each with per-tab chips and an overflow count.</em>
</p>

## Why

Browser tab hoarders lose track of what's in a minimized window. This tool
doesn't manage your tabs or replace your browser; it just keeps a glance-able
trace of them visible in space the taskbar already wastes.

## Is this safe to run?

It reads tab *titles* only (not full URLs), never injects into other
processes, and never talks to the network. Full breakdown: [`SECURITY.md`](SECURITY.md).

## Install

No prebuilt releases yet: build from source.

**Prerequisites:** Windows 10/11 x64, Visual Studio 2022 (Desktop development
with C++ workload) or Build Tools + CMake ≥ 3.20.

```
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
build\Release\peekbar.exe
```

The tool runs as a hidden coordinator window. It draws its dock strip in the
taskbar's gap area and exits cleanly when killed from Task Manager. There's no
installer and nothing is written outside the build folder; delete the `.exe`
to remove it.

## Stack

- **C++17 / Win32**: 
- **UI Automation** for reading tab titles (self-contained; no browser
  extension required).
- Per-monitor DPI aware.

## Roadmap

| Stage | Deliverable | Status |
|---|---|---|
| **1** | Hello-world shell tool: a dock anchored above the taskbar. | ✅ |
| **2** | Detect an open browser and perform a simple action in the dock. | ✅ |
| **3** | Track one browser's tabs; keep them on-screen after minimize. | ✅ |
| **4** | Track multiple browsers; aggregate as a staggered card stack. | ✅ |
| **5** | Taskbar launcher buttons for sites and shortcuts, persisted with the shell. | ✅ |
| **P** | `shell_profiler`, a separate, never-bundled ETW-based perf observability tool. | ✅ |

How it works, in depth: [`docs/`](docs/).

## Performance & observability

Tab-restore felt slow, so I instrumented the path with a separate ETW-based
profiler (`shell_profiler`) and used a git-diffable Power BI dashboard to guide
the work. `restore→tab-found` was the dominant cost. Guided passes took the
path from 602 ms to 271 ms across 5 capture runs (102 clicks). Details:
[`docs/dashboard/`](docs/dashboard/) and [`profiler/`](profiler/).

<p align="center">
  <img src="docs/dashboard/img/stage-bottlenecks.png" width="800" alt="Per-stage latency breakdown: restore-to-tab-found tops at 306.5 ms across 5 capture runs and 102 clicks"><br>
  <em>Per-stage latency breakdown. <code>restore&rarr;tab-found</code> dominates. Full dashboard in <a href="docs/dashboard/">docs/dashboard/</a>.</em>
</p>
