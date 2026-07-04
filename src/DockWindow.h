#pragma once

#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <memory>
#include "Store.h"
#include "TabReader.h"
#include "FanPopup.h"

// Dock window. UI thread only owns this (CLAUDE.md rule 5).
class DockWindow
{
public:
    DockWindow() = default;
    ~DockWindow();
    DockWindow(const DockWindow&) = delete;
    DockWindow& operator=(const DockWindow&) = delete;

    bool Create(HINSTANCE instance);

    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                      LONG idObject, LONG, DWORD, DWORD) noexcept;

    void AppBarRemove(HWND hwnd);
    void AppBarSetPos(HWND hwnd);
    void ShowFanFor(HWND card);

    HWND              m_hwnd             = nullptr;
    APPBARDATA        m_abd              = {};
    bool              m_appBarRegistered = false;
    int               m_dockHeight       = 0;
    Store             m_store;
    std::vector<HWND> m_pendingValidation;
    HWINEVENTHOOK     m_winEventHook           = nullptr;
    HWINEVENTHOOK     m_winEventHookMinimize   = nullptr;
    HWINEVENTHOOK     m_winEventHookNameChange = nullptr;
    HWINEVENTHOOK     m_winEventHookForeground = nullptr;
    std::unique_ptr<TabReader> m_tabReader;
    std::unique_ptr<FanPopup>  m_fanPopup;
    HWND                       m_hoverCard    = nullptr;
    bool                       m_mouseTracking = false;
};
