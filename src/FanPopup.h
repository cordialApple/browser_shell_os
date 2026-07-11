#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "Store.h"

// FanFlavor: Tabs (minimize window tabs), Folders (FolderFan subfolders). Different activation semantics.
enum class FanFlavor { Tabs, Folders };

// Message payload: wparam=targetHwnd (Tabs) or 0 (Folders), lparam=FanActivateRequest*; receiver owns deallocation.
struct FanActivateRequest
{
    FanFlavor flavor;
    int       rowIndex;
    long long tClickUs;
};

// Reusable popup window for tabs/folders; grows upward from anchor, never activates. UI thread only.
class FanPopup
{
public:
    FanPopup() = default;
    ~FanPopup();
    FanPopup(const FanPopup&) = delete;
    FanPopup& operator=(const FanPopup&) = delete;

    // ownerHwnd receives activateMsg (wparam=targetHwnd, lparam=FanActivateRequest*) on a row click.
    bool Create(HINSTANCE instance, HWND ownerHwnd, UINT activateMsg);
    void Destroy();

    void Show(FanFlavor flavor, HWND targetHwnd, const std::vector<Tab>& rows,
              int anchorLeftScreen, int anchorRightScreen, int anchorTopScreen, UINT dpi);
    void Hide();

    // Hover-bridge: defers close on card→fan seam crossing (briefly leaves both). CancelGrace re-arms.
    void BeginGrace();
    void CancelGrace();

    HWND Hwnd() const { return m_hwnd; }
    bool Visible() const { return m_visible; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC hdc);
    int  RowAt(POINT ptClient) const;  // hit-test in client coords; -1 if outside clickable row
    bool CursorInFan() const;

    HWND              m_hwnd        = nullptr;
    HWND              m_ownerHwnd   = nullptr;
    UINT              m_activateMsg = 0;
    FanFlavor         m_flavor      = FanFlavor::Tabs;
    HWND              m_targetHwnd  = nullptr;
    bool              m_visible     = false;
    bool              m_fanTracking = false;
    UINT              m_dpi         = 96;
    std::vector<Tab>  m_tabs;
    int               m_hoverRow    = -1;
};
