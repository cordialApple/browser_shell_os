#include "FanPopup.h"
#include "PaintUtil.h"
#include "Trace.h"
#include <windowsx.h>

using namespace Paint;

namespace
{
    constexpr wchar_t  kClassName[] = L"BrowserShellOsFanPopup";
    constexpr UINT_PTR kGraceTimer  = 1;
    // Chips are edge-adjacent to their fan (fan bottom == chip top), so the only seam is
    // the discrete-message hole on a fast upward flick — shorter than the old dock's gap.
    constexpr UINT     kGraceMs     = 150;
}

FanPopup::~FanPopup()
{
    Destroy();
}

bool FanPopup::Create(HINSTANCE instance, HWND ownerHwnd, UINT activateMsg)
{
    m_ownerHwnd   = ownerHwnd;
    m_activateMsg = activateMsg;

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
        KillTimer(m_hwnd, kGraceTimer);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_visible = false;
}

void FanPopup::Show(HWND targetHwnd, const std::vector<Tab>& tabs,
                    int anchorLeftScreen, int anchorRightScreen, int anchorTopScreen, UINT dpi)
{
    if (!m_hwnd) return;
    if (tabs.empty()) { Hide(); return; }

    m_targetHwnd = targetHwnd;
    const int dpiI = dpi ? static_cast<int>(dpi) : 96;
    m_dpi   = static_cast<UINT>(dpiI);

    const int pad     = ScalePx(6, dpiI);
    const int rowH    = ScalePx(24, dpiI);

    // Width tracks the hovered anchor (card or chip) but stays legible; clamp to monitor.
    int width = anchorRightScreen - anchorLeftScreen;
    const int minW = ScalePx(240, dpiI);
    const int maxW = ScalePx(420, dpiI);
    if (width < minW) width = minW;
    if (width > maxW) width = maxW;

    POINT anchor = { anchorLeftScreen, anchorTopScreen - 1 };
    HMONITOR hmon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hmon, &mi);

    int left = anchorLeftScreen;
    if (left + width > mi.rcMonitor.right)  left = mi.rcMonitor.right - width;
    if (left < mi.rcMonitor.left)           left = mi.rcMonitor.left;

    // Grow upward from the anchor top; cap rows to what fits above it.
    const int avail   = anchorTopScreen - mi.rcMonitor.top;
    int maxRows = (avail - pad * 2) / rowH;
    if (maxRows < 1) maxRows = 1;

    const int total = static_cast<int>(tabs.size());
    const int shown = total > maxRows ? maxRows : total;   // silent cap; no "+N more" trailer

    m_tabs.assign(tabs.begin(), tabs.begin() + shown);
    m_hoverRow = -1;

    int height = shown * rowH + pad * 2;
    if (height > avail) height = avail;   // never overflow above the monitor top
    const int top = anchorTopScreen - height;

    SetWindowPos(m_hwnd, HWND_TOPMOST, left, top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    m_visible = true;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    UpdateWindow(m_hwnd);
}

void FanPopup::Hide()
{
    if (m_hwnd)
        KillTimer(m_hwnd, kGraceTimer);
    if (m_hwnd && m_visible)
    {
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible     = false;
        m_fanTracking = false;
    }
}

void FanPopup::BeginGrace()
{
    if (!m_hwnd || !m_visible) return;
    // Don't arm if the cursor is already inside the fan. BeginGrace runs on the anchor's
    // leave (dock WM_MOUSELEAVE / overlay kChipHoverMsg(0)), which can arrive AFTER the
    // cursor has settled on the edge-adjacent fan; TrackMouseEvent synthesizes no arrival
    // WM_MOUSEMOVE to CancelGrace it, so an unguarded timer would hide the fan mid-read.
    if (CursorInFan()) return;
    SetTimer(m_hwnd, kGraceTimer, kGraceMs, nullptr);
}

bool FanPopup::CursorInFan() const
{
    POINT pt;
    RECT  rc;
    return m_hwnd && GetCursorPos(&pt) && GetWindowRect(m_hwnd, &rc) && PtInRect(&rc, pt);
}

void FanPopup::CancelGrace()
{
    if (m_hwnd)
        KillTimer(m_hwnd, kGraceTimer);
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
            return MA_NOACTIVATE;   // MANDATORY: keeps the fan from stealing activation on hover/click (R1)
        case WM_MOUSEMOVE:
        {
            self->CancelGrace();    // cursor is on the fan — keep it open
            if (!self->m_fanTracking)
            {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                self->m_fanTracking = true;
            }
            const POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            const int hr = self->RowAt(pt);
            if (hr != self->m_hoverRow)
            {
                self->m_hoverRow = hr;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            self->m_fanTracking = false;
            if (self->m_hoverRow != -1)
            {
                self->m_hoverRow = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            self->BeginGrace();     // left the fan — defer close in case we're heading to a card
            return 0;
        case WM_TIMER:
            if (wparam == kGraceTimer)
            {
                KillTimer(hwnd, kGraceTimer);
                // Re-check: if the cursor is on the fan (it returned without a move that
                // would CancelGrace), keep it open — the fan's WM_MOUSELEAVE re-graces.
                if (self->CursorInFan())
                {
                    // Cursor entered the fan without a WM_MOUSEMOVE (edge-seam teleport):
                    // arm leave-tracking so a later exit still closes the fan.
                    if (!self->m_fanTracking)
                    {
                        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                        TrackMouseEvent(&tme);
                        self->m_fanTracking = true;
                    }
                    return 0;
                }
                self->Hide();       // grace expired with cursor on neither window
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
        {
            const long long tClick = trace::NowUs();   // A: trigger detected
            const POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            const int idx = self->RowAt(pt);
            if (idx >= 0 && self->m_ownerHwnd && self->m_activateMsg)
            {
                auto* req = new FanActivateRequest{ idx, tClick };
                if (!PostMessageW(self->m_ownerHwnd, self->m_activateMsg,
                                  reinterpret_cast<WPARAM>(self->m_targetHwnd),
                                  reinterpret_cast<LPARAM>(req)))
                    delete req;
            }
            return 0;
        }
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int FanPopup::RowAt(POINT ptClient) const
{
    if (!m_hwnd) return -1;
    const int dpiI = static_cast<int>(m_dpi);
    const int pad  = ScalePx(6, dpiI);
    const int rowH = ScalePx(24, dpiI);

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (ptClient.x < rc.left + pad || ptClient.x >= rc.right - pad) return -1;

    const int rel = ptClient.y - (rc.top + pad);
    if (rel < 0) return -1;
    const int row = rel / rowH;
    return (row < static_cast<int>(m_tabs.size())) ? row : -1;
}

void FanPopup::Paint(HDC hdc)
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int dpiI = static_cast<int>(m_dpi);
    const Theme& th = ActiveTheme();

    HBRUSH bg = CreateSolidBrush(th.chipTop);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    const int pad     = ScalePx(6, dpiI);
    const int rowH    = ScalePx(24, dpiI);
    const int chipPad = ScalePx(6, dpiI);

    SetBkMode(hdc, TRANSPARENT);

    HFONT rowFont = MakeFont(9, FW_NORMAL, dpiI);
    HFONT old     = static_cast<HFONT>(SelectObject(hdc, rowFont));

    int y = rc.top + pad;
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
    {
        const Tab& tab = m_tabs[i];
        RECT row = { rc.left + pad, y, rc.right - pad, y + rowH };
        if (tab.active)
        {
            if (th.gradient)
            {
                FillVGradient(hdc, row, th.activeTop, th.activeBottom);
            }
            else
            {
                HBRUSH hl = CreateSolidBrush(th.activeTop);
                FillRect(hdc, &row, hl);
                DeleteObject(hl);
            }
        }
        else if (i == m_hoverRow)
        {
            HBRUSH hl = CreateSolidBrush(th.hoverFill);
            FillRect(hdc, &row, hl);
            DeleteObject(hl);
        }
        RECT txt = { row.left + chipPad, row.top, row.right - chipPad, row.bottom };
        SetTextColor(hdc, tab.active ? th.activeText : th.chipText);
        DrawTextW(hdc, tab.title.c_str(), -1, &txt,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += rowH;
    }

    SelectObject(hdc, old);
    DeleteObject(rowFont);
}
