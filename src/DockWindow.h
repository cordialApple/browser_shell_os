#pragma once

#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <memory>
#include "Store.h"
#include "TabReader.h"
#include "FanPopup.h"
#include "Launcher.h"
#include "ConfigWatcher.h"
#include "TaskbarOverlayWindow.h"

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
    int  DockHeightPx(UINT dpi) const;
    void ShowFanFor(HWND card);
    HWND CardAt(POINT ptClient) const;
    int  ButtonAt(POINT ptClient) const;
    const std::vector<Button>& DockButtons() const;  // launcher buttons, or none while gap hosts them
    void RestoreWindow(HWND target);
    void ClearHover();
    void BeginHoverGrace();
    void RequestSnapshotDebounced(HWND hwnd);
    // Hide the gap overlay while a Start/Search flyout is open or a fullscreen app owns
    // the taskbar's monitor; re-measure once it clears. All heuristics isolated here +
    // in TaskbarOverlayWindow (rule 6).
    void UpdateOverlaySuppression();
    bool FullscreenOnDockMonitor(HWND fg) const;

    HWND              m_hwnd             = nullptr;
    APPBARDATA        m_abd              = {};
    bool              m_appBarRegistered = false;
    int               m_dockHeight       = 0;
    Store             m_store;
    std::vector<HWND> m_pendingValidation;
    std::vector<HWND> m_pendingSnapshots;
    HWINEVENTHOOK     m_winEventHook           = nullptr;
    HWINEVENTHOOK     m_winEventHookMinimize   = nullptr;
    HWINEVENTHOOK     m_winEventHookNameChange = nullptr;
    HWINEVENTHOOK     m_winEventHookForeground = nullptr;
    HWINEVENTHOOK     m_winEventHookLocation   = nullptr;
    bool              m_gapActive              = false;
    bool              m_gapResolved            = false;  // first overlay verdict seen? (avoids startup double-show)
    bool              m_overlaySuppressed      = false;  // current overlay suppression (dedupes)
    std::unique_ptr<TabReader>     m_tabReader;
    std::unique_ptr<FanPopup>      m_fanPopup;
    std::unique_ptr<ConfigWatcher> m_configWatcher;
    Launcher                       m_launcher;
    // Declared after m_launcher: the overlay holds a Launcher* and its worker is
    // joined in ~TaskbarOverlayWindow, so it must destruct BEFORE m_launcher.
    std::unique_ptr<TaskbarOverlayWindow> m_taskbarOverlay;
    HWND                       m_hoverCard    = nullptr;
    bool                       m_mouseTracking = false;
};
