#include "DockWindow.h"

#pragma comment(lib, "shell32.lib")

namespace
{
    constexpr wchar_t kClassName[] = L"BrowserShellOsDockWindow";
    constexpr UINT    kCallbackMsg = WM_APP + 1; // AppBar shell callback

    // Debug rect for 1.2. Real size come from AppBar negotiation in 1.5.
    constexpr int kDebugWidth = 550;
    constexpr int kDebugHeight = 64;
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

    // Debug position: bottom-left above taskbar. Replaced by AppBar negotiation in 1.5.
    const int x = 160;
    const int y = 1645;

    // WS_EX_TOOLWINDOW: no taskbar button, no Alt-Tab. WS_EX_TOPMOST: stay
    // visible; AppBar alone does not guarantee z-order. WS_EX_NOACTIVATE: dock
    // never steals foreground. Step 1.6 drops TOPMOST for fullscreen apps.
    const HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClassName,
        L"browser_shell_os dock",
        WS_POPUP,
        x, y, kDebugWidth, kDebugHeight,
        nullptr, nullptr, instance,
        this);
    if (!hwnd)
    {
        return false;
    }

    // m_hwnd already set by WM_NCCREATE, but belt + suspenders.
    m_hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd); // force immediate WM_PAINT before message loop

    // Register AppBar. Position negotiation (ABM_QUERYPOS/SETPOS) is step 1.5.
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
    return true;
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

LRESULT DockWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        // WM_PAINT owns all drawing; suppress default erase to avoid flicker.
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        // Dark background fill
        HBRUSH bg = CreateSolidBrush(RGB(28, 28, 30));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        // DPI-scaled font. PMv2 owns all pixels; stock DC font is 96-DPI baseline
        // and renders too small at 150%. MulDiv(12, dpi, 72) converts 12pt → pixels.
        const UINT dpi = GetDpiForWindow(hwnd);
        LOGFONTW lf = {};
        lf.lfHeight = -MulDiv(12, static_cast<int>(dpi), 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        HFONT font = CreateFontIndirectW(&lf);
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));

        // Centered label
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(220, 220, 220));
        DrawTextW(hdc, L"browser_shell_os dock", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, oldFont);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_RBUTTONUP:
        // Debug quit for Stage 1 testing. DestroyWindow send WM_DESTROY
        // synchronously (re-enter this WndProc) before returning here.
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        AppBarRemove(hwnd); // must precede PostQuitMessage; uses hwnd param not m_hwnd
        PostQuitMessage(0);
        m_hwnd = nullptr;   // null last so AppBarRemove can use it if needed
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
