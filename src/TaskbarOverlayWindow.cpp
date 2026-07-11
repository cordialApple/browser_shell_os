#include "TaskbarOverlayWindow.h"
#include "PaintUtil.h"
#include "Renderer.h"
#include "Trace.h"
#include <shellapi.h>
#include <windowsx.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")   // SHAppBarMessage (ABM_GETSTATE only — query, no register)

using namespace Paint;
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr wchar_t kClassName[] = L"BrowserShellOsTaskbarOverlay";
    constexpr UINT     kApplyGapMsg = WM_APP + 1;
    constexpr UINT_PTR kRetryTimer  = 1;    // hysteresis: re-measure once before hiding
    constexpr UINT     kRetryMs      = 300;  // ~one XAML taskbar animation cycle

    // LWA_COLORKEY makes kColorKey fully see-through, so the gap shows through the
    // overlay everywhere except the opaque pixels the chips + pills paint.
    constexpr COLORREF kColorKey = RGB(1, 1, 1);

    constexpr int kMinGapDip = 40;   // narrower than this → not worth an overlay (→ hide)

    // A Windows update or a third-party shell (YASB/Zebar/Managed Shell) can register
    // Shell_TrayWnd for a fake taskbar. Only the real one is owned by explorer.exe.
    bool IsExplorer(HWND hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return false;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc) return false;
        wchar_t path[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        const bool ok = QueryFullProcessImageNameW(proc, 0, path, &len);
        CloseHandle(proc);
        if (!ok) return false;
        const wchar_t* file = wcsrchr(path, L'\\');
        file = file ? file + 1 : path;
        return _wcsicmp(file, L"explorer.exe") == 0;
    }

    std::wstring GetAutomationId(IUIAutomationElement* el)
    {
        BSTR b = nullptr;
        std::wstring s;
        if (SUCCEEDED(el->get_CurrentAutomationId(&b)) && b) { s = b; SysFreeString(b); }
        return s;
    }

    std::wstring GetClassName(IUIAutomationElement* el)
    {
        BSTR b = nullptr;
        std::wstring s;
        if (SUCCEEDED(el->get_CurrentClassName(&b)) && b) { s = b; SysFreeString(b); }
        return s;
    }

    HWND ChipHitOf(const Renderer::GapLayout& layout, POINT pt)
    {
        for (const Renderer::ChipHit& c : layout.chips)
            if (PtInRect(&c.rect, pt)) return c.hwnd;
        return nullptr;
    }

    int ButtonHitOf(const Renderer::GapLayout& layout, POINT pt)
    {
        for (const Renderer::ButtonHit& h : layout.buttons)
            if (PtInRect(&h.rect, pt)) return h.index;
        return -1;
    }
}

TaskbarOverlayWindow::~TaskbarOverlayWindow()
{
    Destroy();
}

bool TaskbarOverlayWindow::Create(HINSTANCE instance, const Launcher* launcher,
                                  const Store* store, HWND dockHwnd,
                                  UINT chipClickMsg, UINT chipHoverMsg, UINT buttonHoverMsg)
{
    m_launcher     = launcher;
    m_store        = store;
    m_dockHwnd     = dockHwnd;
    m_chipClickMsg = chipClickMsg;
    m_chipHoverMsg = chipHoverMsg;
    m_buttonHoverMsg = buttonHoverMsg;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = StaticWndProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // NOT WS_EX_TRANSPARENT: the overlay must catch clicks on its pills. WM_NCHITTEST
    // returns HTTRANSPARENT off the pills so empty-gap clicks pass to the taskbar.
    // LAYERED enables the color-key transparency. TOOLWINDOW/TOPMOST/NOACTIVATE match
    // the dock; NOACTIVATE keeps clicks from stealing foreground.
    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kClassName,
        L"",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, instance,
        this);
    if (!m_hwnd) return false;

    SetLayeredWindowAttributes(m_hwnd, kColorKey, 0, LWA_COLORKEY);
    m_thread = std::thread([this] { WorkerLoop(); });
    return true;
}

void TaskbarOverlayWindow::Destroy()
{
    if (m_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_stop = true;
        }
        m_cv.notify_one();
        m_thread.join();   // join before destroying the HWND the worker posts to
    }
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_shown = false;
}

void TaskbarOverlayWindow::RequestMeasure()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_stop) return;
        m_pending = true;
    }
    m_measurePending = true;   // until the worker posts a fresh gap (kApplyGapMsg)
    m_cv.notify_one();
}

void TaskbarOverlayWindow::WorkerLoop()
{
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IUIAutomation> automation;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&automation));

    while (true)
    {
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this] { return m_stop || m_pending; });
            if (m_stop) break;
            m_pending = false;
        }

        // On any measure failure (incl. an unexpected throw) post an invalid gap so
        // ApplyGap hides the overlay and the dock falls back — never freeze stale.
        Gap g = { {}, false };
        try { if (automation) g = MeasureGap(automation.Get()); }
        catch (...) {}
        try
        {
            auto* payload = new Gap(g);
            if (!PostMessageW(m_hwnd, kApplyGapMsg, 0, reinterpret_cast<LPARAM>(payload)))
                delete payload;
        }
        catch (...) {}
    }

    automation.Reset();   // release the interface before tearing down the apartment
    if (SUCCEEDED(hrCo)) CoUninitialize();
}

// static
HWND TaskbarOverlayWindow::FindTaskbar()
{
    HWND tray = nullptr;
    while ((tray = FindWindowExW(nullptr, tray, L"Shell_TrayWnd", nullptr)) != nullptr)
        if (IsExplorer(tray)) return tray;
    return nullptr;
}

// static
bool TaskbarOverlayWindow::IsAutoHide()
{
    APPBARDATA abd = { sizeof(abd) };
    return (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0;
}

// Win11's task buttons and Widgets button are XAML with no HWND, so read their
// bounding rects from UI Automation. Gap = [ right edge of the last task button,
// left edge of the Widgets button (else the tray) ], spanning the taskbar height.
// Win10 has no TaskbarFrame element → fall back to the legacy MSTaskListWClass /
// TrayNotifyWnd HWND rects. All rects are physical px (PMv2 → no conversion).
TaskbarOverlayWindow::Gap TaskbarOverlayWindow::MeasureGap(IUIAutomation* automation) const
{
    static const Gap kInvalid = { {}, false };

    if (IsAutoHide()) return kInvalid;

    HWND tray = FindTaskbar();
    if (!tray) return kInvalid;

    RECT rTray;
    if (!GetWindowRect(tray, &rTray)) return kInvalid;

    HWND trayNd = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    RECT rTrayNd = {};
    if (!trayNd || !GetWindowRect(trayNd, &rTrayNd)) return kInvalid;  // else outline spans the tray
    const LONG trayLeft = rTrayNd.left;

    const UINT rawDpi = GetDpiForWindow(tray);
    if (rawDpi == 0) return kInvalid;   // taskbar vanished mid-measure; 96 would halve the threshold
    const int dpi = static_cast<int>(rawDpi);
    const int minGap = ScalePx(kMinGapDip, dpi);

    LONG leftBound  = rTray.left;
    LONG rightBound = trayLeft;

    ComPtr<IUIAutomationElement> trayEl;
    ComPtr<IUIAutomationElement> frame;
    if (SUCCEEDED(automation->ElementFromHandle(tray, &trayEl)) && trayEl)
    {
        VARIANT vaid = {};
        vaid.vt = VT_BSTR;
        vaid.bstrVal = SysAllocString(L"TaskbarFrame");
        ComPtr<IUIAutomationCondition> frameCond;
        if (SUCCEEDED(automation->CreatePropertyCondition(
                UIA_AutomationIdPropertyId, vaid, &frameCond)))
            trayEl->FindFirst(TreeScope_Descendants, frameCond.Get(), &frame);
        VariantClear(&vaid);
    }

    if (frame)
    {
        // Win11: leftKnown = right edge of the rightmost KNOWN task button (apps +
        // Start/Search/TaskView). Widgets is in the gap iff it sits at/right of that
        // (>= so apps abutting Widgets collapse to a zero gap → dock fallback, never
        // an overlap); else the tray bounds the gap. Then extend leftBound over any
        // child sitting between leftKnown and the right bound — that catches an
        // overflow "show more" chevron (unknown class) by geometry, while left>=
        // leftKnown keeps a stray full-width child from collapsing the gap.
        ComPtr<IUIAutomationCondition> trueCond;
        ComPtr<IUIAutomationElementArray> kids;
        if (SUCCEEDED(automation->CreateTrueCondition(&trueCond)) &&
            SUCCEEDED(frame->FindAll(TreeScope_Children, trueCond.Get(), &kids)) && kids)
        {
            int n = 0;
            kids->get_Length(&n);
            std::vector<RECT> others;   // unknown-class children (overflow chevron etc.)
            LONG leftKnown   = rTray.left;
            LONG widgetsLeft = 0;
            bool haveWidgets = false;
            for (int i = 0; i < n; ++i)
            {
                ComPtr<IUIAutomationElement> el;
                if (FAILED(kids->GetElement(i, &el)) || !el) continue;
                const std::wstring aid = GetAutomationId(el.Get());
                RECT r = {};
                const HRESULT hr = el->get_CurrentBoundingRectangle(&r);
                if (aid == L"WidgetsButton")
                {
                    if (FAILED(hr)) return kInvalid;  // can't locate a critical bound → dock fallback
                    widgetsLeft = r.left; haveWidgets = true;
                    continue;
                }
                if (FAILED(hr)) continue;
                const std::wstring cls = GetClassName(el.Get());
                if (cls == L"Taskbar.TaskListButtonAutomationPeer" ||
                    aid == L"StartButton" || aid == L"SearchButton" || aid == L"TaskViewButton")
                {
                    if (r.right > leftKnown) leftKnown = r.right;
                }
                else
                {
                    others.push_back(r);
                }
            }
            if (haveWidgets && widgetsLeft >= leftKnown && widgetsLeft < rightBound)
                rightBound = widgetsLeft;
            leftBound = leftKnown;
            if (leftKnown > rTray.left)
                for (const RECT& r : others)
                    if (r.left >= leftKnown && r.right <= rightBound && r.right > leftBound)
                        leftBound = r.right;
        }
    }
    else
    {
        // Win10 fallback: legacy task-list HWND right edge → tray left.
        HWND rebar  = FindWindowExW(tray, nullptr, L"ReBarWindow32", nullptr);
        HWND taskSw = FindWindowExW(rebar ? rebar : tray, nullptr, L"MSTaskSwWClass", nullptr);
        HWND taskLs = taskSw ? FindWindowExW(taskSw, nullptr, L"MSTaskListWClass", nullptr) : nullptr;
        HWND src    = taskLs ? taskLs : taskSw;
        RECT rTask = {};
        if (!src || !GetWindowRect(src, &rTask)) return kInvalid;
        // Sleep-wake stale-rect guard: a task rect not nested in the taskbar is stale.
        const int taskH = rTask.bottom - rTask.top;
        if (!(taskH > 0 && taskH <= rTray.bottom - rTray.top &&
              rTask.left >= rTray.left && rTask.right <= rTray.right))
            return kInvalid;
        leftBound = rTask.right;
    }

    if (leftBound == rTray.left) return kInvalid;  // no task button matched → don't span the left half
    const RECT gap = { leftBound, rTray.top, rightBound, rTray.bottom };
    if (gap.right - gap.left < minGap) return kInvalid;
    return { gap, true };
}

void TaskbarOverlayWindow::ApplyGap(const Gap& g, bool allowHysteresis)
{
    if (!m_hwnd) return;
    m_lastGap = g;   // cache for RefreshContent's chip-count re-fit (no re-measure)

    // Size to the gap first (visibility unchanged), then show only if a chip or pill
    // actually fits — a valid-but-too-narrow gap must not leave a do-nothing topmost
    // window.
    bool fits = false;
    if (g.valid && !m_suppressed && m_store)
    {
        SetWindowPos(m_hwnd, HWND_TOPMOST,
                     g.rc.left, g.rc.top,
                     g.rc.right - g.rc.left, g.rc.bottom - g.rc.top,
                     SWP_NOACTIVATE);
        const Renderer::GapLayout layout = ComputeGapLayout();
        fits = !layout.chips.empty() || !layout.buttons.empty();
    }

    if (fits)
    {
        m_invalidStreak = 0;
        KillTimer(m_hwnd, kRetryTimer);
        RaiseSibling();
        if (!m_shown) { ShowWindow(m_hwnd, SW_SHOWNOACTIVATE); m_shown = true; }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        UpdateWindow(m_hwnd);
        return;
    }

    // Hide candidate. A UIA read taken mid-taskbar-animation returns zero bounding
    // rects → a spurious invalid; don't yank a healthy overlay on the first one. Retry
    // once ~one animation cycle later and hide only if it's still gone. Suppression
    // (flyout/fullscreen) is a genuine hide — skip hysteresis, drop immediately.
    if (allowHysteresis && m_shown && !m_suppressed && m_invalidStreak == 0)
    {
        m_invalidStreak = 1;
        SetTimer(m_hwnd, kRetryTimer, kRetryMs, nullptr);
        return;
    }
    m_invalidStreak = 0;
    KillTimer(m_hwnd, kRetryTimer);
    if (m_shown) { ShowWindow(m_hwnd, SW_HIDE); m_shown = false; }
}

// UI thread: the monitor the taskbar (and this overlay) live on. Rule 6: taskbar
// geometry stays isolated here. Caches the tray HWND — FindTaskbar's OpenProcess
// (explorer-ownership verify) ran on every foreground change before. The cache is
// UI-thread-only (the worker's MeasureGap has its own FindTaskbar call); IsWindow
// catches an explorer restart and TaskbarCreated invalidates it explicitly.
HMONITOR TaskbarOverlayWindow::TaskbarMonitor() const
{
    if (!m_uiTray || !IsWindow(m_uiTray))
        m_uiTray = FindTaskbar();
    return m_uiTray ? MonitorFromWindow(m_uiTray, MONITOR_DEFAULTTONEAREST) : nullptr;
}

// static — rule 6: taskbar discovery stays here. PID of the explorer-owned taskbar so
// the host can PID-scope its LOCATIONCHANGE hook (and re-scope after an explorer restart).
DWORD TaskbarOverlayWindow::TaskbarProcessId()
{
    DWORD pid = 0;
    if (HWND tray = FindTaskbar())
        GetWindowThreadProcessId(tray, &pid);
    return pid;
}

void TaskbarOverlayWindow::SetSuppressed(bool suppressed)
{
    if (m_suppressed == suppressed) return;
    m_suppressed = suppressed;
    if (!m_hwnd) return;
    if (suppressed)
    {
        m_invalidStreak = 0;
        KillTimer(m_hwnd, kRetryTimer);
        if (m_shown) { ShowWindow(m_hwnd, SW_HIDE); m_shown = false; }
    }
    // Un-suppress: the dock schedules a re-measure once the flyout/fullscreen animation
    // settles — an immediate re-measure would just catch zero rects again.
}

// UI thread: re-fit against the last measured gap (chip count may flip shown/hidden)
// and repaint. No UIA re-measure — the gap geometry hasn't changed, only our content.
// Skip while a RequestMeasure is in-flight: m_lastGap is stale then, and the imminent
// kApplyGapMsg re-lays-out from live Store content anyway (avoids a wrong-region blip).
void TaskbarOverlayWindow::RefreshContent()
{
    if (m_hwnd && !m_measurePending) ApplyGap(m_lastGap, /*allowHysteresis=*/false);
}

void TaskbarOverlayWindow::RaiseSibling()
{
    if (m_topSibling && IsWindowVisible(m_topSibling))
        SetWindowPos(m_topSibling, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void TaskbarOverlayWindow::ReassertVisibility()
{
    if (!m_hwnd) return;
    if (!m_shown)
    {
        // stuck hidden: caller (HostWindow kSafetyTimer) handles recovery via RequestMeasure
        return;
    }
    if (m_store)
    {
        const Renderer::GapLayout layout = ComputeGapLayout();
        if (layout.chips.empty() && layout.buttons.empty())
        {
            // stuck shown-but-empty: hide immediately, no repaint
            ShowWindow(m_hwnd, SW_HIDE);
            m_shown = false;
            return;
        }
    }
    // healthy: re-assert TOPMOST only — no geometry change, no repaint (avoids kSafetyTimer flicker)
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
    RaiseSibling();
}

const std::vector<Button>& TaskbarOverlayWindow::Buttons() const
{
    static const std::vector<Button> kNoButtons;
    return m_launcher ? m_launcher->Buttons() : kNoButtons;
}

Renderer::GapLayout TaskbarOverlayWindow::ComputeGapLayout() const
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    return Renderer::GapChipLayout(rc, GetDpiForWindow(m_hwnd), *m_store, Buttons());
}

int TaskbarOverlayWindow::ButtonAt(POINT ptClient) const
{
    if (!m_store) return -1;
    return ButtonHitOf(ComputeGapLayout(), ptClient);
}

HWND TaskbarOverlayWindow::ChipAt(POINT ptClient) const
{
    if (!m_store) return nullptr;
    return ChipHitOf(ComputeGapLayout(), ptClient);
}

void TaskbarOverlayWindow::UpdateHover(HWND chip)
{
    if (chip == m_hoverChip) return;
    m_hoverChip = chip;
    if (m_dockHwnd && m_chipHoverMsg)
        PostMessageW(m_dockHwnd, m_chipHoverMsg, reinterpret_cast<WPARAM>(chip), 0);
}

void TaskbarOverlayWindow::UpdateButtonHover(int index)
{
    if (index == m_hoverButton) return;
    m_hoverButton = index;
    if (m_dockHwnd && m_buttonHoverMsg)
        PostMessageW(m_dockHwnd, m_buttonHoverMsg, static_cast<WPARAM>(index), 0);
}

bool TaskbarOverlayWindow::ChipRectScreen(HWND hwnd, RECT* out) const
{
    if (!m_store || !m_hwnd || !out) return false;
    for (const Renderer::ChipHit& c : ComputeGapLayout().chips)
    {
        if (c.hwnd != hwnd) continue;
        POINT tl = { c.rect.left,  c.rect.top };
        POINT br = { c.rect.right, c.rect.bottom };
        ClientToScreen(m_hwnd, &tl);
        ClientToScreen(m_hwnd, &br);
        *out = { tl.x, tl.y, br.x, br.y };
        return true;
    }
    return false;
}

bool TaskbarOverlayWindow::ButtonRectScreen(int index, RECT* out) const
{
    if (!m_store || !m_hwnd || !out) return false;
    for (const Renderer::ButtonHit& b : ComputeGapLayout().buttons)
    {
        if (b.index != index) continue;
        POINT tl = { b.rect.left,  b.rect.top };
        POINT br = { b.rect.right, b.rect.bottom };
        ClientToScreen(m_hwnd, &tl);
        ClientToScreen(m_hwnd, &br);
        *out = { tl.x, tl.y, br.x, br.y };
        return true;
    }
    return false;
}

// static
LRESULT CALLBACK TaskbarOverlayWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    TaskbarOverlayWindow* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<TaskbarOverlayWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<TaskbarOverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self)
    {
        switch (msg)
        {
        case kApplyGapMsg:
        {
            auto* payload = reinterpret_cast<Gap*>(lparam);
            self->m_measurePending = false;   // fresh measurement arrived
            if (payload) { self->ApplyGap(*payload); delete payload; }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            self->Paint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST:
        {
            // Only chips + pills are ours; everywhere else falls through to the taskbar.
            // NCHITTEST lParam is in SCREEN coords — convert before hit-testing. Drive hover
            // from here (not WM_MOUSEMOVE): NCHITTEST fires on every move over the window
            // rect regardless of the HTCLIENT/HTTRANSPARENT result, so it catches a
            // chip→inter-chip-gap move that WM_MOUSEMOVE would miss (HTTRANSPARENT stops
            // mouse-move delivery but does NOT fire WM_MOUSELEAVE — the stale-hover trap).
            POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ScreenToClient(hwnd, &pt);
            // Drive hover from the LIVE cursor, not the queried point: WM_NCHITTEST also
            // fires for drag/drop queries whose point may not be the cursor (Raymond Chen),
            // and a stray query must not drop hover while the cursor rests on a chip.
            POINT cur;
            const bool haveCur = GetCursorPos(&cur);
            if (haveCur) ScreenToClient(hwnd, &cur);
            if (!self->m_store)
            {
                if (haveCur) { self->UpdateHover(nullptr); self->UpdateButtonHover(-1); }
                return HTTRANSPARENT;
            }
            const Renderer::GapLayout layout = self->ComputeGapLayout();
            if (haveCur)
            {
                const HWND curChip = ChipHitOf(layout, cur);
                self->UpdateHover(curChip);
                // A chip takes priority; a button only fan-hovers (FolderFan only —
                // other actions have nothing to show in a fan) when no chip is hovered.
                int fanButton = -1;
                if (!curChip)
                {
                    const int curBtn = ButtonHitOf(layout, cur);
                    const auto& buttons = self->Buttons();
                    if (curBtn >= 0 && curBtn < static_cast<int>(buttons.size()) &&
                        buttons[curBtn].action == ButtonAction::FolderFan)
                        fanButton = curBtn;
                }
                self->UpdateButtonHover(fanButton);
            }
            return (ChipHitOf(layout, pt) || ButtonHitOf(layout, pt) >= 0) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_LBUTTONUP:
        {
            // WM_LBUTTONUP lParam is already CLIENT coords.
            const POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            if (HWND chip = self->ChipAt(pt))
            {
                if (self->m_dockHwnd && self->m_chipClickMsg)
                    PostMessageW(self->m_dockHwnd, self->m_chipClickMsg,
                                 reinterpret_cast<WPARAM>(chip), 0);
                return 0;
            }
            const int i = self->ButtonAt(pt);
            if (i >= 0 && self->m_launcher)
                self->m_launcher->Execute(self->m_launcher->Buttons()[i]);
            return 0;
        }
        case WM_RBUTTONUP:
        {
            // Right-click our own content (a chip or pill) quits the app — restores the
            // dock strip's old right-click-to-close now that the strip is gone. Empty gap
            // is HTTRANSPARENT, so a right-click there still reaches the taskbar. A plain
            // PostMessage (not a modal popup menu): a nested TrackPopupMenu loop here would
            // re-entrantly pump teardown/hide messages — dismissing the menu or freeing this
            // overlay under its own stack frame.
            const POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            if ((self->ChipAt(pt) || self->ButtonAt(pt) >= 0) && self->m_dockHwnd)
                PostMessageW(self->m_dockHwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        case WM_MOUSEMOVE:
            // Arm whole-overlay leave tracking: when the cursor leaves the window entirely
            // (e.g. up into the fan), WM_MOUSELEAVE clears hover. Chip→gap transitions
            // inside the window are handled by WM_NCHITTEST above.
            if (!self->m_mouseTracking)
            {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                self->m_mouseTracking = true;
            }
            return 0;
        case WM_MOUSELEAVE:
            self->m_mouseTracking = false;
            // Reset BOTH dedupe fields: NCHITTEST can't fire once the cursor is over the
            // fan (a separate HWND), so a straight pill→fan move never re-derives fanButton
            // = -1 on its own. Leaving m_hoverButton stuck at idx deduped the next re-hover
            // of the SAME pill to a no-op — fan silently wouldn't reopen. Chips already got
            // this reset; buttons didn't when the FolderFan hover path was bolted on.
            self->UpdateHover(nullptr);   // left the overlay → fan graces (may cross into it)
            self->UpdateButtonHover(-1);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_TIMER:
            if (wparam == kRetryTimer) { KillTimer(hwnd, kRetryTimer); self->RequestMeasure(); }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void TaskbarOverlayWindow::Paint(HDC hdc)
{
    const long long tStartUs = trace::NowUs();

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(hdc, w, h) : nullptr;
    if (!mem || !bmp) { if (bmp) DeleteObject(bmp); if (mem) DeleteDC(mem); return; }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH key = CreateSolidBrush(kColorKey);
    FillRect(mem, &rc, key);
    DeleteObject(key);

    if (m_store)
    {
        const UINT rawDpi = GetDpiForWindow(m_hwnd);
        const int  dpi    = rawDpi ? static_cast<int>(rawDpi) : 96;
        const std::vector<Button>& buttons = Buttons();
        const Renderer::GapLayout layout =
            Renderer::GapChipLayout(rc, rawDpi, *m_store, buttons);

        const auto& all = m_store->All();
        for (const Renderer::ChipHit& c : layout.chips)
        {
            auto it = all.find(c.hwnd);
            if (it != all.end())
                Renderer::DrawChip(mem, c.rect, it->second.title, dpi);
        }
        for (const Renderer::ButtonHit& bh : layout.buttons)
            Renderer::DrawButton(mem, bh.rect, buttons[bh.index], dpi);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);

    TRACE_EVENT("Paint",
        TraceLoggingInt64(trace::NowUs() - tStartUs, "duration_us"),
        TraceLoggingInt32(w, "dirty_w"),
        TraceLoggingInt32(h, "dirty_h"));
}
