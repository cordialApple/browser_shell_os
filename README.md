# browser_shell_os

A native Windows shell tool that keeps your browser's tabs visible when you
minimize the window.

Today, minimizing a browser window collapses it into a flat taskbar button and
everything you were looking at disappears. This project builds a **dock strip
directly above the Windows taskbar** where minimized browser windows leave their
tab information behind — eventually as staggered, multilayered cards that
aggregate the tabs of multiple minimized windows.

## Stack

- **C++17 / Win32** — a real shell tool, no frameworks, no Electron, no web view.
  The dock registers as an application desktop toolbar (AppBar) via
  `SHAppBarMessage`, so it reserves its strip of screen at the shell level.
- **UI Automation** for reading tab titles (self-contained; no browser extension
  required to start). A browser extension + native messaging host is the
  documented upgrade path if exact URLs are needed later.
- Target: Windows 10/11 x64, per-monitor DPI aware.

## Roadmap (iterative, minimum functionality per stage)

| Stage | Deliverable |
|---|---|
| **1** | Hello-world shell tool: an AppBar dock anchored above the taskbar that reserves its own screen strip. *(The hardest stage — all native plumbing lives here.)* |
| **2** | Detect that a browser is open and perform a simple action in the dock. |
| **3** | Track one browser's tabs; keep them on-screen in the dock after the window is minimized. |
| **4** | Track multiple browsers' tabs; aggregate minimized windows as a staggered card stack. |
| **5** | Automation buttons in the taskbar's empty space: pill- or icon-style launchers for a website, a designated website, or a bundled automation shortcut — persisted with the shell. |

Full technical detail, per-stage APIs, acceptance criteria, and risks:
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

Stage 1 is broken down into ordered implementation steps in
[`CLAUDE.md`](CLAUDE.md), which also carries the working rules for AI-assisted
development on this repo.

## Building

Implementation begins with Stage 1. Prerequisites (when code lands):

- Windows 10 or 11, x64
- Visual Studio 2022 (Desktop development with C++) or Build Tools + CMake ≥ 3.20

```
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

Note: this is Windows-native code; it does not build or run on Linux/macOS.
