#include "HostWindow.h"
#include "WindowMonitor.h"
#include "PaintUtil.h"
#include "Trace.h"
#include <algorithm>
#include <thread>
#include <dwmapi.h>

namespace
{
    constexpr wchar_t kClassName[]      = L"BrowserShellOsHostWindow";
    constexpr UINT    kWindowEventMsg   = WM_APP + 2;
    constexpr UINT    kTabSnapshotMsg   = WM_APP + 3;
    constexpr UINT    kConfigChangedMsg = WM_APP + 4;
    constexpr UINT_PTR kDebounceTimer   = 1;
    constexpr UINT    kDebounceMs       = 200;
    constexpr UINT_PTR kSnapshotTimer   = 3;
    constexpr UINT    kSnapshotMs       = 150;
    constexpr UINT_PTR kConfigTimer     = 4;
    constexpr UINT    kConfigMs         = 300;
    constexpr UINT    kRemeasureMsg     = WM_APP + 5;  // LOCATIONCHANGE hook → debounce
    constexpr UINT    kFanActivateMsg   = WM_APP + 7;  // fan(UI) → dock: tab row clicked
    constexpr UINT    kTabActivateResultMsg = WM_APP + 8;  // TabReader worker → dock: activate outcome
    constexpr UINT    kChipHoverMsg     = WM_APP + 9;  // overlay → dock: hovered chip (HWND or 0)
    constexpr UINT    kChipClickMsg     = WM_APP + 10; // overlay → dock: chip clicked (restore window)
    constexpr UINT    kButtonHoverMsg   = WM_APP + 12; // overlay → dock: hovered FolderFan button (index or -1)
    constexpr UINT    kFolderScanResultMsg = WM_APP + 13; // scan worker → dock: FolderFan subfolders ready
    constexpr UINT    kFolderFanChangedMsg = WM_APP + 14; // any-change watcher → dock: FolderFan root changed
    constexpr UINT_PTR kFolderFanScanTimer = 8;   // debounces a change-burst (e.g. a git clone) into one rescan per root
    constexpr UINT    kFolderFanScanMs     = 400;

    // kFolderScanResultMsg payload (owned by receiver — delete after reading).
    struct FolderScanResult
    {
        std::wstring               root;
        std::vector<std::wstring>  entries;
    };
    // In-place fullscreen (F11/video/game) no EVENT_SYSTEM_FOREGROUND — global LOCATIONCHANGE hook catches rect change for zero-latency suppression, debounced to final rect.
    constexpr UINT     kFgLocationMsg   = WM_APP + 11;
    constexpr UINT_PTR kSuppressTimer   = 7;
    constexpr UINT     kSuppressMs      = 120;
    // Re-measure gap on task-list LOCATIONCHANGE (already coalesced by overlay worker).
    constexpr UINT_PTR kOverlayTimer    = 5;
    constexpr UINT    kOverlayMs        = 200;
    constexpr UINT    kResumeRemeasureMs = 500;  // let shell settle after wake before re-measure
    // Low-frequency safety re-check: self-heals stuck-hidden/stuck-suppressed state from missed events.
    constexpr UINT_PTR kSafetyTimer     = 6;
    constexpr UINT    kSafetyMs         = 1500;
    constexpr int     kQuitHotkeyId     = 1;  // Ctrl+Alt+Shift+Q — guaranteed quit hatch (no dock strip to right-click)

    // Single-instance: safe to keep a plain HWND here for the WinEventProc callback.
    // Written on the UI thread in Create/WM_DESTROY; read on the same thread in WinEventProc
    // (OUTOFCONTEXT delivers on our message-pump thread). No cross-thread access.
    HWND s_dockHwnd = nullptr;

    // Same single-instance/UI-thread contract as s_dockHwnd. Lets the one static WinEventProc
    // tell the global fg-fullscreen LOCATIONCHANGE hook apart from the explorer-scoped taskbar
    // hook (both fire EVENT_OBJECT_LOCATIONCHANGE) and route it to suppression, not a re-measure.
    HWINEVENTHOOK s_fgLocationHook = nullptr;

    // Same single-instance/UI-thread contract. Win11 Start/Search close by DWM-cloaking (no
    // foreground change, no LOCATIONCHANGE), so closing a flyout emits none of the events the
    // other hooks watch. This system-wide CLOAKED hook is the zero-latency close signal; without
    // it, un-suppression waits on the 1500ms kSafetyTimer. The callback re-derives suppression,
    // which re-reads the (now cloaked) foreground and clears the hide.
    HWINEVENTHOOK s_flyoutCloakHook = nullptr;

    void StoreWindow(Store& store, HWND hwnd)
    {
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, _countof(title));
        store.Set(hwnd, title);
    }

    // Basename of process owning hwnd (e.g. L"StartMenuExperienceHost.exe"), or empty if query fails.
    std::wstring ProcessBaseName(HWND hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return {};
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc) return {};
        wchar_t path[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        const bool ok = QueryFullProcessImageNameW(proc, 0, path, &len);
        CloseHandle(proc);
        if (!ok) return {};
        const wchar_t* file = wcsrchr(path, L'\\');
        return file ? file + 1 : path;
    }

    // Win11 flyouts (Start/Search) DWM-CLOAK instead of closing: process-name checks stay "open" until different window claims foreground. DwmGetWindowAttribute is safe on UI thread; query failure → "not cloaked".
    bool IsCloaked(HWND hwnd)
    {
        DWORD cloaked = 0;
        return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
               && cloaked != 0;
    }
}

HostWindow::~HostWindow()
{
    // GetMessage==-1 abnormal exit: ensure orderly WM_DESTROY teardown if window not yet destroyed.
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
    }
}

bool HostWindow::Create(HINSTANCE instance)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // WM_PAINT owns all drawing; WM_ERASEBKGND returns 1
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc))
    {
        return false;
    }

    // Top-level WS_POPUP (needs broadcasts): owns pump/hooks, paints nothing. TOOLWINDOW: no taskbar. NOACTIVATE: no foreground steal. 1x1 at origin; chips in overlay.
    const HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClassName,
        L"browser_shell_os",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, instance,
        this);
    if (!hwnd)
    {
        return false;
    }

    // Redundant (WM_NCCREATE sets m_hwnd) but belt+suspenders.
    m_hwnd = hwnd;

    // Explorer broadcasts TaskbarCreated to every top-level on taskbar (re)create; we need it to re-hook.
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // Dock strip gone; register global hatch so app is always closable (overlay may be suppressed).
    RegisterHotKey(hwnd, kQuitHotkeyId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'Q');

    for (HWND h : ScanBrowserFrames())
    {
        StoreWindow(m_store, h);
        if (IsIconic(h))
            m_store.SetMinimized(h, true);
    }

    s_dockHwnd = hwnd;
    m_winEventHook = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE,
        nullptr, WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT);
    m_winEventHookMinimize = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
        nullptr, WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT);
    m_winEventHookNameChange = SetWinEventHook(
        EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
        nullptr, WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT);
    // Pre-warm UIA on foreground: minimized window's UIA tree is stripped, so snapshot while visible.
    m_winEventHookForeground = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT);

    m_tabReader = std::make_unique<TabReader>(hwnd, kTabSnapshotMsg, kTabActivateResultMsg);

    m_fanPopup = std::make_unique<FanPopup>();
    m_fanPopup->Create(instance, hwnd, kFanActivateMsg);

    m_launcher.Load();  // Stage 5a: automation-button config
    Paint::SetActiveTheme(m_launcher.ThemeName());  // Stage D: skin from optional config `theme=`
    RequestFolderScans();
    RebuildFolderFanWatchers();

    // Stage 5b: host automation buttons in taskbar's gap. Measure now, re-measure on timer + geometry changes.
    m_taskbarOverlay = std::make_unique<TaskbarOverlayWindow>();
    if (m_taskbarOverlay->Create(instance, &m_launcher, &m_store, hwnd,
                                 kChipClickMsg, kChipHoverMsg, kButtonHoverMsg))
    {
        if (m_fanPopup) m_taskbarOverlay->SetTopSibling(m_fanPopup->Hwnd());
        m_taskbarOverlay->RequestMeasure();
        SetTimer(hwnd, kOverlayTimer, kOverlayMs, nullptr);  // Backstop: guarantees 2nd verdict if 1st post lost
        SetTimer(hwnd, kSafetyTimer, kSafetyMs, nullptr);    // Periodic self-heal
        // Re-measure task-list resize (app open/close → gap grows/shrinks); scope to explorer PID to avoid every window move.
        HookTaskbarLocation();
        // Zero-latency fullscreen watch: single system-wide LOCATIONCHANGE (never re-scoped; per-foreground re-scope was crash suspect). Filters to current foreground frame.
        m_winEventHookFgLocation = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        s_fgLocationHook = m_winEventHookFgLocation;
        // Zero-latency Win11 flyout-close watch: only CLOAKED reports it (no foreground change, no LOCATIONCHANGE). Without it, recovery waits 1500ms safety tick.
        m_winEventHookFlyoutCloak = SetWinEventHook(
            EVENT_OBJECT_CLOAKED, EVENT_OBJECT_CLOAKED,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        s_flyoutCloakHook = m_winEventHookFlyoutCloak;
    }

    // Watch config dir for live edits; create first so watch attaches before user writes config.
    const std::wstring cfgDir = Launcher::ConfigDir();
    if (!cfgDir.empty())
    {
        CreateDirectoryW(cfgDir.c_str(), nullptr);
        m_configWatcher = std::make_unique<ConfigWatcher>(hwnd, kConfigChangedMsg);
        m_configWatcher->Start(cfgDir, Launcher::ConfigFileName());
    }

    return true;
}

// Fullscreen fills rcMonitor (maximized stops at rcWork, above taskbar) on same monitor as overlay. Derive from overlay's taskbar. ±2px tolerance for scaling.
bool HostWindow::FullscreenOnDockMonitor(HWND fg) const
{
    if (!fg || fg == GetDesktopWindow() || fg == GetShellWindow()) return false;
    RECT r;
    if (!GetWindowRect(fg, &r)) return false;
    HMONITOR dockMon = m_taskbarOverlay ? m_taskbarOverlay->TaskbarMonitor() : nullptr;
    if (!dockMon) return false;
    if (MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) != dockMon) return false;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfo(dockMon, &mi)) return false;
    // ±2px: DPI-unaware fullscreen app DWM stretch can round ±2px at fractional scaling (175%); maximized differs by ≥ border+taskbar.
    auto within2 = [](LONG a, LONG b) { return (a > b ? a - b : b - a) <= 2; };
    const RECT& m = mi.rcMonitor;
    return within2(r.left, m.left) && within2(r.top, m.top) &&
           within2(r.right, m.right) && within2(r.bottom, m.bottom);
}

// Flyouts DWM-cloak (no LOCATIONCHANGE on close): drive suppression off foreground process + fullscreen state; re-measure on release (taskbar animation settle).
void HostWindow::UpdateOverlaySuppression()
{
    if (!m_taskbarOverlay) return;
    const HWND fg = GetForegroundWindow();
    const std::wstring proc = ProcessBaseName(fg);
    // Cloaked flyout still foreground (hide-in-place): corroborate process name with DWMWA_CLOAKED so visual close counts as closed.
    const bool procMatch = _wcsicmp(proc.c_str(), L"StartMenuExperienceHost.exe") == 0 ||
                            _wcsicmp(proc.c_str(), L"SearchHost.exe") == 0;
    const bool cloaked = IsCloaked(fg);
    const bool flyoutOpen = procMatch && !cloaked;
    const bool suppress = flyoutOpen || FullscreenOnDockMonitor(fg);
    if (suppress == m_overlaySuppressed) return;
    m_overlaySuppressed = suppress;
    m_taskbarOverlay->SetSuppressed(suppress);
    if (!suppress)
        SetTimer(m_hwnd, kOverlayTimer, kOverlayMs, nullptr);
}

// (Re)scope LOCATIONCHANGE hook to live explorer PID: on explorer restart, old hook is dead so gap would freeze. Idempotent: unhook-then-hook.
void HostWindow::HookTaskbarLocation()
{
    if (m_winEventHookLocation)
    {
        UnhookWinEvent(m_winEventHookLocation);
        m_winEventHookLocation = nullptr;
    }
    const DWORD explorerPid = TaskbarOverlayWindow::TaskbarProcessId();
    if (explorerPid)
        m_winEventHookLocation = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc, explorerPid, 0, WINEVENT_OUTOFCONTEXT);
}

// Coalesce snapshot requests (Win+M burst or NAMECHANGE flood) into one flush after 150ms. Foreground pre-warm stays immediate (must beat UIA tree-strip on minimize).
void HostWindow::RequestSnapshotDebounced(HWND hwnd)
{
    if (std::find(m_pendingSnapshots.begin(), m_pendingSnapshots.end(), hwnd)
            == m_pendingSnapshots.end())
        m_pendingSnapshots.push_back(hwnd);
    SetTimer(m_hwnd, kSnapshotTimer, kSnapshotMs, nullptr);
}

// Un-minimize and focus. ShowWindowAsync to avoid blocking UI pump if target is hung. SetForegroundWindow fails when we're not foreground process; fall back to flash.
void HostWindow::RestoreWindow(HWND target)
{
    if (!target) return;
    ShowWindowAsync(target, SW_RESTORE);
    if (!SetForegroundWindow(target))
    {
        FLASHWINFO fi = { sizeof(fi), target, FLASHW_TRAY, 3, 0 };
        FlashWindowEx(&fi);
    }
}

// Anchor fan above chip (direct query from overlay, same UI thread). Fan grows upward from chip top → edge-adjacent (minimal seam).
void HostWindow::ShowFanForChip(HWND chip)
{
    if (!chip || !m_fanPopup || !m_taskbarOverlay) return;
    const auto& all = m_store.All();
    auto it = all.find(chip);
    if (it == all.end()) return;

    RECT r;
    if (!m_taskbarOverlay->ChipRectScreen(chip, &r)) return;
    m_fannedButtonIndex = -1;
    m_fanPopup->CancelGrace();   // cursor on chip → keep fan alive
    m_fanPopup->Show(FanFlavor::Tabs, chip, it->second.tabs, r.left, r.right, r.top,
                     GetDpiForWindow(m_taskbarOverlay->Hwnd()));
}

// Anchor fan above FolderFan button (same edge-adjacent scheme as ShowFanForChip). Rows = button's subfolders wrapped as Tab{name, false}.
void HostWindow::ShowFanForButton(int buttonIndex)
{
    if (buttonIndex < 0 || !m_fanPopup || !m_taskbarOverlay) return;
    const auto& buttons = m_launcher.Buttons();
    if (buttonIndex >= static_cast<int>(buttons.size())) return;
    const Button& b = buttons[buttonIndex];
    if (b.action != ButtonAction::FolderFan || b.folderEntries.empty()) return;

    RECT r;
    if (!m_taskbarOverlay->ButtonRectScreen(buttonIndex, &r)) return;

    std::vector<Tab> rows;
    rows.reserve(b.folderEntries.size());
    for (const std::wstring& name : b.folderEntries)
        rows.push_back(Tab{ name, false });

    m_fannedButtonIndex = buttonIndex;
    m_fanPopup->CancelGrace();   // cursor on button → keep fan alive
    m_fanPopup->Show(FanFlavor::Folders, nullptr, rows, r.left, r.right, r.top,
                     GetDpiForWindow(m_taskbarOverlay->Hwnd()));
}

void HostWindow::ScanRootAsync(const std::wstring& root)
{
    const HWND dock = m_hwnd;
    std::thread([dock, root]() {
        auto* result = new FolderScanResult{ root, Launcher::ScanImmediateSubfolders(root) };
        if (!PostMessageW(dock, kFolderScanResultMsg, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

void HostWindow::RequestFolderScans()
{
    for (const std::wstring& root : m_launcher.PendingFolderScans())
        ScanRootAsync(root);
}

void HostWindow::RebuildFolderFanWatchers()
{
    m_folderFanWatchers.clear();   // Each dtor stops+joins thread before next Start
    // Drop queued change-notify from old watcher (e.g., root removed from config): stale rescan is harmless but skip it.
    KillTimer(m_hwnd, kFolderFanScanTimer);
    m_pendingFolderFanRescans.clear();
    for (const std::wstring& root : m_launcher.FolderFanRoots())
    {
        auto watcher = std::make_unique<ConfigWatcher>(m_hwnd, kFolderFanChangedMsg);
        watcher->Start(root, L"");   // empty fileName = any-change mode
        m_folderFanWatchers.push_back(std::move(watcher));
    }
}

void HostWindow::QueueFolderFanRescan(const std::wstring& root)
{
    if (std::find(m_pendingFolderFanRescans.begin(), m_pendingFolderFanRescans.end(), root)
            == m_pendingFolderFanRescans.end())
        m_pendingFolderFanRescans.push_back(root);
    SetTimer(m_hwnd, kFolderFanScanTimer, kFolderFanScanMs, nullptr);
}

LRESULT CALLBACK HostWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    // Messages before WM_NCCREATE come with GWLP_USERDATA unset → self==nullptr → fall to DefWindowProcW.
    HostWindow* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<HostWindow*>(cs->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<HostWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // WndProc has no WM_NCCREATE arm → DefWindowProcW returns TRUE (creation proceeds). Never short-circuit to 0.
    if (self)
    {
        return self->WndProc(hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// static
void CALLBACK HostWindow::WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd,
                                        LONG idObject, LONG, DWORD, DWORD) noexcept
{
    if (!hwnd || !s_dockHwnd) return;
    if (event == EVENT_OBJECT_CLOAKED)
    {
        // Win11 flyout closed by cloak in place. System-wide hook: narrow to OBJID_WINDOW of Start/Search process (async delivery already hands foreground back). Routed through kFgLocationMsg debounce.
        if (hHook == s_flyoutCloakHook && idObject == OBJID_WINDOW)
        {
            const std::wstring cloakedProc = ProcessBaseName(hwnd);
            const bool isFlyout = _wcsicmp(cloakedProc.c_str(), L"StartMenuExperienceHost.exe") == 0 ||
                                   _wcsicmp(cloakedProc.c_str(), L"SearchHost.exe") == 0;
            if (isFlyout)
                PostMessageW(s_dockHwnd, kFgLocationMsg, 0, 0);
        }
        return;
    }
    if (event == EVENT_OBJECT_LOCATIONCHANGE)
    {
        // Global fg-fullscreen hook: fires for every window move system-wide, so filter hard to foreground top-level frame resizing itself (possible in-place fullscreen).
        if (hHook == s_fgLocationHook)
        {
            if (idObject == OBJID_WINDOW && hwnd == GetForegroundWindow())
                PostMessageW(s_dockHwnd, kFgLocationMsg, 0, 0);
            return;
        }
        // Taskbar-scoped hook (explorer PID): layout moved → re-measure gap. Window+client only (skip cursor/caret/scrollbar noise).
        if (idObject == OBJID_WINDOW || idObject == OBJID_CLIENT)
            PostMessageW(s_dockHwnd, kRemeasureMsg, 0, 0);
        return;
    }
    if (idObject != OBJID_WINDOW) return;
    TRACE_EVENT("WinEventCallback",
        TraceLoggingUInt32(event, "event_id"),
        TraceLoggingPointer(hwnd, "hwnd"));
    PostMessageW(s_dockHwnd, kWindowEventMsg,
                 static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(hwnd));
}

LRESULT HostWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    // Explorer (re)created taskbar. RegisterWindowMessageW ids aren't compile-time constants. PID-scoped hook is dead, tray HWND/monitor changed → re-scope hook, drop cache, re-derive suppression, re-measure.
    if (m_taskbarCreatedMsg && msg == m_taskbarCreatedMsg)
    {
        HookTaskbarLocation();
        if (m_taskbarOverlay)
        {
            m_taskbarOverlay->InvalidateTaskbarCache();
            UpdateOverlaySuppression();
            m_taskbarOverlay->RequestMeasure();
        }
        return 0;
    }

    switch (msg)
    {
    case WM_DISPLAYCHANGE:
        // Topology change can recreate Shell_TrayWnd (possibly on another monitor) without TaskbarCreated broadcast; drop cached tray.
        if (m_taskbarOverlay)
        {
            m_taskbarOverlay->InvalidateTaskbarCache();
            m_taskbarOverlay->RequestMeasure();  // Gap geometry may move
        }
        return 0;

    case WM_DPICHANGED:
        if (m_taskbarOverlay)
        {
            m_taskbarOverlay->InvalidateTaskbarCache();
            m_taskbarOverlay->RequestMeasure();  // Re-measure at new DPI
        }
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wparam)
        {
            // Kill timers: don't let queued ticks call into dying explorer.
            KillTimer(hwnd, kDebounceTimer);
            KillTimer(hwnd, kSnapshotTimer);
            KillTimer(hwnd, kConfigTimer);
            KillTimer(hwnd, kOverlayTimer);
            KillTimer(hwnd, kSafetyTimer);
            KillTimer(hwnd, kSuppressTimer);
            KillTimer(hwnd, kFolderFanScanTimer);
            PostQuitMessage(0);
        }
        return 0;

    case kWindowEventMsg:
    {
        HWND  target = reinterpret_cast<HWND>(lparam);
        DWORD event  = static_cast<DWORD>(wparam);

        if (event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_SYSTEM_MINIMIZEEND)
        {
            if (m_store.Has(target))
            {
                const bool minimizing = (event == EVENT_SYSTEM_MINIMIZESTART);
                m_store.SetMinimized(target, minimizing);
                if (minimizing)
                    RequestSnapshotDebounced(target);
                if (m_taskbarOverlay) m_taskbarOverlay->RefreshContent();  // Chip set changed
            }
            return 0;
        }

        if (event == EVENT_SYSTEM_FOREGROUND)
        {
            if (m_store.Has(target) && m_tabReader)
                m_tabReader->RequestSnapshot(target);
            UpdateOverlaySuppression();  // Flyout or fullscreen app → hide gap pills
            return 0;
        }

        if (event == EVENT_OBJECT_NAMECHANGE)
        {
            if (m_store.Has(target))
            {
                StoreWindow(m_store, target);
                RequestSnapshotDebounced(target);
                if (m_taskbarOverlay) m_taskbarOverlay->RefreshContent();  // Chip title changed
            }
            return 0;
        }

        // Coalesce create/show/hide burst: record HWND, restart debounce timer.
        if (std::find(m_pendingValidation.begin(), m_pendingValidation.end(), target)
                == m_pendingValidation.end())
            m_pendingValidation.push_back(target);
        SetTimer(hwnd, kDebounceTimer, kDebounceMs, nullptr);
        return 0;
    }

    case WM_TIMER:
        if (wparam == kDebounceTimer)
        {
            KillTimer(hwnd, kDebounceTimer);
            for (HWND target : m_pendingValidation)
            {
                if (IsBrowserFrame(target))
                {
                    StoreWindow(m_store, target);
                }
                else if (m_store.Has(target))
                {
                    m_store.Remove(target);
                }
            }
            m_pendingValidation.clear();
            if (m_taskbarOverlay) m_taskbarOverlay->RefreshContent();  // Removed window may drop chip
        }
        else if (wparam == kSnapshotTimer)
        {
            KillTimer(hwnd, kSnapshotTimer);
            if (m_tabReader)
                for (HWND h : m_pendingSnapshots)
                    m_tabReader->RequestSnapshot(h);
            m_pendingSnapshots.clear();
        }
        else if (wparam == kConfigTimer)
        {
            KillTimer(hwnd, kConfigTimer);
            // Reload rebuilds m_buttons: close fan to avoid stale m_fannedButtonIndex.
            if (m_fanPopup) m_fanPopup->Hide();
            m_fannedButtonIndex = -1;
            m_launcher.Load();
            Paint::SetActiveTheme(m_launcher.ThemeName());  // Live re-skin on config edit
            RequestFolderScans();
            RebuildFolderFanWatchers();
            // Re-measure: new pill set may change which chips/pills fit gap.
            if (m_taskbarOverlay)
            {
                m_taskbarOverlay->RefreshContent();  // Repaint with new theme (geometry unchanged)
                m_taskbarOverlay->RequestMeasure();
            }
        }
        else if (wparam == kOverlayTimer)
        {
            KillTimer(hwnd, kOverlayTimer);  // One-shot debounce
            if (m_taskbarOverlay) m_taskbarOverlay->RequestMeasure();
        }
        else if (wparam == kSuppressTimer)
        {
            KillTimer(hwnd, kSuppressTimer);  // One-shot debounce for fullscreen resize burst
            UpdateOverlaySuppression();
        }
        else if (wparam == kFolderFanScanTimer)
        {
            KillTimer(hwnd, kFolderFanScanTimer);  // One-shot debounce for change-burst (e.g., git clone)
            for (const std::wstring& root : m_pendingFolderFanRescans)
                ScanRootAsync(root);
            m_pendingFolderFanRescans.clear();
        }
        else if (wparam == kSafetyTimer)  // Periodic; NOT killed here
        {
            // Self-heal dropped LOCATIONCHANGE hook: if explorer's PID unresolvable at Create/TaskbarCreated, hook is null. Re-hook once resolves (no-op when already hooked).
            if (!m_winEventHookLocation) HookTaskbarLocation();
            // Backstop: global fg-location hook drives zero-latency suppression. Re-derive every tick to self-heal missed events (flyout dismiss, filtered LOCATIONCHANGE). UpdateOverlaySuppression early-outs with no repaint if unchanged.
            UpdateOverlaySuppression();
            if (m_taskbarOverlay)
            {
                // Self-heal stuck shown-but-empty overlay (lost kApplyGapMsg leaves visible+sized). Re-assert HWND_TOPMOST against taskbar re-layout z-occlusion. Recover stuck-hidden transient.
                m_taskbarOverlay->ReassertVisibility();
                if (!m_overlaySuppressed && !m_taskbarOverlay->Shown())
                    m_taskbarOverlay->RequestMeasure();
            }
        }
        return 0;

    case kRemeasureMsg:
        // Coalesce LOCATIONCHANGE burst into one measure after quiet period.
        SetTimer(hwnd, kOverlayTimer, kOverlayMs, nullptr);
        return 0;

    case kFgLocationMsg:
        // Coalesce fullscreen-transition resize burst into suppression re-derive after quiet period (window reaches final rect).
        SetTimer(hwnd, kSuppressTimer, kSuppressMs, nullptr);
        return 0;

    case kChipHoverMsg:
    {
        // Hovered chip: HWND → show fan; 0 → grace-close (fan's mouse-move cancels if we cross in).
        const HWND chip = reinterpret_cast<HWND>(wparam);
        if (chip)
            ShowFanForChip(chip);
        else if (m_fanPopup)
            m_fanPopup->BeginGrace();
        return 0;
    }

    case kChipClickMsg:
        // Chip clicked → restore + foreground window.
        RestoreWindow(reinterpret_cast<HWND>(wparam));
        return 0;

    case kButtonHoverMsg:
    {
        // Hovered FolderFan button (fires only when no chip hovered; see TaskbarOverlayWindow::WM_NCHITTEST): index → show fan; -1 → grace-close.
        const int idx = static_cast<int>(wparam);
        if (idx >= 0)
            ShowFanForButton(idx);
        else if (m_fanPopup)
            m_fanPopup->BeginGrace();
        return 0;
    }

    case kFolderFanChangedMsg:
    {
        // FolderFan root changed on disk (any-change ConfigWatcher): coalesce burst into one rescan per root.
        std::unique_ptr<std::wstring> root(reinterpret_cast<std::wstring*>(lparam));
        if (root)
            QueueFolderFanRescan(*root);
        else
            SetTimer(hwnd, kFolderFanScanTimer, kFolderFanScanMs, nullptr);
        return 0;
    }

    case WM_HOTKEY:
        if (wparam == kQuitHotkeyId) DestroyWindow(hwnd);  // → WM_DESTROY teardown
        return 0;

    case WM_POWERBROADCAST:
        // Resume from sleep: re-measure stale task-list rect. Backstop for FolderFan freshness (change-notify can miss events during sleep). Routed through same debounce as kFolderFanChangedMsg (coalesces multiple broadcasts).
        if (wparam == PBT_APMRESUMEAUTOMATIC)
        {
            SetTimer(hwnd, kOverlayTimer, kResumeRemeasureMs, nullptr);
            for (const std::wstring& root : m_launcher.FolderFanRoots())
                QueueFolderFanRescan(root);
        }
        return TRUE;

    case kConfigChangedMsg:
        // Coalesce editor's multi-write burst into one reload.
        SetTimer(hwnd, kConfigTimer, kConfigMs, nullptr);
        return 0;

    case kTabSnapshotMsg:
    {
        auto* payload = reinterpret_cast<TabSnapshot*>(lparam);
        if (payload)
        {
#ifdef _DEBUG
            wchar_t dbg[512];
            // Tab title from UIA Name with no length cap: use _TRUNCATE to avoid assert/crash on overflow (UI thread).
            _snwprintf_s(dbg, _countof(dbg), _TRUNCATE, L"[TabReader] hwnd=%p tabs=%d failed=%d\n",
                       reinterpret_cast<void*>(payload->hwnd),
                       static_cast<int>(payload->tabs.size()),
                       payload->failed ? 1 : 0);
            OutputDebugStringW(dbg);
            for (const auto& tab : payload->tabs)
            {
                _snwprintf_s(dbg, _countof(dbg), _TRUNCATE, L"  %s\n", tab.title.c_str());
                OutputDebugStringW(dbg);
            }
#endif
            if (payload->failed)
                m_store.MarkTabsStale(payload->hwnd);
            else
                m_store.SetTabs(payload->hwnd, std::move(payload->tabs));
            delete payload;
        }
        return 0;
    }

    case kFolderScanResultMsg:
    {
        std::unique_ptr<FolderScanResult> result(reinterpret_cast<FolderScanResult*>(lparam));
        if (result)
        {
            const std::wstring root = result->root;
            m_launcher.ApplyFolderScan(root, std::move(result->entries));
            // Fan open on this root: update in place (no need to wait for next hover).
            if (m_fanPopup && m_fanPopup->Visible() && m_fannedButtonIndex >= 0)
            {
                const auto& buttons = m_launcher.Buttons();
                if (m_fannedButtonIndex < static_cast<int>(buttons.size()) &&
                    buttons[m_fannedButtonIndex].target == root)
                    ShowFanForButton(m_fannedButtonIndex);
            }
        }
        return 0;
    }

    case kFanActivateMsg:
    {
        // wparam = target browser HWND (Tabs flavor) or 0 (Folders); lparam = FanActivateRequest* (owned here — delete below).
        const HWND target = reinterpret_cast<HWND>(wparam);
        std::unique_ptr<FanActivateRequest> req(reinterpret_cast<FanActivateRequest*>(lparam));
        if (req && req->flavor == FanFlavor::Folders)
        {
            const auto& buttons = m_launcher.Buttons();
            if (m_fannedButtonIndex >= 0 && m_fannedButtonIndex < static_cast<int>(buttons.size()))
            {
                const Button& b = buttons[m_fannedButtonIndex];
                if (b.action == ButtonAction::FolderFan &&
                    req->rowIndex >= 0 && req->rowIndex < static_cast<int>(b.folderEntries.size()))
                    m_launcher.LaunchFolder(b.target + L"\\" + b.folderEntries[req->rowIndex]);
            }
        }
        else if (req)
        {
            // Tabs flavor: resolve title NOW (pre-restore), restore+foreground FIRST (R1: patterns on background/iconic unreliable), hand worker title to re-match post-restore tree.
            const auto& all = m_store.All();
            auto it = all.find(target);
            if (it != all.end() && req->rowIndex >= 0 &&
                req->rowIndex < static_cast<int>(it->second.tabs.size()))
            {
                std::wstring wanted = it->second.tabs[req->rowIndex].title;
                RestoreWindow(target);
                const long long tRestore = trace::NowUs();   // C: restore/show issued
                if (m_tabReader)
                    m_tabReader->RequestActivate(target, std::move(wanted), req->rowIndex,
                                                 req->tClickUs, tRestore);
            }
        }
        if (m_fanPopup) m_fanPopup->Hide();   // Close on click (window/launch is the feedback)
        return 0;
    }

    case kTabActivateResultMsg:
    {
        auto* payload = reinterpret_cast<TabActivateResult*>(lparam);
        if (payload)
        {
#ifdef _DEBUG
            wchar_t dbg[256];
            swprintf_s(dbg, L"[Activate] hwnd=%p outcome=%d matchedIndex=%d freshTabs=%d\n",
                       reinterpret_cast<void*>(payload->hwnd),
                       static_cast<int>(payload->outcome),
                       payload->matchedIndex,
                       static_cast<int>(payload->freshTabs.size()));
            OutputDebugStringW(dbg);
#endif
            // Refresh fan from worker's re-snapshot.
            if (!payload->freshTabs.empty() && m_store.Has(payload->hwnd))
                m_store.SetTabs(payload->hwnd, std::move(payload->freshTabs));
            delete payload;
        }
        return 0;
    }

    case WM_DESTROY:
        // Kill timers first: no tick may fire into object we're about to reset.
        KillTimer(hwnd, kDebounceTimer);
        KillTimer(hwnd, kSnapshotTimer);
        KillTimer(hwnd, kConfigTimer);
        KillTimer(hwnd, kOverlayTimer);
        KillTimer(hwnd, kSafetyTimer);
        KillTimer(hwnd, kSuppressTimer);
        KillTimer(hwnd, kFolderFanScanTimer);
        UnregisterHotKey(hwnd, kQuitHotkeyId);
        // Join every worker before unhooking (worker post lands on s_dockHwnd).
        m_configWatcher.reset();
        m_folderFanWatchers.clear();
        m_tabReader.reset();
        m_fanPopup.reset();
        m_taskbarOverlay.reset();
        if (m_winEventHookFgLocation) { UnhookWinEvent(m_winEventHookFgLocation); m_winEventHookFgLocation = nullptr; s_fgLocationHook = nullptr; }
        if (m_winEventHookFlyoutCloak) { UnhookWinEvent(m_winEventHookFlyoutCloak); m_winEventHookFlyoutCloak = nullptr; s_flyoutCloakHook = nullptr; }
        if (m_winEventHookLocation)   { UnhookWinEvent(m_winEventHookLocation);   m_winEventHookLocation   = nullptr; }
        if (m_winEventHookForeground) { UnhookWinEvent(m_winEventHookForeground); m_winEventHookForeground = nullptr; }
        if (m_winEventHookNameChange) { UnhookWinEvent(m_winEventHookNameChange); m_winEventHookNameChange = nullptr; }
        if (m_winEventHookMinimize)   { UnhookWinEvent(m_winEventHookMinimize);   m_winEventHookMinimize   = nullptr; }
        if (m_winEventHook)           { UnhookWinEvent(m_winEventHook);           m_winEventHook           = nullptr; }
        s_dockHwnd = nullptr;
        PostQuitMessage(0);
        m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
