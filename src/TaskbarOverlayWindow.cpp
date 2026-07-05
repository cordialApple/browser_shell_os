#include "TaskbarOverlayWindow.h"
#include "PaintUtil.h"
#include "Renderer.h"
#include <shellapi.h>
#include <windowsx.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <cwchar>
#include <string>
#include <vector>

using namespace Paint;
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr wchar_t kClassName[] = L"BrowserShellOsTaskbarOverlay";
    constexpr UINT     kApplyGapMsg = WM_APP + 1;
    constexpr UINT_PTR kRetryTimer  = 1;    // hysteresis: re-measure once before hiding
    constexpr UINT     kRetryMs      = 300;  // ~one XAML taskbar animation cycle

    // LWA_COLORKEY makes kColorKey fully see-through, so the gap shows through the
    // overlay everywhere except the pills the buttons paint.
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
}

TaskbarOverlayWindow::~TaskbarOverlayWindow()
{
    Destroy();
}

bool TaskbarOverlayWindow::Create(HINSTANCE instance, const Launcher* launcher,
                                  HWND dockHwnd, UINT stateMsg)
{
    m_launcher = launcher;
    m_dockHwnd = dockHwnd;
    m_stateMsg = stateMsg;

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

void TaskbarOverlayWindow::ApplyGap(const Gap& g)
{
    if (!m_hwnd) return;

    // Size to the gap first (visibility unchanged), then show only if a pill actually
    // fits — a valid-but-too-narrow gap must not leave a do-nothing topmost window.
    bool fits = false;
    if (g.valid && !m_suppressed)
    {
        SetWindowPos(m_hwnd, HWND_TOPMOST,
                     g.rc.left, g.rc.top,
                     g.rc.right - g.rc.left, g.rc.bottom - g.rc.top,
                     SWP_NOACTIVATE);
        RECT client;
        GetClientRect(m_hwnd, &client);
        fits = m_launcher && !Renderer::GapButtonLayout(
                   client, GetDpiForWindow(m_hwnd), m_launcher->Buttons()).empty();
    }

    if (fits)
    {
        m_invalidStreak = 0;
        KillTimer(m_hwnd, kRetryTimer);
        if (!m_shown) { ShowWindow(m_hwnd, SW_SHOWNOACTIVATE); m_shown = true; }
        InvalidateRect(m_hwnd, nullptr, TRUE);
        UpdateWindow(m_hwnd);
        PostState();
        return;
    }

    // Hide candidate. A UIA read taken mid-taskbar-animation returns zero bounding
    // rects → a spurious invalid; don't yank a healthy overlay on the first one. Retry
    // once ~one animation cycle later and hide only if it's still gone. Suppression
    // (flyout/fullscreen) is a genuine hide — skip hysteresis, drop immediately.
    if (m_shown && !m_suppressed && m_invalidStreak == 0)
    {
        m_invalidStreak = 1;
        SetTimer(m_hwnd, kRetryTimer, kRetryMs, nullptr);
        return;
    }
    m_invalidStreak = 0;
    KillTimer(m_hwnd, kRetryTimer);
    if (m_shown) { ShowWindow(m_hwnd, SW_HIDE); m_shown = false; }
    PostState();
}

// Tell the dock on the first verdict too (not just flips), so it knows whether to show
// its fallback strip instead of waiting — and never double-shows at startup.
void TaskbarOverlayWindow::PostState()
{
    if (m_dockHwnd && (m_shown != m_lastPostedShown || !m_statePosted))
    {
        m_statePosted     = true;
        m_lastPostedShown = m_shown;
        PostMessageW(m_dockHwnd, m_stateMsg, m_shown ? 1 : 0, 0);
    }
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
        if (m_shown) { ShowWindow(m_hwnd, SW_HIDE); m_shown = false; PostState(); }
    }
    // Un-suppress: the dock schedules a re-measure once the flyout/fullscreen animation
    // settles — an immediate re-measure would just catch zero rects again.
}

int TaskbarOverlayWindow::ButtonAt(POINT ptClient) const
{
    if (!m_launcher) return -1;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    for (const Renderer::ButtonHit& h :
         Renderer::GapButtonLayout(rc, GetDpiForWindow(m_hwnd), m_launcher->Buttons()))
        if (PtInRect(&h.rect, ptClient)) return h.index;
    return -1;
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
            // Only the pills are ours; everywhere else falls through to the taskbar.
            POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ScreenToClient(hwnd, &pt);
            return self->ButtonAt(pt) >= 0 ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_LBUTTONUP:
        {
            const POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            const int i = self->ButtonAt(pt);
            if (i >= 0 && self->m_launcher)
                self->m_launcher->Execute(self->m_launcher->Buttons()[i]);
            return 0;
        }
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
    RECT rc;
    GetClientRect(m_hwnd, &rc);

    HBRUSH key = CreateSolidBrush(kColorKey);
    FillRect(hdc, &rc, key);          // gap shows through (transparent via LWA_COLORKEY)
    DeleteObject(key);

    if (!m_launcher) return;

    const UINT rawDpi = GetDpiForWindow(m_hwnd);
    const int dpi = rawDpi ? static_cast<int>(rawDpi) : 96;
    const auto& buttons = m_launcher->Buttons();
    for (const Renderer::ButtonHit& h : Renderer::GapButtonLayout(rc, rawDpi, buttons))
        Renderer::DrawButton(hdc, h.rect, buttons[h.index], dpi);
}
