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

// Coordinator window: owns message loop, hooks, Store (sole writer), subsystems. UI thread only.
class HostWindow
{
public:
    HostWindow() = default;
    ~HostWindow();
    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    bool Create(HINSTANCE instance);

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                      LONG idObject, LONG, DWORD, DWORD) noexcept;

    void ShowFanForChip(HWND chip);
    void ShowFanForButton(int buttonIndex);
    void RequestFolderScans();  // spawn workers per root, post results via kFolderScanResultMsg
    void ScanRootAsync(const std::wstring& root);  // async scan from multiple callers
    void RebuildFolderFanWatchers();  // one watcher per root, dtor stops+joins old watchers
    void QueueFolderFanRescan(const std::wstring& root);  // debounced rescan, deduped pending
    void RestoreWindow(HWND target);
    void RequestSnapshotDebounced(HWND hwnd);
    void UpdateOverlaySuppression();  // hide overlay on Start/Search flyout or fullscreen
    bool FullscreenOnDockMonitor(HWND fg) const;
    void HookTaskbarLocation();  // re-establish on TaskbarCreated (explorer PID changes)

    HWND              m_hwnd             = nullptr;
    Store             m_store;
    std::vector<HWND> m_pendingValidation;
    std::vector<HWND> m_pendingSnapshots;
    HWINEVENTHOOK     m_winEventHook           = nullptr;
    HWINEVENTHOOK     m_winEventHookMinimize   = nullptr;
    HWINEVENTHOOK     m_winEventHookNameChange = nullptr;
    HWINEVENTHOOK     m_winEventHookForeground = nullptr;
    HWINEVENTHOOK     m_winEventHookLocation   = nullptr;
    HWINEVENTHOOK     m_winEventHookFgLocation = nullptr;  // global fg LOCATIONCHANGE (fullscreen)
    HWINEVENTHOOK     m_winEventHookFlyoutCloak = nullptr; // global CLOAKED (Start/Search)
    bool              m_overlaySuppressed      = false;
    int               m_fannedButtonIndex      = -1;  // FolderFan button index or -1 if tabs
    std::vector<std::unique_ptr<ConfigWatcher>> m_folderFanWatchers;  // one per FolderFan root
    std::vector<std::wstring>                   m_pendingFolderFanRescans;  // debounced rescan queue
    UINT              m_taskbarCreatedMsg      = 0;      // RegisterWindowMessageW(L"TaskbarCreated")
    std::unique_ptr<TabReader>     m_tabReader;
    std::unique_ptr<FanPopup>      m_fanPopup;
    std::unique_ptr<ConfigWatcher> m_configWatcher;
    Launcher                       m_launcher;
    std::unique_ptr<TaskbarOverlayWindow> m_taskbarOverlay;  // declared after m_launcher for dtor order
};
