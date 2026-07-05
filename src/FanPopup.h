#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "Store.h"

// Transient per-window tab list. A single reusable WS_POPUP window, repositioned
// and repopulated per hovered anchor (a dock card or a taskbar chip). Grows upward
// from the anchor's top edge. Never activates, never registers an AppBar. UI thread only.
class FanPopup
{
public:
    FanPopup() = default;
    ~FanPopup();
    FanPopup(const FanPopup&) = delete;
    FanPopup& operator=(const FanPopup&) = delete;

    // ownerHwnd receives activateMsg (wparam=targetHwnd, lparam=tab index) on a row click.
    bool Create(HINSTANCE instance, HWND ownerHwnd, UINT activateMsg);
    void Destroy();

    // targetHwnd = the window whose tabs these are (echoed back on a row click).
    // Anchor: bottom edge sits at anchorTopScreen, left aligned to anchorLeftScreen,
    // clamped to the anchor's monitor. Grows upward.
    void Show(HWND targetHwnd, const std::vector<Tab>& tabs,
              int anchorLeftScreen, int anchorRightScreen, int anchorTopScreen, UINT dpi);
    void Hide();

    // Hover-bridge: the fan is a separate window above the strip, so the cursor
    // crossing the card→fan seam briefly leaves both. BeginGrace (from the dock's
    // WM_MOUSELEAVE / off-card move) defers the close by a short timer; CancelGrace
    // (cursor back on the fan or a card) keeps it open. Grace firing = cursor on
    // neither window → Hide. UI thread only.
    void BeginGrace();
    void CancelGrace();

    HWND Hwnd() const { return m_hwnd; }
    bool Visible() const { return m_visible; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC hdc);
    // Displayed-row hit-test in client coords; shares Paint's geometry. Returns the
    // index into the ORIGINAL tab vector (== displayed row, tabs shown contiguously
    // from the front), or -1 for the "+N more" row / outside any clickable row.
    int  RowAt(POINT ptClient) const;
    bool CursorInFan() const;

    HWND              m_hwnd        = nullptr;
    HWND              m_ownerHwnd   = nullptr;
    UINT              m_activateMsg = 0;
    HWND              m_targetHwnd  = nullptr;
    bool              m_visible     = false;
    bool              m_fanTracking = false;
    UINT              m_dpi         = 96;
    std::vector<Tab>  m_tabs;
    int               m_hiddenCount = 0;
};
