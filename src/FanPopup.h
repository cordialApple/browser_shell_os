#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "Store.h"

// Transient per-window tab list. A single reusable WS_POPUP window, repositioned
// and repopulated per hovered card. Grows upward from the dock strip's top edge.
// Never activates, never registers an AppBar. UI thread only.
class FanPopup
{
public:
    FanPopup() = default;
    ~FanPopup();
    FanPopup(const FanPopup&) = delete;
    FanPopup& operator=(const FanPopup&) = delete;

    bool Create(HINSTANCE instance);
    void Destroy();

    // Anchor: bottom edge sits at stripTopScreen, left aligned to cardLeftScreen,
    // clamped to the card's monitor. Grows upward.
    void Show(const std::wstring& title, const std::vector<Tab>& tabs,
              int cardLeftScreen, int cardRightScreen, int stripTopScreen, UINT dpi);
    void Hide();

    HWND Hwnd() const { return m_hwnd; }
    bool Visible() const { return m_visible; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC hdc);

    HWND              m_hwnd    = nullptr;
    bool              m_visible = false;
    UINT              m_dpi     = 96;
    std::wstring      m_title;
    std::vector<Tab>  m_tabs;
    int               m_hiddenCount = 0;
};
