#include "FanPopup.h"
#include "PaintUtil.h"

using namespace Paint;

namespace
{
    constexpr wchar_t kClassName[] = L"BrowserShellOsFanPopup";
}

FanPopup::~FanPopup()
{
    Destroy();
}

bool FanPopup::Create(HINSTANCE instance)
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = StaticWndProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    // Class may already be registered by a prior instance in-process; tolerate that.
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClassName,
        L"",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, instance,
        this);
    return m_hwnd != nullptr;
}

void FanPopup::Destroy()
{
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_visible = false;
}

void FanPopup::Show(const std::wstring& title, const std::vector<Tab>& tabs,
                    int cardLeftScreen, int cardRightScreen, int stripTopScreen, UINT dpi)
{
    if (!m_hwnd) return;
    if (tabs.empty()) { Hide(); return; }

    const int dpiI = dpi ? static_cast<int>(dpi) : 96;
    m_dpi   = static_cast<UINT>(dpiI);
    m_title = title;

    const int pad     = ScalePx(6, dpiI);
    const int headerH = ScalePx(24, dpiI);
    const int rowH    = ScalePx(24, dpiI);

    // Width tracks the hovered card but stays legible; clamp to the card's monitor.
    int width = cardRightScreen - cardLeftScreen;
    const int minW = ScalePx(240, dpiI);
    const int maxW = ScalePx(420, dpiI);
    if (width < minW) width = minW;
    if (width > maxW) width = maxW;

    POINT anchor = { cardLeftScreen, stripTopScreen - 1 };
    HMONITOR hmon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hmon, &mi);

    int left = cardLeftScreen;
    if (left + width > mi.rcMonitor.right)  left = mi.rcMonitor.right - width;
    if (left < mi.rcMonitor.left)           left = mi.rcMonitor.left;

    // Grow upward from the strip; cap rows to what fits above the strip.
    const int avail   = stripTopScreen - mi.rcMonitor.top;
    int maxRows = (avail - headerH - pad * 2) / rowH;
    if (maxRows < 1) maxRows = 1;

    const int total = static_cast<int>(tabs.size());
    int shown = total;
    m_hiddenCount = 0;
    if (total > maxRows)
    {
        shown = maxRows - 1;          // reserve the last row for the "+N more" line
        if (shown < 1) shown = 1;
        m_hiddenCount = total - shown;
    }

    m_tabs.assign(tabs.begin(), tabs.begin() + shown);

    const int rows   = shown + (m_hiddenCount > 0 ? 1 : 0);
    int height = headerH + rows * rowH + pad * 2;
    if (height > avail) height = avail;   // never overflow above the monitor top
    const int top = stripTopScreen - height;

    SetWindowPos(m_hwnd, HWND_TOPMOST, left, top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    m_visible = true;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    UpdateWindow(m_hwnd);
}

void FanPopup::Hide()
{
    if (m_hwnd && m_visible)
    {
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
    }
}

// static
LRESULT CALLBACK FanPopup::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    FanPopup* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<FanPopup*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<FanPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self)
    {
        switch (msg)
        {
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
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void FanPopup::Paint(HDC hdc)
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int dpiI = static_cast<int>(m_dpi);

    HBRUSH bg = CreateSolidBrush(kCardBg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    const int pad     = ScalePx(6, dpiI);
    const int headerH = ScalePx(24, dpiI);
    const int rowH    = ScalePx(24, dpiI);
    const int chipPad = ScalePx(6, dpiI);

    SetBkMode(hdc, TRANSPARENT);

    HFONT hdrFont = MakeFont(10, FW_SEMIBOLD, dpiI);
    HFONT rowFont = MakeFont(9,  FW_NORMAL,   dpiI);
    HFONT old     = static_cast<HFONT>(SelectObject(hdc, hdrFont));

    RECT hdrRc = { rc.left + pad, rc.top + pad, rc.right - pad, rc.top + pad + headerH };
    SetTextColor(hdc, kTextPrimary);
    DrawTextW(hdc, m_title.c_str(), -1, &hdrRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, rowFont);
    int y = rc.top + pad + headerH;
    for (const Tab& tab : m_tabs)
    {
        RECT row = { rc.left + pad, y, rc.right - pad, y + rowH };
        if (tab.active)
        {
            HBRUSH hl = CreateSolidBrush(kChipActiveBg);
            FillRect(hdc, &row, hl);
            DeleteObject(hl);
        }
        RECT txt = { row.left + chipPad, row.top, row.right - chipPad, row.bottom };
        SetTextColor(hdc, tab.active ? kTextActive : kTextPrimary);
        DrawTextW(hdc, tab.title.c_str(), -1, &txt,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += rowH;
    }

    if (m_hiddenCount > 0)
    {
        wchar_t buf[24];
        swprintf_s(buf, L"+%d more", m_hiddenCount);
        RECT row = { rc.left + pad + chipPad, y, rc.right - pad - chipPad, y + rowH };
        SetTextColor(hdc, kTextSecond);
        DrawTextW(hdc, buf, -1, &row, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, old);
    DeleteObject(hdrFont);
    DeleteObject(rowFont);
}
