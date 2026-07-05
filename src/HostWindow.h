#pragma once

#include <windows.h>
#include <vector>
#include <memory>
#include "Store.h"
#include "TabReader.h"
#include "FanPopup.h"
#include "Launcher.h"
#include "ConfigWatcher.h"
#include "TaskbarOverlayWindow.h"

// Hidden coordinator window. Owns the message loop, WinEvent hooks, Store (sole
// writer), TabReader, ConfigWatcher, Launcher, FanPopup, and the taskbar overlay.
// Never shown — chips live in the overlay; there is no dock strip and no AppBar.
// UI thread only owns this (CLAUDE.md rule 5).
class HostWindow
{
public:
    HostWindow() = default;
    ~HostWindow();
    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    bool Create(HINSTANCE instance);

    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                      LONG idObject, LONG, DWORD, DWORD) noexcept;

    void ShowFanForChip(HWND chip);
    void RestoreWindow(HWND target);
    void RequestSnapshotDebounced(HWND hwnd);
    // Hide the gap overlay while a Start/Search flyout is open or a fullscreen app owns
    // the taskbar's monitor; re-measure once it clears. All heuristics isolated here +
    // in TaskbarOverlayWindow (rule 6).
    void UpdateOverlaySuppression();
    bool FullscreenOnDockMonitor(HWND fg) const;
    // (Re)establish the explorer-PID-scoped LOCATIONCHANGE hook. Called at Create and
    // again on TaskbarCreated — an explorer restart gives the taskbar a new PID, so the
    // old hook is dead and the gap would never re-measure without re-scoping.
    void HookTaskbarLocation();

    HWND              m_hwnd             = nullptr;
    Store             m_store;
    std::vector<HWND> m_pendingValidation;
    std::vector<HWND> m_pendingSnapshots;
    HWINEVENTHOOK     m_winEventHook           = nullptr;
    HWINEVENTHOOK     m_winEventHookMinimize   = nullptr;
    HWINEVENTHOOK     m_winEventHookNameChange = nullptr;
    HWINEVENTHOOK     m_winEventHookForeground = nullptr;
    HWINEVENTHOOK     m_winEventHookLocation   = nullptr;
    bool              m_overlaySuppressed      = false;  // current overlay suppression (dedupes)
    UINT              m_taskbarCreatedMsg      = 0;      // RegisterWindowMessageW(L"TaskbarCreated")
    std::unique_ptr<TabReader>     m_tabReader;
    std::unique_ptr<FanPopup>      m_fanPopup;
    std::unique_ptr<ConfigWatcher> m_configWatcher;
    Launcher                       m_launcher;
    // Declared after m_launcher: the overlay holds a Launcher* and its worker is
    // joined in ~TaskbarOverlayWindow, so it must destruct BEFORE m_launcher.
    std::unique_ptr<TaskbarOverlayWindow> m_taskbarOverlay;
};
