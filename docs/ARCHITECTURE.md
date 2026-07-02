# Architecture & Design Spec

Native Windows shell tool: a dock strip above the taskbar that keeps minimized
browser windows' tab information visible, aggregating multiple windows as
staggered card stacks.

This spec defines the component model, the four iterative stages, the exact
Win32/UIA APIs each stage rests on, acceptance criteria, and known risks. Each
stage is **minimum functionality** and independently runnable — no stage depends
on a later stage's polish.

---

## 1. Guiding principles

1. **Minimum functionality per stage.** Every stage produces a demonstrable
   executable. Ship the smallest thing that proves the stage's capability.
2. **Native and dependency-light.** C++17, Win32 API only. No UI framework, no
   Electron, no embedded web view. The dock *is* a shell citizen: it registers
   as an application desktop toolbar (AppBar) and reserves real screen space —
   something an ordinary always-on-top window cannot do.
3. **One UI thread.** A single message loop owns the dock window; all Win32 UI
   calls happen on that thread. Anything that can block (UI Automation tree
   walks, window enumeration) runs on a worker thread and marshals results back
   via `PostMessage`.
4. **Fail visible, exit clean.** The AppBar must always deregister
   (`ABM_REMOVE`) on every exit path, or Windows keeps the strip reserved as
   dead space until logoff.

## 2. Component model

Components are introduced incrementally; the stage that introduces each is
noted.

| Component | Stage | Responsibility |
|---|---|---|
| `DockWindow` | 1 | The AppBar-registered, always-on-top, borderless window anchored above the taskbar. Owns the message loop. |
| `WindowMonitor` | 2 | Discovers browser top-level windows and tracks lifecycle (create / destroy / minimize / restore) via `EnumWindows` + `SetWinEventHook`. |
| `TabReader` | 3 | Reads a browser window's tab titles through UI Automation. Interface is deliberately narrow so a native-messaging implementation can replace it later. |
| `Store` | 3 | In-memory model: tracked windows and their last-known tabs. Single writer (UI thread); workers post snapshots into it. |
| `Renderer` | 2–5 | Paints the dock. Grows from a text indicator (2) → one tab card (3) → staggered multi-card stack with hover-fan and click-to-restore (4) → button strip (5a). |
| `Launcher` | 5 | Owns automation-button config (JSON, persisted); executes button actions (`ShellExecuteW` / `CreateProcessW`). |
| `TaskbarOverlayWindow` | 5b | Click-through overlay positioned over the taskbar's empty region, hosting the buttons there. |

Data model (introduced Stage 3, generalized Stage 4):

```cpp
struct Tab {
    std::wstring title;
    // url: not reliably available via UIA; see §8 upgrade path
};

struct TrackedWindow {
    HWND hwnd;
    std::wstring title;        // window title
    bool minimized;
    std::vector<Tab> tabs;     // last-known snapshot
};
```

## 3. Stage 1 — Hello-world shell tool: the AppBar dock

**The hardest stage.** All of the native shell plumbing lives here; stages 2–4
are additive once this is solid.

### Goal
A borderless, always-on-top window that registers as an **application desktop
toolbar** anchored to the bottom screen edge, sitting directly above the
taskbar, **reserving** its strip (maximized windows do not cover it), and
painting placeholder text.

### Key APIs and flow
1. `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`
   — before any window is created.
2. `RegisterClassExW` + `CreateWindowExW` with style `WS_POPUP` and extended
   styles `WS_EX_TOOLWINDOW | WS_EX_TOPMOST` (tool window keeps the dock out of
   Alt-Tab and the taskbar itself).
3. AppBar registration via `SHAppBarMessage`:
   - `ABM_NEW` with a private callback message ID registers the appbar.
   - `ABM_QUERYPOS` proposes a bottom-edge rectangle; the shell adjusts it so it
     does not overlap the taskbar or other appbars.
   - `ABM_SETPOS` commits the adjusted rectangle, then `SetWindowPos` moves the
     window into it.
   - On the callback message, handle `ABN_POSCHANGED` (taskbar moved/resized,
     another appbar appeared) by re-running QUERYPOS/SETPOS.
4. `WM_PAINT`: plain GDI text for Stage 1.
5. `WM_DPICHANGED` / `WM_DISPLAYCHANGE`: recompute the desired rect and
   renegotiate.
6. `WM_DESTROY`: `SHAppBarMessage(ABM_REMOVE, ...)` then `PostQuitMessage`.
   Also remove on `WM_ENDSESSION` and fatal-error paths.

Reference implementation to mirror: the official AppBar sample at
`microsoft/Windows-classic-samples` →
`Samples/Win7Samples/winui/shell/legacysamples/appbar/AppBar.cpp`, plus MS Learn
"Using Application Desktop Toolbars".

### Acceptance criteria
- Dock strip renders directly above the taskbar.
- A maximized window stops at the top of the dock (space genuinely reserved).
- Moving the taskbar to another edge, or a resolution/DPI change, repositions
  the dock correctly.
- Exiting the tool releases the strip — no dead reserved band remains.

### Risks / gotchas
- QUERYPOS/SETPOS negotiation is stateful and order-sensitive; getting it wrong
  yields overlaps or ghost gaps.
- Taskbar auto-hide (`ABS_AUTOHIDE`) changes the geometry rules.
- Multi-monitor: Stage 1 targets the primary monitor only; the rect must still
  be computed from that monitor's work area, not virtual-screen coordinates.
- Crash before `ABM_REMOVE` leaves reserved space until explorer restart —
  register a last-chance handler that removes the appbar.

## 4. Stage 2 — Detect a browser is open; perform a simple action

### Goal
The dock reflects browser presence: when a Chrome/Edge window exists, the dock
lights an indicator and shows the window's title; when the last one closes, it
clears. No polling loops.

### Key APIs
- **Discovery:** `EnumWindows`, filtering to real browser frames:
  - window class `Chrome_WidgetWin_1` (Chromium family: Chrome, Edge, Brave...),
  - visible (`IsWindowVisible`), unowned (`GetWindow(hwnd, GW_OWNER) == NULL`),
    non-empty title — Chromium spawns several helper top-level windows with the
    same class that must be excluded,
  - process image name via `GetWindowThreadProcessId` +
    `OpenProcess`/`QueryFullProcessImageNameW` (`chrome.exe`, `msedge.exe`).
- **Lifecycle without polling:** `SetWinEventHook` (out-of-context,
  `WINEVENT_OUTOFCONTEXT`) for `EVENT_OBJECT_CREATE`, `EVENT_OBJECT_DESTROY`,
  `EVENT_OBJECT_SHOW`, `EVENT_OBJECT_HIDE`; the hook callback re-validates the
  HWND against the filter and posts add/remove messages to the UI thread.

### Acceptance criteria
- Launch browser → indicator appears within ~1s. Close last browser window →
  indicator clears. CPU usage idles at ~0% (event-driven, no timer scans).

### Risks
- Helper-window false positives (mitigated by the filter above).
- Hook callbacks arrive on arbitrary threads' contexts — do no UI work there;
  post to the dock thread.

## 5. Stage 3 — Track one browser's tabs; keep them on-screen after minimize

### Goal
When a tracked browser window is **minimized**, the dock shows a card listing
that window's tab titles, and the card persists while the window stays
minimized. Restoring the window clears the card. This is the core "toolbar
stays visible above the taskbar" behavior.

### Key APIs
- **Minimize/restore detection:** extend the Stage-2 hook set with
  `EVENT_SYSTEM_MINIMIZESTART` and `EVENT_SYSTEM_MINIMIZEEND`.
- **Tab reading (UI Automation):**
  - `CoCreateInstance(CLSID_CUIAutomation)` → `IUIAutomation`.
  - `ElementFromHandle(browserHwnd)`, then a condition-based find for the tab
    strip: control type `UIA_TabControlTypeId`, children
    `UIA_TabItemControlTypeId`; each tab item's `Name` property is the tab
    title.
  - Use a cache request (`IUIAutomationCacheRequest`) to pull all names in one
    cross-process round trip.
- **Timing:** snapshot tabs on `MINIMIZESTART` (the UIA tree is still live at
  that moment) and opportunistically on foreground/title changes, storing the
  result in `Store`. The card renders from the stored snapshot, so it remains
  valid even if the minimized window's UIA tree goes dormant (Chromium can
  suspend renderers of minimized windows).

### Acceptance criteria
- Minimize a browser with N tabs → dock card lists those N titles.
- Card persists for as long as the window is minimized.
- Restore → card clears (or collapses back to the Stage-2 indicator).

### Risks — where UIA is known to be fragile
- **Tab titles: yes. Full URLs: no.** UIA exposes only the *active* tab's
  address-bar value; background-tab URLs are not available. This is accepted
  for Stage 3 and is the trigger for the §8 upgrade path.
- Chromium's accessibility tree may be inactive until a client queries it;
  first query can be slow. Do all UIA work on a worker thread; debounce.
- Browser updates can rearrange the automation tree; keep the tab-strip lookup
  heuristic isolated inside `TabReader` so fixes are one-file.

## 6. Stage 4 — Track multiple browsers' tabs; staggered stack aggregation

### Goal
Generalize Stage 3 to *every* tracked browser window. Each minimized window is
a card; multiple cards render as a **staggered, layered stack** in the dock:
offset x/y like a fanned deck, expanding upward on hover, and restoring the
corresponding window when a card is clicked.

### Key additions
- `Store` becomes a map `HWND → TrackedWindow`; `WindowMonitor` keeps it in
  sync across all browser windows (including multiple processes/profiles —
  Chrome and Edge tracked simultaneously).
- `Renderer` gains:
  - staggered layout (per-card offset + z-order; newest minimized on top),
  - hover hit-testing → fan animation opening upward into the space above the
    dock (`WM_MOUSEMOVE`/`TrackMouseEvent` for enter/leave),
  - click → `ShowWindow(hwnd, SW_RESTORE)` + `SetForegroundWindow(hwnd)`, then
    the stack re-settles.
- Fan overlay taller than the reserved strip: the reserved AppBar band stays
  slim; the fan renders in a transient pop-up window (`WS_EX_TOPMOST`,
  layered) that appears above the dock on hover and dismisses on leave — the
  reserved area itself never grows.

### Acceptance criteria
- Minimize three browser windows → three staggered cards, each listing its own
  window's tabs.
- Hover fans the stack; leaving collapses it.
- Clicking a card restores exactly that window and removes its card.

### Risks
- Input routing/z-order across overlapping cards (rigorous hit-test rects).
- Dock overflow with many minimized windows: cap visible cards, spill into a
  "+N more" affordance rather than growing the reserved strip.
- Repeated UIA snapshots across many windows: debounce per-window, snapshot
  only on minimize/title-change events, never on a timer.

## 7. Stage 5 — Automation buttons in the taskbar's empty space

### Goal
Put quick-action launchers in the unused real estate of the taskbar itself:
**pill-style or icon-style buttons** that perform one of three actions:

1. **Open a website** — launch the default browser to a URL entered ad hoc.
2. **Open a designated website** — a pinned, pre-configured URL (one button per
   pinned site, with favicon/label).
3. **Run an automation shortcut** — launch a configured command, script, or
   `.lnk` shortcut (e.g. "open these 3 sites + this app"), bundled and
   persistent with the shell tool.

Buttons persist across sessions via the shell tool's config file.

### Approach — two sub-phases (minimum functionality first)

**Phase 5a — buttons hosted in the dock strip (safe, ships first).**
The Stage 1 dock already owns reserved shell real estate; render the pill/icon
buttons at its right end, next to the card stacks. Zero new shell integration
risk; proves the button model, config persistence, and actions end to end.

**Phase 5b — overlay on the taskbar's empty region (the headline).**
Windows 11 removed the DeskBand/toolbar extension APIs, so nothing can be
*injected into* the taskbar process safely. Instead, position a slim,
borderless, topmost overlay window *over* the taskbar's empty region:

- Locate the taskbar (`FindWindowW(L"Shell_TrayWnd", ...)`) and measure the
  gap between the end of the task-button list and the tray area via UI
  Automation over the taskbar's element tree (`ElementFromHandle` on the tray,
  walk to the task list / `ReBarWindow32` rects on Win10; UIA rects on Win11).
- Size/position the overlay inside that gap; re-measure on
  `EVENT_OBJECT_LOCATIONCHANGE` for the task list (buttons appear/disappear as
  apps open) and on the Stage-1 display-change events.
- The overlay is click-through outside its buttons (`WM_NCHITTEST` →
  `HTTRANSPARENT` in dead zones) so taskbar behavior is otherwise untouched.

### Key APIs
- Actions: `ShellExecuteW` (`open` verb) for URLs and `.lnk` shortcuts;
  `CreateProcessW` for raw commands.
- Config: a JSON file next to the executable
  (`%LOCALAPPDATA%\browser_shell_os\config.json`) defining buttons:
  `{ id, style: pill|icon, label, iconPath?, action: url|shortcut|command,
  target }`. Loaded at startup, hot-reloaded on change
  (`ReadDirectoryChangesW`).
- New component: `Launcher` (owns button config, executes actions);
  `Renderer` gains a button strip; Phase 5b adds `TaskbarOverlayWindow`.

### Acceptance criteria
- 5a: configured buttons render in the dock; clicking a URL button opens the
  default browser to that site; clicking a shortcut button runs it; buttons
  survive restart (config persistence).
- 5b: buttons appear in the taskbar's empty space; opening many apps (task
  list grows) pushes/shrinks the overlay rather than overlapping buttons;
  clicks in remaining empty taskbar space still reach the taskbar.

### Risks
- 5b rests on measuring explorer's internal layout — resilient re-measurement
  and a graceful fallback to 5a (dock-hosted buttons) are mandatory.
- Windows updates can rearrange the taskbar's UIA tree; keep all measurement
  heuristics inside `TaskbarOverlayWindow`, mirroring the `TabReader`
  isolation rule.
- Never inject code into explorer.exe; overlay-only.

## 8. Cross-cutting concerns

- **Threading model:** UI thread = dock message loop, sole `Store` writer.
  Worker thread(s) run `EnumWindows` re-validation and UIA snapshots; they
  communicate exclusively by `PostMessage`-ing owned heap payloads to the dock
  window. `SetWinEventHook` callbacks do the minimum (validate + post).
- **DPI / display changes:** per-monitor-v2 awareness set at startup; renegotiate
  the AppBar rect on `WM_DPICHANGED`, `WM_DISPLAYCHANGE`, and `ABN_POSCHANGED`.
- **Lifetime hygiene:** `ABM_REMOVE` on every exit path; unhook all WinEvent
  hooks; `CoUninitialize` after UIA teardown.
- **Configuration:** a small config file for which browser processes to track
  and dock appearance. Formalized in Stage 5 (button definitions live there);
  not needed for stages 1–4 minimum functionality.

## 9. Upgrade path: browser extension + native messaging (documented, not built)

If/when UIA's limits bite (no background-tab URLs, tree fragility), the
replacement is the browsers' official **native messaging** mechanism:

- A WebExtension (Chrome/Edge, Manifest V3) with the `tabs` permission observes
  exact tab titles, URLs, favicons, and window membership, and connects to a
  registered **native messaging host**.
- The host is a small native executable (stdin/stdout, length-prefixed JSON)
  that forwards tab state to the shell tool over a local pipe — or the shell
  tool itself acts as the host.
- Integration point: this becomes an alternative implementation behind the
  `TabReader` interface. `Store`, `Renderer`, `WindowMonitor`, `DockWindow`,
  and `Launcher` are untouched.

Note: **no Electron anywhere in this architecture.** Electron was considered in
early ideation as a host for a web-based dock UI; with a native C++ AppBar,
the dock already owns real shell-level screen space, and the native messaging
host is a plain executable. Electron would add a Chromium runtime without
adding capability.

## 10. Repository layout

```
README.md
CLAUDE.md                   working rules + Stage 1 step breakdown for AI-assisted dev
docs/ARCHITECTURE.md        ← this document
CMakeLists.txt              (lands with Stage 1)
src/
  main.cpp                  entry point, DPI setup, message loop
  DockWindow.{h,cpp}        Stage 1
  WindowMonitor.{h,cpp}     Stage 2
  TabReader.{h,cpp}         Stage 3 (UIA implementation)
  Store.{h,cpp}             Stage 3
  Renderer.{h,cpp}          Stages 2–4
  Launcher.{h,cpp}          Stage 5
  TaskbarOverlayWindow.{h,cpp}  Stage 5b
```

## 11. Per-stage verification (run on Windows)

| Stage | Test |
|---|---|
| 1 | Build, run; maximize Notepad → it must stop above the dock. Exit tool → maximize again → window reaches the taskbar (strip released). Move taskbar to left edge → dock renegotiates. |
| 2 | Start tool, then open Chrome → indicator on. Close all Chrome windows → indicator off. Task Manager shows ~0% CPU at idle. |
| 3 | Open a browser with 5 known tabs, minimize → dock card lists the 5 titles. Restore → card clears. |
| 4 | Open 3 browser windows (mixed Chrome + Edge), minimize all → 3 staggered cards. Hover → fan. Click card #2 → exactly that window restores. |
| 5a | Configure a URL button + a shortcut button → both render in the dock; clicks perform the actions; restart the tool → buttons persist. |
| 5b | Buttons appear in the taskbar's empty region; open 15 apps → overlay yields space to the growing task list; click empty taskbar space outside a button → normal taskbar behavior. |

## 12. References

- [Using Application Desktop Toolbars — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/shell/application-desktop-toolbars)
- [`SHAppBarMessage` — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shappbarmessage)
- [`APPBARDATA` — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-appbardata)
- [Official AppBar sample (`AppBar.cpp`) — microsoft/Windows-classic-samples](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/winui/shell/legacysamples/appbar/AppBar.cpp)
- [Native messaging — Microsoft Edge developer docs](https://learn.microsoft.com/en-us/microsoft-edge/extensions-chromium/developer-guide/native-messaging)
