# Stage 5 plan — automation buttons in taskbar empty space

Spec: `docs/ARCHITECTURE.md` §7. Acceptance: §12 rows 5a/5b.
Draft — refine against the actual Stage-4 code before starting.
Two sub-phases: 5a (dock-hosted buttons) must fully land before 5b (taskbar
overlay) begins; 5a remains the permanent fallback.

## Phase 5a — buttons hosted in the dock strip

### Step 5a.1 — Config load ✅ (done 2026-07-04)
**Built:** `src/Launcher.{h,cpp}`. Chose the plan's line-based format (not JSON)
to honor hard rule 2 (no dep) and avoid a fragile hand-rolled JSON parser:
`%LOCALAPPDATA%\browser_shell_os\config.txt`, one button per line
`style|label|action|target|iconPath?` (`style`∈pill|icon, `action`∈url|shortcut|
command). Blank / `#` / `;` lines ignored; malformed lines skipped with
`OutputDebugStringW` reason; missing file → zero buttons. Decodes UTF-16LE-BOM /
UTF-8-BOM / raw-UTF-8. `Button` struct + `Launcher::Buttons()` accessor for
5a.2/5a.3. Loaded synchronously in `DockWindow::Create` (startup, like
`ScanBrowserFrames`). Two inspector bursts (parsing-robustness + resource-
hygiene): fixed a swprintf_s-overflow process-kill (→ `_snwprintf_s`/`_TRUNCATE`),
a misaligned-wchar_t UB (→ memcpy), an unchecked MBTWC size-query, dropped a
redundant link pragma → re-burst clean → adjudicator MAY PROCEED. Sample config
written to the config path for the runtime check.

**Checkpoint:** config with 2 buttons loads; malformed file → dock still
starts, buttons skipped, `OutputDebugStringW` notes why. (Runtime check pending
on Windows via debugger; render/execute land in 5a.2–5a.3.)

### Step 5a.2 — Actions ✅ (done 2026-07-04)
**Built:** `Launcher::Execute(const Button&)` — copies action+target into a
detached **MTA** worker (pump-less fire-and-forget; STA would need a pump and
could hang a DDE handler) → `url`/`shortcut`: `ShellExecuteW(open, target)`;
`command`: `CreateProcessW` (both handles closed). `CoUninitialize` only on
`SUCCEEDED(CoInitializeEx)`. Never blocks the UI thread (spawn+detach returns
immediately). Debug trigger: `#ifdef _DEBUG` `WM_MBUTTONUP` cycles configured
buttons. Two bursts (threading + resource): fixed unconditional-CoUninitialize,
STA→MTA, ConfigPath free-of-garbage guard, MBTWC guard → re-burst clean →
adjudicator MAY PROCEED.

**Checkpoint:** each action type works from the debug middle-click trigger.
(Runtime check pending on Windows.)

### Step 5a.3 — Button strip rendering + clicks
**Build:** `Renderer` draws pill/icon buttons at the dock's right end;
hit-testing routes clicks to `Launcher::Execute`. Icons via
`LoadImageW`/`SHGetFileInfoW` for shortcut targets; pills are rounded rects
with label text.

**Checkpoint:** §12 row 5a: URL button opens the site in the default
browser; shortcut button runs; buttons persist across restart.

### Step 5a.4 — Config hot-reload
**Build:** watch the config directory (`ReadDirectoryChangesW` on a worker,
post to dock thread); reload and repaint on change.

**Checkpoint:** edit config while running → buttons update within ~1s, no
restart.

## Phase 5b — overlay on the taskbar's empty region

### Step 5b.1 — Gap measurement
**Build:** `src/TaskbarOverlayWindow.{h,cpp}` (ALL taskbar-geometry
heuristics live here — hard rule 6). Find `Shell_TrayWnd`; measure the empty
gap between the task-button list and the tray via UIA over the taskbar tree
(Win11) / child-window rects (`ReBarWindow32`, Win10). Debug overlay outline
only.

**Checkpoint:** outline hugs the empty region on Win10 and Win11, centered
and left-aligned taskbar layouts; opening/closing apps moves it correctly.

### Step 5b.2 — Overlay window + click-through
**Build:** borderless topmost tool window positioned in the measured gap;
`WM_NCHITTEST` returns `HTTRANSPARENT` outside button rects. Buttons render
via the SAME `Launcher`/button model as 5a (shared component, different
host).

**Checkpoint:** buttons work in the taskbar gap; clicks outside buttons
reach the taskbar normally (e.g. right-click taskbar menu still opens).

### Step 5b.3 — Dynamic re-measure + fallback
**Build:** re-measure on task-list `EVENT_OBJECT_LOCATIONCHANGE`
(debounced), `WM_DISPLAYCHANGE`, `ABN_POSCHANGED`. If measurement fails or
the gap is too small → hide the overlay and fall back to 5a's dock-hosted
strip automatically.

**Checkpoint:** §12 row 5b: open 15 apps → overlay yields; close them →
overlay grows back; simulate failure (rename detection heuristic) → buttons
appear in the dock instead.

## Definition of done

- [ ] 5a passes independently of 5b; 5b failure degrades to 5a silently.
- [ ] No code injection into explorer.exe anywhere; overlay only.
- [ ] Stage 1–4 acceptance rows still pass.
