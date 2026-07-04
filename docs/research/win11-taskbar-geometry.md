# Win11/Win10 taskbar gap geometry (scout findings, 2026-07-04)

Reference for `TaskbarOverlayWindow` (Stage 5b). ALL heuristics isolated to that
one file (hard rule 6); this doc records *why* the code looks the way it does so
a future Windows update that breaks it is a one-file + one-doc fix.

## The headline: Win11 killed the per-button HWNDs — and the HWND stubs LIE

Win11 (22H2/23H2/24H2, incl. build 26200) rebuilt the taskbar as XAML islands
(`Windows.UI.Composition.DesktopWindowContentBridge` under `Shell_TrayWnd`). The
Win10 per-button `MSTaskListWClass` is **absent** on Win11.

The `MSTaskSwWClass` ("Running applications") container HWND still exists — **but
its rect is a legacy stub that does NOT match the XAML layout.** Measured live on
build 26200 @ 200%: `MSTaskSwWClass` = 45..577 (in the non-PMv2 probe's virtualized
coords), while the actual app icons render out to ~1418. So a pure-HWND measurement
lands badly wrong on Win11. **UIA is required** to read the real XAML element rects.
(Confirmed the hard way: the first HWND-only 5b.1 outline spanned far too wide on
both sides.)

## Win11 measurement — UIA (what 5b.1 actually does)

`ElementFromHandle(Shell_TrayWnd)` → `FindFirst(Descendants, AutomationId="TaskbarFrame")`
→ enumerate its children (`FindAll(Children, TrueCondition)`), reading each child's
`get_CurrentBoundingRectangle` + `ClassName` + `AutomationId`:

- **gap.left** = max `.right` over task buttons: `ClassName ==
  "Taskbar.TaskListButtonAutomationPeer"` (the app buttons) plus the `StartButton`/
  `SearchButton`/`TaskViewButton` toggle buttons.
- **gap.right** = `TrayNotifyWnd` left edge (HWND rect), OVERRIDDEN by the
  `WidgetsButton` (`AutomationId == "WidgetsButton"`) left edge **only when** it sits
  in the gap (`widgetsLeft > gap.left && widgetsLeft < trayLeft`). Default Win11 puts
  Widgets on the far left → then it's ignored and the tray bounds the gap.
- **gap top/bottom** = `Shell_TrayWnd` rect (HWND).

Live element rects on the user's taskbar (physical px in a PMv2 process): app
buttons end at Postman `..1418`; `WidgetsButton` = `1948..2252`; tray starts 2276.
→ gap = **[1418, 1948]**. In a PMv2 process, UIA `BoundingRectangle` and
`GetWindowRect` share one physical-pixel space (no conversion). The 1440-vs-2880
discrepancy seen in a raw PowerShell probe is only because that probe wasn't PMv2.

Guards (each prevents a visibly-wrong outline): explorer.exe owner verify;
`IsAutoHide` bail; null `TrayNotifyWnd` → invalid; `GetDpiForWindow(tray)==0` →
invalid; no task-button matched (`gap.left == tray.left`) → invalid; min-gap
threshold (scaled by the taskbar monitor's DPI).

Debt: the overflow/"show more" chevron's class is unconfirmed — if the task list
overflows it may not be counted as a task button and the outline could overlap it.
Revisit with a live overflowed taskbar.

## Win10 fallback (no TaskbarFrame element)

```
rebar  = FindWindowExW(tray, 0, L"ReBarWindow32", NULL)
taskSw = FindWindowExW(rebar?rebar:tray, 0, L"MSTaskSwWClass", NULL)
taskLs = FindWindowExW(taskSw, 0, L"MSTaskListWClass", NULL)   // exists on Win10
gap.left = (taskLs?taskLs:taskSw).right   // with sleep-wake stale-rect sanity check
gap.right = TrayNotifyWnd.left
```

On Win10 the HWND rects DO match the layout, so no UIA is needed there. The UIA vs
HWND fork is decided by whether the `TaskbarFrame` element is found.

## Threading (rule 5)

UIA is blocking cross-process work → it runs on a **worker thread** (mirrors
`TabReader`): `CoInitializeEx(MULTITHREADED)` + `CLSID_CUIAutomation`, waits on a
request flag, measures, `PostMessageW`s a heap `Gap*` back; the UI thread
`SetWindowPos`es the outline. `automation.Reset()` before `CoUninitialize`;
`CoUninitialize` only on `SUCCEEDED(CoInitializeEx)`.

## Mandatory guards (each maps to a real failure mode)

1. **Explorer-owner check.** Third-party shells (YASB, Zebar, Managed Shell)
   register `Shell_TrayWnd` for fake taskbars. Verify `GetWindowThreadProcessId`
   → `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` →
   `QueryFullProcessImageNameW` basename == `explorer.exe` (case-insensitive).
2. **Sleep-wake stale rect.** After resume, `MSTaskSwWClass`'s rect can be stale
   (documented by Windows11DragAndDropToTaskbarFix). Sanity-check: task height
   > 0 and ≤ tray height, and `taskSw.left/right` within `tray.left/right`. If
   not sane → skip this measurement (retry on next tick / delayed remeasure).
3. **Auto-hide.** `SHAppBarMessage(ABM_GETSTATE)` & `ABS_AUTOHIDE` → taskbar is a
   1–2px sliver; hide the overlay. (`ABM_GETSTATE` ignores `hWnd`; null is fine.)
   ⚠️ This is a synchronous `SendMessage` to explorer on the UI thread — fine as a
   light query, but 5b.3 should fence it (`SendMessageTimeout`) so a hung explorer
   can't hold the pump.

## Layout variants

- `HKCU\...\Explorer\Advanced\TaskbarAl`: 0 = left-aligned, 1 = centered (Win11
  default). Both use the SAME formula — `MSTaskSwWClass.right` moves with app
  count either way. Centered ALSO leaves a left-side region, but that holds
  Start/Search/Widgets/TaskView — not clean; we only use the right-side gap.
- As apps open/close, `MSTaskSwWClass` grows/shrinks → right-side gap tracks it →
  fall back to 5a dock-hosted buttons when gap < min threshold.

## DPI / multimon

- PMv2 process: `GetWindowRect`, `SetWindowPos`, UIA `BoundingRectangle` are ALL
  physical screen pixels → pass measured rect straight to `SetWindowPos`, no
  conversion.
- Scale DPI-dependent thresholds by the **taskbar monitor's** DPI, not the
  overlay's (it starts at 0,0 on the primary monitor). `GetDpiForWindow(tray)`
  works cross-process and is monitor-authoritative.
- Scope: primary `Shell_TrayWnd` only (matches Stage 1 dock scope). Secondary
  taskbars are `Shell_SecondaryTrayWnd` — out of scope until multimon stage.

## Dynamic re-measure (for 5b.3)

`SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, ..., explorerPid, 0, OUTOFCONTEXT)`,
filter `hwnd==taskSw` or `hwnd==tray` with `idObject==OBJID_WINDOW`, debounce
~200ms, remeasure on UI thread. Also remeasure on `WM_DISPLAYCHANGE`,
`ABN_POSCHANGED`, and `PBT_APMRESUMEAUTOMATIC` (delayed ~500ms for the stale-rect
bug). 5b.1 uses a 500ms poll timer as a stand-in.

## Sources

TaskbarXI (`Taskbar11.cpp`), Windows11DragAndDropToTaskbarFix CHANGELOG (sleep-wake
bug), windhawk-mods #1704 (class-name spoofing → verify PID), Ramen Software Win11
analysis (XAML rebuild), MSDN "UI Automation and Screen Scaling" (physical-px APIs),
NVDA PR #13691 (`Shell_TrayWnd`/`XamlExplorerHostIslandWindow` are real HWNDs).
