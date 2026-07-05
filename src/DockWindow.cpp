#include "DockWindow.h"
#include "Renderer.h"
#include "PaintUtil.h"
#include "WindowMonitor.h"
#include <windowsx.h>
#include <algorithm>

#pragma comment(lib, "shell32.lib")

namespace
{
    constexpr wchar_t kClassName[]      = L"BrowserShellOsDockWindow";
    constexpr UINT    kCallbackMsg      = WM_APP + 1;
    constexpr UINT    kWindowEventMsg   = WM_APP + 2;
    constexpr UINT    kTabSnapshotMsg   = WM_APP + 3;
    constexpr UINT    kConfigChangedMsg = WM_APP + 4;
    constexpr UINT_PTR kDebounceTimer   = 1;
    constexpr UINT    kDebounceMs       = 200;
    constexpr UINT_PTR kHoverTimer      = 2;
    constexpr UINT    kHoverMs          = 250;
    constexpr UINT_PTR kSnapshotTimer   = 3;
    constexpr UINT    kSnapshotMs       = 150;
    constexpr UINT_PTR kConfigTimer     = 4;
    constexpr UINT    kConfigMs         = 300;
    constexpr UINT    kRemeasureMsg     = WM_APP + 5;  // LOCATIONCHANGE hook → debounce
    constexpr UINT    kGapStateMsg      = WM_APP + 6;  // overlay → dock: gap host active?
    constexpr UINT    kFanActivateMsg   = WM_APP + 7;  // fan(UI) → dock: a tab row was clicked
    constexpr UINT    kTabActivateResultMsg = WM_APP + 8;  // TabReader worker → dock: activate outcome
    // 5b.3: re-measure the gap on task-list EVENT_OBJECT_LOCATIONCHANGE, debounced
    // through this one-shot (RequestMeasure is already coalesced by the overlay worker).
    constexpr UINT_PTR kOverlayTimer    = 5;
    constexpr UINT    kOverlayMs        = 200;
    constexpr UINT    kResumeRemeasureMs = 500;  // let the shell settle after wake before re-measuring

    // Single-instance: safe to keep a plain HWND here for the WinEventProc callback.
    // Written on the UI thread in Create/WM_DESTROY; read on the same thread in WinEventProc
    // (OUTOFCONTEXT delivers on our message-pump thread). No cross-thread access.
    HWND s_dockHwnd = nullptr;

    void StoreWindow(Store& store, HWND hwnd)
    {
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, _countof(title));
        store.Set(hwnd, title);
    }
}

DockWindow::~DockWindow()
{
    // Covers GetMessage==-1 abnormal exit: destructor fires, window not yet
    // destroyed, so we destroy it here to guarantee WM_DESTROY→AppBarRemove.
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
    }
}

bool DockWindow::Create(HINSTANCE instance)
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

    // WS_EX_TOOLWINDOW: no taskbar button, no Alt-Tab. WS_EX_TOPMOST: stay
    // visible; AppBar alone does not guarantee z-order. WS_EX_NOACTIVATE: dock
    // never steals foreground. Step 1.6 drops TOPMOST for fullscreen apps.
    // Initial position is (0,0,1,1) — AppBarSetPos moves it before ShowWindow.
    const HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClassName,
        L"browser_shell_os dock",
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

    // Register AppBar before Show so position negotiation runs first.
    m_abd        = {};
    m_abd.cbSize = sizeof(m_abd);
    m_abd.hWnd   = hwnd;
    m_abd.uCallbackMessage = kCallbackMsg;
    if (!SHAppBarMessage(ABM_NEW, &m_abd))
    {
        DestroyWindow(hwnd);
        return false;
    }
    m_appBarRegistered = true;

    // Negotiate and commit position before making window visible.
    AppBarSetPos(hwnd);

    for (HWND h : ScanBrowserFrames())
    {
        StoreWindow(m_store, h);
        if (IsIconic(h))
            m_store.SetMinimized(h, true);
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

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

    // Stage 5b: host the automation buttons in the taskbar's empty gap. Measure now,
    // then re-measure on a low-frequency timer + on geometry-change events.
    m_taskbarOverlay = std::make_unique<TaskbarOverlayWindow>();
    if (m_taskbarOverlay->Create(instance, &m_launcher, hwnd, kGapStateMsg))
    {
        m_taskbarOverlay->RequestMeasure();
        SetTimer(hwnd, kOverlayTimer, kOverlayMs, nullptr);  // backstop: guarantees a 2nd verdict if the 1st post is lost
        // Re-measure when the task list resizes. Scope to explorer's PID so we only
        // wake on taskbar layout changes, not every window move system-wide.
        DWORD explorerPid = 0;
        if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr))
            GetWindowThreadProcessId(tray, &explorerPid);
        if (explorerPid)
            m_winEventHookLocation = SetWinEventHook(
                EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                nullptr, WinEventProc, explorerPid, 0, WINEVENT_OUTOFCONTEXT);
    }
    else
    {
        m_gapResolved = true;  // no overlay → dock is the sole host from the start
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

HWND DockWindow::CardAt(POINT ptClient) const
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    for (const Renderer::CardHit& c : Renderer::CardLayout(rc, GetDpiForWindow(m_hwnd), m_store))
        if (PtInRect(&c.rect, ptClient)) return c.hwnd;
    return nullptr;
}

// Single host: the dock strip shows the buttons only once the overlay has reported
// its verdict AND isn't hosting them itself (before the first verdict, neither shows
// — avoids a startup frame where both paint buttons).
const std::vector<Button>& DockWindow::DockButtons() const
{
    static const std::vector<Button> none;
    return (m_gapResolved && !m_gapActive) ? m_launcher.Buttons() : none;
}

int DockWindow::ButtonAt(POINT ptClient) const
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    for (const Renderer::ButtonHit& h :
         Renderer::ButtonLayout(rc, GetDpiForWindow(m_hwnd), DockButtons()))
        if (PtInRect(&h.rect, ptClient)) return h.index;
    return -1;
}

void DockWindow::ClearHover()
{
    KillTimer(m_hwnd, kHoverTimer);
    m_hoverCard = nullptr;
    if (m_fanPopup) m_fanPopup->Hide();
}

// Grace-variant of ClearHover: defer the fan's close instead of hiding at once,
// so the cursor can cross the card→fan seam without the fan flickering shut.
void DockWindow::BeginHoverGrace()
{
    KillTimer(m_hwnd, kHoverTimer);
    m_hoverCard = nullptr;
    if (m_fanPopup) m_fanPopup->BeginGrace();
}

// Coalesce snapshot requests: a burst (Win+M minimizing many windows, or a flood
// of NAMECHANGE during page loads) collapses into one flush after 150ms of quiet,
// so the UIA worker doesn't thrash. Foreground pre-warm stays immediate — it must
// beat the minimized window's UIA tree-strip.
void DockWindow::RequestSnapshotDebounced(HWND hwnd)
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
void DockWindow::RestoreWindow(HWND target)
{
    if (!target) return;
    ShowWindowAsync(target, SW_RESTORE);
    if (!SetForegroundWindow(target))
    {
        FLASHWINFO fi = { sizeof(fi), target, FLASHW_TRAY, 3, 0 };
        FlashWindowEx(&fi);
    }
}

// Map a hovered card (client coords) to screen anchor and open the fan above it.
void DockWindow::ShowFanFor(HWND card)
{
    if (!card || !m_fanPopup) return;

    const auto& all = m_store.All();
    auto it = all.find(card);
    if (it == all.end()) return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const UINT dpi = GetDpiForWindow(m_hwnd);

    for (const Renderer::CardHit& c : Renderer::CardLayout(rc, dpi, m_store))
    {
        if (c.hwnd != card) continue;
        // Anchor to the dock's top edge (client y=0), not the padded card top.
        POINT pl = { c.rect.left,  0 };
        POINT pr = { c.rect.right, 0 };
        ClientToScreen(m_hwnd, &pl);
        ClientToScreen(m_hwnd, &pr);
        m_fanPopup->Show(card, it->second.tabs, pl.x, pr.x, pl.y, dpi);
        return;
    }
}

// Reserved height grows with the minimized-window count: one kBandHeightDip band
// each (plus inter-band pad), clamped to [1, kMaxBands]. One band when idle so the
// empty-state message still has a strip.
int DockWindow::DockHeightPx(UINT dpi) const
{
    int n = 0;
    for (const auto& [h, w] : m_store.All())
        if (w.minimized) ++n;
    n = std::clamp(n, 1, Paint::kMaxBands);
    const int band = MulDiv(Paint::kBandHeightDip, dpi, 96);
    const int pad  = MulDiv(Paint::kBandPadDip, dpi, 96);
    return n * band + (n + 1) * pad;
}

void DockWindow::AppBarSetPos(HWND hwnd)
{
    // A minimize event queued before WM_ENDSESSION can dispatch after AppBarRemove
    // during the session-end drain; don't QUERYPOS/SETPOS an unregistered AppBar.
    if (!m_appBarRegistered) return;

    // Primary monitor physical rect (rcMonitor, NOT rcWork — shell arbitrates via QUERYPOS).
    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hmon, &mi);

    // Scale dock height from 96-DPI baseline to this window's per-monitor DPI.
    // Guard against 0 (before first WM_NCCREATE, or pre-Win10) → treat as 96.
    const UINT rawDpi = GetDpiForWindow(hwnd);
    const int dpi = rawDpi ? static_cast<int>(rawDpi) : 96;
    m_dockHeight = DockHeightPx(static_cast<UINT>(dpi));

    // Propose: full monitor width, bottom-anchored.
    m_abd.hWnd  = hwnd;
    m_abd.uEdge = ABE_BOTTOM;
    m_abd.rc    = { mi.rcMonitor.left,
                    mi.rcMonitor.bottom - m_dockHeight,
                    mi.rcMonitor.right,
                    mi.rcMonitor.bottom };

    // Shell shrinks/moves rc so we don't overlap the taskbar or other appbars.
    SHAppBarMessage(ABM_QUERYPOS, &m_abd);

    // Re-anchor top relative to adjusted bottom (shell may have moved bottom up).
    m_abd.rc.top = m_abd.rc.bottom - m_dockHeight;

    // Commit reservation.
    SHAppBarMessage(ABM_SETPOS, &m_abd);

    // Move window to exactly the negotiated rect.
    SetWindowPos(hwnd, nullptr,
                 m_abd.rc.left,
                 m_abd.rc.top,
                 m_abd.rc.right  - m_abd.rc.left,
                 m_abd.rc.bottom - m_abd.rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK DockWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    // Messages before WM_NCCREATE (WM_GETMINMAXINFO, WM_NCCALCSIZE) come with
    // GWLP_USERDATA unset -> self==nullptr -> fall to DefWindowProcW. Keep guard.
    DockWindow* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<DockWindow*>(cs->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // WndProc has no WM_NCCREATE arm -> DefWindowProcW return TRUE, creation
    // proceed. Never short-circuit WM_NCCREATE to 0 -> CreateWindow fail.
    if (self)
    {
        return self->WndProc(hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void DockWindow::AppBarRemove(HWND hwnd)
{
    if (!m_appBarRegistered) return;
    m_appBarRegistered = false;
    m_abd.hWnd = hwnd; // use WndProc param, not m_hwnd (may already be null)
    SHAppBarMessage(ABM_REMOVE, &m_abd);
}

// static
void CALLBACK DockWindow::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                        LONG idObject, LONG, DWORD, DWORD) noexcept
{
    if (!hwnd || !s_dockHwnd) return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE)  // taskbar layout moved → re-measure gap
    {
        // Window + client only (XAML task list resizes as OBJID_CLIENT); skip
        // cursor/caret/scrollbar noise.
        if (idObject == OBJID_WINDOW || idObject == OBJID_CLIENT)
            PostMessageW(s_dockHwnd, kRemeasureMsg, 0, 0);
        return;
    }
    if (idObject != OBJID_WINDOW) return;
    PostMessageW(s_dockHwnd, kWindowEventMsg,
                 static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(hwnd));
}

LRESULT DockWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        Renderer::Paint(hdc, rc, GetDpiForWindow(hwnd), m_store, DockButtons());
        EndPaint(hwnd, &ps);
        return 0;
    }

    case kCallbackMsg:
        if (wparam == ABN_POSCHANGED || wparam == ABN_STATECHANGE)
        {
            AppBarSetPos(hwnd);
            if (m_taskbarOverlay) m_taskbarOverlay->RequestMeasure();  // taskbar moved/auto-hide toggled
        }
        else if (wparam == ABN_FULLSCREENAPP)
        {
            SetWindowPos(hwnd, lparam ? HWND_BOTTOM : HWND_TOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_DISPLAYCHANGE:
        AppBarSetPos(hwnd);
        if (m_taskbarOverlay) m_taskbarOverlay->RequestMeasure();
        InvalidateRect(hwnd, nullptr, TRUE);  // width may change → repaint whole client
        return 0;

    case WM_DPICHANGED:
        AppBarSetPos(hwnd);
        if (m_taskbarOverlay) m_taskbarOverlay->RequestMeasure();
        InvalidateRect(hwnd, nullptr, TRUE);  // repaint all at new DPI, not just exposed strip
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wparam)
        {
            KillTimer(hwnd, kOverlayTimer);  // don't let a queued tick Update() into a dying explorer
            AppBarRemove(hwnd);
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
                AppBarSetPos(hwnd);  // band count changed → re-negotiate reserved height
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (event == EVENT_SYSTEM_FOREGROUND)
        {
            if (m_store.Has(target) && m_tabReader)
                m_tabReader->RequestSnapshot(target);
            return 0;
        }

        if (event == EVENT_OBJECT_NAMECHANGE)
        {
            if (m_store.Has(target))
            {
                StoreWindow(m_store, target);
                RequestSnapshotDebounced(target);
                InvalidateRect(hwnd, nullptr, FALSE);
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
        if (wparam == kHoverTimer)
        {
            KillTimer(hwnd, kHoverTimer);
            ShowFanFor(m_hoverCard);
        }
        else if (wparam == kDebounceTimer)
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
            AppBarSetPos(hwnd);  // a removed window may drop a band → re-negotiate
            InvalidateRect(hwnd, nullptr, FALSE);
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
            InvalidateRect(hwnd, nullptr, FALSE);
            // Re-measure (not just repaint): the new config may change which host fits,
            // so ApplyGap must re-evaluate and re-post the gap-active state.
            if (m_taskbarOverlay) m_taskbarOverlay->RequestMeasure();
        }
        else if (wparam == kOverlayTimer)
        {
            KillTimer(hwnd, kOverlayTimer);  // one-shot debounce
            if (m_taskbarOverlay) m_taskbarOverlay->RequestMeasure();
        }
        return 0;

    case kRemeasureMsg:
        // Coalesce a LOCATIONCHANGE burst into one measure after 200ms of quiet.
        SetTimer(hwnd, kOverlayTimer, kOverlayMs, nullptr);
        return 0;

    case kGapStateMsg:
        // Overlay reported whether it's hosting the buttons → flip the dock fallback.
        m_gapActive   = (wparam != 0);
        m_gapResolved = true;
        InvalidateRect(hwnd, nullptr, FALSE);
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
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case kFanActivateMsg:
    {
        // wparam = target browser HWND; lparam = index into that window's Store tab
        // vector (FanPopup already mapped displayed row → original index). Resolve the
        // wanted title NOW (pre-restore Store state), then restore+foreground FIRST
        // (R1: patterns on a background/iconic window are unreliable) and hand the
        // worker a title to re-match against the post-restore tree.
        const HWND target   = reinterpret_cast<HWND>(wparam);
        const int  tabIndex = static_cast<int>(lparam);
        const auto& all = m_store.All();
        auto it = all.find(target);
        if (it != all.end() && tabIndex >= 0 &&
            tabIndex < static_cast<int>(it->second.tabs.size()))
        {
            std::wstring wanted = it->second.tabs[tabIndex].title;
            RestoreWindow(target);
            if (m_tabReader)
                m_tabReader->RequestActivate(target, std::move(wanted), tabIndex);
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
            // Refresh the card/fan from the same re-snapshot the worker just took.
            if (!payload->freshTabs.empty() && m_store.Has(payload->hwnd))
            {
                m_store.SetTabs(payload->hwnd, std::move(payload->freshTabs));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            delete payload;
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        if (!m_mouseTracking)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            m_mouseTracking = true;
        }

        const POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        // A button overlays the card's top-right; don't fan the card underneath it.
        const HWND card = (ButtonAt(pt) >= 0) ? nullptr : CardAt(pt);
        if (card && m_fanPopup) m_fanPopup->CancelGrace();  // cursor back over the dock → keep fan alive
        if (card != m_hoverCard)
        {
            m_hoverCard = card;
            if (card)
            {
                if (m_fanPopup && m_fanPopup->Visible())
                    ShowFanFor(card);   // instant switch to the newly hovered card's fan
                else
                    SetTimer(hwnd, kHoverTimer, kHoverMs, nullptr);  // delayed first-open
            }
            else
            {
                BeginHoverGrace();  // over empty dock → grace-close
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        m_mouseTracking = false;
        // Left the dock — but may be crossing into the fan; defer the close (BeginGrace
        // no-ops if the fan isn't visible). The fan's own mouse tracking cancels grace.
        BeginHoverGrace();
        return 0;

    case WM_LBUTTONUP:
    {
        const POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        // Buttons overlay the cards → hit-test them first.
        const int btn = ButtonAt(pt);
        if (btn >= 0)
        {
            ClearHover();
            m_launcher.Execute(m_launcher.Buttons()[btn]);
            return 0;
        }
        const HWND target = CardAt(pt);
        if (target)
        {
            ClearHover();
            // Card removal is driven by the EVENT_SYSTEM_MINIMIZEEND handler, the
            // single source of truth — so a restore that fails won't orphan a card.
            RestoreWindow(target);
        }
        return 0;
    }

    case WM_RBUTTONUP:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        m_configWatcher.reset();  // join watcher worker before teardown
        m_tabReader.reset();  // join worker before unhooking and removing appbar
        m_fanPopup.reset();
        m_taskbarOverlay.reset();
        KillTimer(hwnd, kDebounceTimer);
        KillTimer(hwnd, kHoverTimer);
        KillTimer(hwnd, kSnapshotTimer);
        KillTimer(hwnd, kConfigTimer);
        KillTimer(hwnd, kOverlayTimer);
        if (m_winEventHookLocation)   { UnhookWinEvent(m_winEventHookLocation);   m_winEventHookLocation   = nullptr; }
        if (m_winEventHookForeground) { UnhookWinEvent(m_winEventHookForeground); m_winEventHookForeground = nullptr; }
        if (m_winEventHookNameChange) { UnhookWinEvent(m_winEventHookNameChange); m_winEventHookNameChange = nullptr; }
        if (m_winEventHookMinimize)   { UnhookWinEvent(m_winEventHookMinimize);   m_winEventHookMinimize   = nullptr; }
        if (m_winEventHook)           { UnhookWinEvent(m_winEventHook);           m_winEventHook           = nullptr; }
        s_dockHwnd = nullptr;
        AppBarRemove(hwnd);
        PostQuitMessage(0);
        m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
