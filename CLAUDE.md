# CLAUDE.md — working rules for this repo

This file steers AI-assisted development on `browser_shell_os`. Read
`docs/ARCHITECTURE.md` first — it is the source of truth for the design; this
file is the source of truth for **how to work** and for the **Stage 1 step
breakdown**.

## What this project is

A native **C++17 / Win32** Windows shell tool: an AppBar dock above the taskbar
that keeps minimized browser windows' tab info visible, later aggregating
multiple windows as staggered card stacks and adding automation buttons in the
taskbar's empty space. Five stages, defined in `docs/ARCHITECTURE.md`.

## Hard rules — do not drift

1. **Stay in the current stage.** Implement only the step you were asked for.
   Do not scaffold future stages' components "while you're at it."
2. **Win32 only.** No Electron, no web views, no UI frameworks (Qt/WinUI/WPF),
   no third-party dependencies without explicit approval. C++17, MSVC, CMake.
3. **This code builds and runs only on Windows.** If developing from a
   non-Windows environment: write the code, keep it compiling in your head
   against the documented APIs, and state plainly that build/run verification
   is pending on Windows. Never claim runtime verification you didn't do.
4. **AppBar hygiene is non-negotiable.** Every code path that exits the
   process must deregister the AppBar (`ABM_REMOVE`). A missed removal leaves
   dead reserved screen space until explorer restarts. When touching lifetime
   code, re-check every exit path.
5. **One UI thread.** The dock's message loop thread owns all windows and is
   the sole `Store` writer. Blocking work (UIA, enumeration) goes on workers
   that communicate via `PostMessage` only. `SetWinEventHook` callbacks do the
   minimum and post.
6. **Isolate fragile heuristics.** Anything that depends on undocumented
   browser/explorer internals (UIA tree shapes, window-class filtering,
   taskbar geometry) lives in exactly one file (`TabReader`,
   `TaskbarOverlayWindow`) so breakage from a Windows/browser update is a
   one-file fix.
7. **Wide APIs everywhere.** `W`-suffixed Win32 functions, `std::wstring`,
   `UNICODE`/`_UNICODE` defined. No ANSI variants.

## Build & verify

```
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug
build\Debug\browser_shell_os.exe
```

Per-stage acceptance tests live in `docs/ARCHITECTURE.md` §11. Run the relevant
row after every step below and report results honestly.

---

# Stage 1 step breakdown — the AppBar dock

Stage 1 is the hardest stage: all the native shell plumbing lives here. It is
deliberately split into **seven small steps, each independently buildable and
testable**. Do them in order. Do not start a step until the previous one's
checkpoint passes. If a checkpoint fails, fix it before moving on — later steps
build on the invariants established earlier.

## Step 1.1 — Project scaffolding

**Build:** `CMakeLists.txt` (C++17, `WIN32` executable target, `UNICODE`
definitions, link `shell32 user32 gdi32`), `src/main.cpp` with a `wWinMain`
that does nothing but `return 0`, and `.gitignore` for `build/`.

**Checkpoint:** `cmake -B build` + `cmake --build build` succeed on Windows;
the exe runs and exits cleanly. Nothing visible yet — that's correct.

## Step 1.2 — DPI awareness + bare window + message loop

**Build:** In `wWinMain`, FIRST call
`SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`
(before any window/USER32 UI call — this cannot be set later). Create
`src/DockWindow.{h,cpp}`: `RegisterClassExW`, `CreateWindowExW` with
`WS_POPUP`, `WS_EX_TOOLWINDOW | WS_EX_TOPMOST`, a fixed debug rect (e.g.
800×64 centered), `ShowWindow`, and a standard
`GetMessage/TranslateMessage/DispatchMessage` loop. `WndProc` handles
`WM_DESTROY` → `PostQuitMessage(0)`. Add a way to quit for testing:
handle `WM_RBUTTONUP` → `DestroyWindow`.

**Checkpoint:** a blank borderless topmost rectangle appears; it does NOT
appear in Alt-Tab or the taskbar; right-click closes it; process exits with 0.

## Step 1.3 — Placeholder painting

**Build:** `WM_PAINT`: fill background (dark brush), draw centered text
"browser_shell_os dock" with GDI (`DrawTextW`). Handle `WM_ERASEBKGND`
(return 1, paint everything in `WM_PAINT`) to avoid flicker.

**Checkpoint:** text renders crisply at 100% and 150% display scaling (the
per-monitor-v2 context from 1.2 is what makes 150% crisp — if it's blurry,
1.2 is wrong).

## Step 1.4 — AppBar registration + removal (no positioning yet)

**Build:** Add to `DockWindow`: a private callback message
(`RegisterWindowMessageW(L"BrowserShellOsAppBar")` or `WM_APP + 1`), then
`SHAppBarMessage(ABM_NEW, &abd)` after window creation with
`abd.uCallbackMessage` set. On `WM_DESTROY`, call
`SHAppBarMessage(ABM_REMOVE, &abd)` **before** `PostQuitMessage`. Wrap
register/remove in a small RAII guard so early-return paths can't skip
removal. Keep the fixed debug rect — positioning is the next step.

**Checkpoint:** register succeeds (returns TRUE); exiting the app and then
maximizing another window shows NO reserved gap anywhere (removal worked).
Kill the process from Task Manager and confirm what happens — document the
observed behavior; this motivates step 1.7.

## Step 1.5 — Position negotiation: the reserved strip

The heart of Stage 1.

**Build:** Compute the desired rect: primary monitor's full width
(`MonitorFromPoint` + `GetMonitorInfoW`, use `rcMonitor` — the shell, not the
work area, arbitrates via QUERYPOS), height = dock height scaled by the
monitor DPI (`GetDpiForWindow`), anchored to the bottom edge
(`abd.uEdge = ABE_BOTTOM`). Then the canonical sequence:

1. `ABM_QUERYPOS` with the proposed rect — the shell shrinks/moves it so it
   doesn't overlap the taskbar or other appbars. **After QUERYPOS, re-anchor:**
   set `rc.top = rc.bottom - dockHeight` (the shell may have moved the bottom
   edge up to sit on top of the taskbar).
2. `ABM_SETPOS` with the adjusted rect — commits the reservation.
3. `SetWindowPos` to exactly that rect.

Mirror the flow in the official sample
(`Windows-classic-samples/.../appbar/AppBar.cpp`) — do not improvise the
sequence.

**Checkpoint:** dock sits flush on top of the taskbar, full width. Maximize
Notepad → it stops at the dock's top edge (the strip is genuinely reserved,
not just topmost). Exit → maximize again → Notepad reaches the taskbar.

## Step 1.6 — Reacting to shell changes: the AppBar callback + display events

**Build:** Handle the callback message registered in 1.4:

- `ABN_POSCHANGED` (taskbar moved/resized/auto-hide toggled, another appbar
  came or went) → re-run the 1.5 negotiation.
- `ABN_FULLSCREENAPP` → when a fullscreen app is active, drop `WS_EX_TOPMOST`
  (move to bottom) so games/video aren't overlapped; restore topmost when it
  ends.
- `ABN_STATECHANGE` → re-check taskbar state (`ABM_GETSTATE`) and renegotiate.

Also renegotiate on `WM_DISPLAYCHANGE` (resolution change) and
`WM_DPICHANGED` (use the suggested rect's monitor DPI to rescale dock height,
then renegotiate).

**Checkpoint:** move the taskbar to the left edge → dock renegotiates and
stays at the bottom without overlap; change display scaling 100%→150% → dock
height scales, no gaps; play a fullscreen video → dock does not sit on top of
it.

## Step 1.7 — Exit hygiene + single instance

**Build:**
- `WM_ENDSESSION`/`WM_QUERYENDSESSION`: remove the AppBar on logoff/shutdown.
- Single instance: named mutex (`CreateMutexW`) at startup; if it already
  exists, exit immediately (two appbars from the same tool = geometry chaos).
- Crash safety: `SetUnhandledExceptionFilter` whose filter removes the AppBar
  before letting the process die (best effort — document that a hard kill
  still leaks the strip until explorer restarts, as observed in 1.4).
- A clean quit affordance to replace the debug right-click if desired (keep
  right-click quit; it's fine for this stage).

**Checkpoint:** run the full Stage 1 acceptance row from
`docs/ARCHITECTURE.md` §11: reserved strip works, taskbar-move renegotiation
works, exit releases the strip, second launch is a no-op while one is running.
Stage 1 is DONE when every item passes on a real Windows 10 or 11 machine.

---

## Stage 1 definition of done (summary)

- [ ] All seven checkpoints pass on Windows 10/11 x64.
- [ ] No reserved dead space after any normal exit path (window close,
      right-click quit, logoff).
- [ ] Per-monitor DPI: crisp at 100%/150%, height rescales on `WM_DPICHANGED`.
- [ ] Not present in Alt-Tab or the taskbar's window list.
- [ ] Code layout matches `docs/ARCHITECTURE.md` §10 (`main.cpp`,
      `DockWindow.{h,cpp}` only — no Stage 2+ files).

## After Stage 1

Stages 2–5 are specified in `docs/ARCHITECTURE.md` §§4–7. When a stage begins,
break it into steps in this file first (as above), get the breakdown approved,
then implement step by step. Never begin a stage by writing all of its code at
once.
