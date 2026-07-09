#include "HostWindow.h"
#include "WindowMonitor.h"
#include "PaintUtil.h"
#include "Trace.h"
#include <algorithm>

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
    constexpr UINT    kFanActivateMsg   = WM_APP + 7;  // fan(UI) → dock: a tab row was clicked
    constexpr UINT    kTabActivateResultMsg = WM_APP + 8;  // TabReader worker → dock: activate outcome
    constexpr UINT    kChipHoverMsg     = WM_APP + 9;  // overlay → dock: hovered chip changed (HWND, or 0)
    constexpr UINT    kChipClickMsg     = WM_APP + 10; // overlay → dock: a chip was clicked (restore its window)
    // In-place fullscreen (browser F11, video element, borderless game) keeps the same
    // foreground HWND, so it fires NO EVENT_SYSTEM_FOREGROUND — only its own rect change. A
    // set-once global LOCATIONCHANGE hook (see Create) catches it for zero-latency suppression;
    // kSuppressTimer debounces the transition's resize burst so we read the final rect.
    constexpr UINT     kFgLocationMsg   = WM_APP + 11;
    constexpr UINT_PTR kSuppressTimer   = 7;
    constexpr UINT     kSuppressMs      = 120;
    // 5b.3: re-measure the gap on task-list EVENT_OBJECT_LOCATIONCHANGE, debounced
    // through this one-shot (RequestMeasure is already coalesced by the overlay worker).
    constexpr UINT_PTR kOverlayTimer    = 5;
    constexpr UINT    kOverlayMs        = 200;
    constexpr UINT    kResumeRemeasureMs = 500;  // let the shell settle after wake before re-measuring
    // Low-frequency safety re-check: self-heals the overlay from any stuck-hidden /
    // stuck-suppressed state whose clearing event was missed (Start self-dismiss, a missed
    // fullscreen enter/exit, a transient invalid with no follow-up LOCATIONCHANGE).
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

    void StoreWindow(Store& store, HWND hwnd)
    {
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, _countof(title));
        store.Set(hwnd, title);
    }

    // Basename of the process owning hwnd (e.g. L"StartMenuExperienceHost.exe"), or empty.
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
}

HostWindow::~HostWindow()
{
    // Covers GetMessage==-1 abnormal exit: destructor fires, window not yet
    // destroyed, so we destroy it here to guarantee an orderly WM_DESTROY teardown.
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
    wc.hbrBackground = nullptr; // WM_ERASEBKGND returns 1; WM_PAINT owns all drawing
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc))
    {
        return false;
    }

    // Never-shown coordinator: a top-level WS_POPUP (NOT HWND_MESSAGE — it needs
    // WM_DISPLAYCHANGE / WM_ENDSESSION broadcasts) that owns the pump and hooks but
    // paints nothing. TOOLWINDOW: no taskbar button / Alt-Tab. NOACTIVATE: never
    // steals foreground. Stays 1x1 at origin; the chips render in the overlay.
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

    // m_hwnd already set by WM_NCCREATE, but belt + suspenders.
    m_hwnd = hwnd;

    // Explorer broadcasts this to every top-level window when it (re)creates the taskbar
    // (crash-restart, some DPI/theme changes). Our hidden host is top-level → receives it.
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // The dock strip (and its right-click-to-quit) is gone; register a global hatch so the
    // app is always closable even if the gap overlay is suppressed. Best-effort.
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
    // Pre-warm UIA snapshot on foreground: a minimized window's UIA tree is
    // stripped, so MINIMIZESTART fires too late. Snapshot while still visible.
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

    // Stage 5b: host the automation buttons in the taskbar's empty gap. Measure now,
    // then re-measure on a low-frequency timer + on geometry-change events.
    m_taskbarOverlay = std::make_unique<TaskbarOverlayWindow>();
    if (m_taskbarOverlay->Create(instance, &m_launcher, &m_store, hwnd,
                                 kChipClickMsg, kChipHoverMsg))
    {
        if (m_fanPopup) m_taskbarOverlay->SetTopSibling(m_fanPopup->Hwnd());
        m_taskbarOverlay->RequestMeasure();
        SetTimer(hwnd, kOverlayTimer, kOverlayMs, nullptr);  // backstop: guarantees a 2nd verdict if the 1st post is lost
        SetTimer(hwnd, kSafetyTimer, kSafetyMs, nullptr);    // periodic self-heal (see kSafetyTimer)
        // Re-measure when the task list resizes (open/close apps → Win11 center group
        // shifts → gap grows/shrinks). Scope to explorer's PID so we only wake on taskbar
        // layout changes, not every window move system-wide.
        HookTaskbarLocation();
        // Zero-latency in-place-fullscreen watch. A SINGLE system-wide LOCATIONCHANGE hook set
        // ONCE, never re-scoped: the old per-foreground re-scope (unhook+hook on every
        // foreground change) was the churn/crash suspect. The callback filters hard to the
        // current foreground frame; UpdateOverlaySuppression re-checks the live fg + monitor.
        m_winEventHookFgLocation = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        s_fgLocationHook = m_winEventHookFgLocation;
    }

    // Watch the config dir for live edits. Create it first so the watch attaches even
    // before the user writes a config (CreateDirectoryW is a no-op if it exists).
    const std::wstring cfgDir = Launcher::ConfigDir();
    if (!cfgDir.empty())
    {
        CreateDirectoryW(cfgDir.c_str(), nullptr);
        m_configWatcher = std::make_unique<ConfigWatcher>(hwnd, kConfigChangedMsg);
        m_configWatcher->Start(cfgDir, Launcher::ConfigFileName());
    }

    return true;
}

// A fullscreen app fills rcMonitor (a merely-maximized window stops at rcWork, above
// the taskbar) on the same monitor as the taskbar overlay. The hidden host is 1x1 at
// origin, so derive the monitor from the overlay's taskbar. ±2px tolerance for scaling.
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
    // ±2px: DWM stretch of a DPI-unaware fullscreen app can round up to 2px off an
    // edge at fractional scaling (175%); a maximized window still differs by ≥ the
    // resize border + taskbar height, so no false positive.
    auto within2 = [](LONG a, LONG b) { return (a > b ? a - b : b - a) <= 2; };
    const RECT& m = mi.rcMonitor;
    return within2(r.left, m.left) && within2(r.top, m.top) &&
           within2(r.right, m.right) && within2(r.bottom, m.bottom);
}

// Start/Search flyouts use DWM cloaking, not move/hide, so closing one emits no taskbar
// LOCATIONCHANGE — a measure-driven overlay would stay stuck hidden. Drive suppression
// off the foreground window (flyout process) + the fullscreen state instead, and kick a
// delayed re-measure on release so the taskbar animation has settled first.
void HostWindow::UpdateOverlaySuppression()
{
    if (!m_taskbarOverlay) return;
    const HWND fg = GetForegroundWindow();
    const std::wstring proc = ProcessBaseName(fg);
    const bool flyoutOpen = (_wcsicmp(proc.c_str(), L"StartMenuExperienceHost.exe") == 0 ||
                             _wcsicmp(proc.c_str(), L"SearchHost.exe") == 0);
    const bool suppress = flyoutOpen || FullscreenOnDockMonitor(fg);
    if (suppress == m_overlaySuppressed) return;
    m_overlaySuppressed = suppress;
    m_taskbarOverlay->SetSuppressed(suppress);
    if (!suppress)
        SetTimer(m_hwnd, kOverlayTimer, kOverlayMs, nullptr);
}

// (Re)scope the LOCATIONCHANGE hook to the live explorer PID. On an explorer restart the
// old hook is dead (its PID is gone), so without this the gap would never re-measure again
// — the overlay would freeze at its last position. Idempotent: unhook-then-hook.
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

// Coalesce snapshot requests: a burst (Win+M minimizing many windows, or a flood
// of NAMECHANGE during page loads) collapses into one flush after 150ms of quiet,
// so the UIA worker doesn't thrash. Foreground pre-warm stays immediate — it must
// beat the minimized window's UIA tree-strip.
void HostWindow::RequestSnapshotDebounced(HWND hwnd)
{
    if (std::find(m_pendingSnapshots.begin(), m_pendingSnapshots.end(), hwnd)
            == m_pendingSnapshots.end())
        m_pendingSnapshots.push_back(hwnd);
    SetTimer(m_hwnd, kSnapshotTimer, kSnapshotMs, nullptr);
}

// Un-minimize and focus. ShowWindowAsync (not ShowWindow) so a hung target's
// queue can't block our UI pump. SetForegroundWindow is restricted when we
// aren't the foreground process; on failure flash the taskbar button instead
// (documented + non-blocking, unlike SwitchToThisWindow/AttachThreadInput).
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

// Anchor the fan above a taskbar chip. The chip's screen rect comes from the overlay
// (same UI thread — direct query, no cross-thread post); the fan grows upward from the
// chip's top edge so chip and fan are edge-adjacent (minimal hover seam).
void HostWindow::ShowFanForChip(HWND chip)
{
    if (!chip || !m_fanPopup || !m_taskbarOverlay) return;
    const auto& all = m_store.All();
    auto it = all.find(chip);
    if (it == all.end()) return;

    RECT r;
    if (!m_taskbarOverlay->ChipRectScreen(chip, &r)) return;
    m_fanPopup->CancelGrace();   // cursor is on a chip → keep the fan alive
    m_fanPopup->Show(chip, it->second.tabs, r.left, r.right, r.top,
                     GetDpiForWindow(m_taskbarOverlay->Hwnd()));
}

LRESULT CALLBACK HostWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    // Messages before WM_NCCREATE (WM_GETMINMAXINFO, WM_NCCALCSIZE) come with
    // GWLP_USERDATA unset -> self==nullptr -> fall to DefWindowProcW. Keep guard.
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

    // WndProc has no WM_NCCREATE arm -> DefWindowProcW return TRUE, creation
    // proceed. Never short-circuit WM_NCCREATE to 0 -> CreateWindow fail.
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
    if (event == EVENT_OBJECT_LOCATIONCHANGE)
    {
        // Global fg-fullscreen hook: it fires for EVERY window move system-wide, so filter hard
        // — only the foreground top-level frame resizing itself (→ possible in-place fullscreen)
        // wakes suppression. UpdateOverlaySuppression re-reads the live foreground + monitor.
        if (hHook == s_fgLocationHook)
        {
            if (idObject == OBJID_WINDOW && hwnd == GetForegroundWindow())
                PostMessageW(s_dockHwnd, kFgLocationMsg, 0, 0);
            return;
        }
        // Taskbar-scoped hook (explorer PID): layout moved → re-measure gap. Window + client
        // only (XAML task list resizes as OBJID_CLIENT); skip cursor/caret/scrollbar noise.
        if (idObject == OBJID_WINDOW || idObject == OBJID_CLIENT)
            PostMessageW(s_dockHwnd, kRemeasureMsg, 0, 0);
        return;
    }
    if (idObject != OBJID_WINDOW) return;
    PostMessageW(s_dockHwnd, kWindowEventMsg,
                 static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(hwnd));
}

LRESULT HostWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    // Explorer (re)created the taskbar. RegisterWindowMessageW ids aren't compile-time
    // constants, so this can't be a switch arm. The old PID-scoped LOCATIONCHANGE hook is
    // dead and the tray HWND/monitor changed — re-scope the hook, drop the cached monitor,
    // re-derive suppression, and re-measure so the overlay re-establishes on the new taskbar.
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
        // A topology change can recreate Shell_TrayWnd (possibly on another monitor)
        // without a TaskbarCreated broadcast — drop the cached tray so the fullscreen
        // monitor check can't consult a stale/recycled HWND.
        if (m_taskbarOverlay)
        {
            m_taskbarOverlay->InvalidateTaskbarCache();
            m_taskbarOverlay->RequestMeasure();  // gap geometry may move
        }
        return 0;

    case WM_DPICHANGED:
        if (m_taskbarOverlay)
        {
            m_taskbarOverlay->InvalidateTaskbarCache();
            m_taskbarOverlay->RequestMeasure();  // re-measure at new DPI
        }
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wparam)
        {
            // Don't let a queued tick call into a dying explorer (measure/snapshot/config).
            KillTimer(hwnd, kDebounceTimer);
            KillTimer(hwnd, kSnapshotTimer);
            KillTimer(hwnd, kConfigTimer);
            KillTimer(hwnd, kOverlayTimer);
            KillTimer(hwnd, kSafetyTimer);
            KillTimer(hwnd, kSuppressTimer);
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
                if (m_taskbarOverlay) m_taskbarOverlay->RefreshContent();  // chip set changed
            }
            return 0;
        }

        if (event == EVENT_SYSTEM_FOREGROUND)
        {
            if (m_store.Has(target) && m_tabReader)
                m_tabReader->RequestSnapshot(target);
            UpdateOverlaySuppression();  // Start/Search flyout or fullscreen app → hide gap pills
            return 0;
        }

        if (event == EVENT_OBJECT_NAMECHANGE)
        {
            if (m_store.Has(target))
            {
                StoreWindow(m_store, target);
                RequestSnapshotDebounced(target);
                if (m_taskbarOverlay) m_taskbarOverlay->RefreshContent();  // chip title changed
            }
            return 0;
        }

        // Coalesce create/show/hide burst: record HWND, restart 200ms timer.
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
            if (m_taskbarOverlay) m_taskbarOverlay->RefreshContent();  // a removed window may drop a chip
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
            m_launcher.Load();
            Paint::SetActiveTheme(m_launcher.ThemeName());  // live re-skin on config edit
            // Re-measure: the new pill set may change which chips/pills fit the gap.
            if (m_taskbarOverlay)
            {
                m_taskbarOverlay->RefreshContent();  // repaint with the new theme even if geometry is unchanged
                m_taskbarOverlay->RequestMeasure();
            }
        }
        else if (wparam == kOverlayTimer)
        {
            KillTimer(hwnd, kOverlayTimer);  // one-shot debounce
            if (m_taskbarOverlay) m_taskbarOverlay->RequestMeasure();
        }
        else if (wparam == kSuppressTimer)
        {
            KillTimer(hwnd, kSuppressTimer);  // one-shot debounce for the fg-fullscreen resize burst
            UpdateOverlaySuppression();
        }
        else if (wparam == kSafetyTimer)  // periodic; NOT killed here
        {
            // Self-heal a dropped LOCATIONCHANGE hook: if explorer's PID was momentarily
            // unresolvable at Create or at a TaskbarCreated restart, the hook is null and
            // the gap would only re-measure on this slow tick. Re-hook once it resolves.
            // Costs a FindTaskbar only while the hook is actually null (normal case: no-op).
            if (!m_winEventHookLocation) HookTaskbarLocation();
            // Backstop only: the global fg-location hook drives zero-latency suppression; this
            // re-derives every tick to self-heal a missed event (flyout self-dismiss, a
            // fullscreen enter/exit whose LOCATIONCHANGE was filtered out). One ProcessBaseName
            // OpenProcess per tick; UpdateOverlaySuppression early-outs with no repaint when
            // unchanged.
            UpdateOverlaySuppression();
            if (m_taskbarOverlay)
            {
                // Self-heal the stuck shown-but-empty overlay (a lost kApplyGapMsg leaves it
                // visible+sized but painting nothing) and re-assert HWND_TOPMOST against a
                // taskbar re-layout that z-occluded it. Then recover a stuck-hidden transient.
                m_taskbarOverlay->ReassertVisibility();
                if (!m_overlaySuppressed && !m_taskbarOverlay->Shown())
                    m_taskbarOverlay->RequestMeasure();
            }
        }
        return 0;

    case kRemeasureMsg:
        // Coalesce a LOCATIONCHANGE burst into one measure after 200ms of quiet.
        SetTimer(hwnd, kOverlayTimer, kOverlayMs, nullptr);
        return 0;

    case kFgLocationMsg:
        // Coalesce a fullscreen-transition resize burst into one suppression re-derive after
        // kSuppressMs of quiet — the window reaches its final rect before we check monitor fill.
        SetTimer(hwnd, kSuppressTimer, kSuppressMs, nullptr);
        return 0;

    case kChipHoverMsg:
    {
        // Hovered chip changed. A real HWND → open the fan above that chip; 0 (cursor left
        // the overlay) → grace-close (the fan's own mouse-move cancels it if we cross in).
        const HWND chip = reinterpret_cast<HWND>(wparam);
        if (chip)
            ShowFanForChip(chip);
        else if (m_fanPopup)
            m_fanPopup->BeginGrace();
        return 0;
    }

    case kChipClickMsg:
        // A taskbar chip was clicked → restore + foreground that window.
        RestoreWindow(reinterpret_cast<HWND>(wparam));
        return 0;

    case WM_HOTKEY:
        if (wparam == kQuitHotkeyId) DestroyWindow(hwnd);  // → WM_DESTROY teardown
        return 0;

    case WM_POWERBROADCAST:
        // Resume from sleep can leave a stale task-list rect; re-measure after a beat.
        if (wparam == PBT_APMRESUMEAUTOMATIC)
            SetTimer(hwnd, kOverlayTimer, kResumeRemeasureMs, nullptr);
        return TRUE;

    case kConfigChangedMsg:
        // Coalesce an editor's multi-write burst into one reload.
        SetTimer(hwnd, kConfigTimer, kConfigMs, nullptr);
        return 0;

    case kTabSnapshotMsg:
    {
        auto* payload = reinterpret_cast<TabSnapshot*>(lparam);
        if (payload)
        {
#ifdef _DEBUG
            wchar_t dbg[512];
            swprintf_s(dbg, L"[TabReader] hwnd=%p tabs=%d failed=%d\n",
                       reinterpret_cast<void*>(payload->hwnd),
                       static_cast<int>(payload->tabs.size()),
                       payload->failed ? 1 : 0);
            OutputDebugStringW(dbg);
            for (const auto& tab : payload->tabs)
            {
                swprintf_s(dbg, L"  %s\n", tab.title.c_str());
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

    case kFanActivateMsg:
    {
        // wparam = target browser HWND; lparam = FanActivateRequest* (owned here — delete
        // below). Resolve the wanted title NOW (pre-restore Store state), then
        // restore+foreground FIRST (R1: patterns on a background/iconic window are
        // unreliable) and hand the worker a title to re-match against the post-restore tree.
        const HWND target = reinterpret_cast<HWND>(wparam);
        std::unique_ptr<FanActivateRequest> req(reinterpret_cast<FanActivateRequest*>(lparam));
        const auto& all = m_store.All();
        auto it = all.find(target);
        if (req && it != all.end() && req->tabIndex >= 0 &&
            req->tabIndex < static_cast<int>(it->second.tabs.size()))
        {
            std::wstring wanted = it->second.tabs[req->tabIndex].title;
            RestoreWindow(target);
            const long long tRestore = trace::NowUs();   // C: restore/show issued
            if (m_tabReader)
                m_tabReader->RequestActivate(target, std::move(wanted), req->tabIndex,
                                             req->tClickUs, tRestore);
        }
        if (m_fanPopup) m_fanPopup->Hide();   // close on click (committed; window coming forward is the feedback)
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
            // Refresh the fan from the same re-snapshot the worker just took.
            if (!payload->freshTabs.empty() && m_store.Has(payload->hwnd))
                m_store.SetTabs(payload->hwnd, std::move(payload->freshTabs));
            delete payload;
        }
        return 0;
    }

    case WM_DESTROY:
        // Kill timers first: no tick may fire into an object we're about to reset.
        KillTimer(hwnd, kDebounceTimer);
        KillTimer(hwnd, kSnapshotTimer);
        KillTimer(hwnd, kConfigTimer);
        KillTimer(hwnd, kOverlayTimer);
        KillTimer(hwnd, kSafetyTimer);
        KillTimer(hwnd, kSuppressTimer);
        UnregisterHotKey(hwnd, kQuitHotkeyId);
        // Then join every worker before unhooking (a worker post lands on s_dockHwnd).
        m_configWatcher.reset();
        m_tabReader.reset();
        m_fanPopup.reset();
        m_taskbarOverlay.reset();
        if (m_winEventHookFgLocation) { UnhookWinEvent(m_winEventHookFgLocation); m_winEventHookFgLocation = nullptr; s_fgLocationHook = nullptr; }
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
