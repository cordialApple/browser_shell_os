#pragma once

#include <windows.h>
#include <shellapi.h>
#include <vector>
#include "Store.h"

// Dock window. UI thread only owns this (CLAUDE.md rule 5).
class DockWindow
{
public:
    DockWindow() = default;
    ~DockWindow();
    DockWindow(const DockWindow&) = delete;
    DockWindow& operator=(const DockWindow&) = delete;

    // Register class + create + show + AppBar register. False if Win32 say no.
    bool Create(HINSTANCE instance);

    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                      LONG idObject, LONG, DWORD, DWORD) noexcept;

    void AppBarRemove(HWND hwnd);
    void AppBarSetPos(HWND hwnd);   // ABM_QUERYPOS → re-anchor → ABM_SETPOS → SetWindowPos

    HWND              m_hwnd             = nullptr;
    APPBARDATA        m_abd              = {};
    bool              m_appBarRegistered = false;
    int               m_dockHeight       = 0;
    Store             m_store;
    std::vector<HWND> m_pendingValidation;
    HWINEVENTHOOK     m_winEventHook         = nullptr;
    HWINEVENTHOOK     m_winEventHookMinimize = nullptr;
};
