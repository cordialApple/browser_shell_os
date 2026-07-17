#pragma once
#include <windows.h>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include "Store.h"

#include <string>

struct TabSnapshot {
    HWND             hwnd;
    std::vector<Tab> tabs;
    bool             failed = false;
};

enum class ActivateOutcome { Selected, NoMatch, PatternUnavailable, Failed };

struct TabActivateResult {
    HWND             hwnd;
    ActivateOutcome  outcome = ActivateOutcome::Failed;
    int              matchedIndex = -1;
    std::vector<Tab> freshTabs;
};

// All UIA tree-shape assumptions live in TabReader.cpp only (CLAUDE.md rule 6).
// Worker thread owns COM; dock thread only calls Request* + receives result messages.
class TabReader
{
public:
    TabReader(HWND dockHwnd, UINT snapshotMsg, UINT activateMsg);
    ~TabReader();
    TabReader(const TabReader&) = delete;
    TabReader& operator=(const TabReader&) = delete;

    void RequestSnapshot(HWND hwnd);
    void RequestActivate(HWND hwnd, std::wstring wantedTitle, int fallbackIndex,
                         long long tClickUs, long long tRestoreUs);
    void RequestKeystrokeHop(HWND hwnd, int activeIndex, int targetIndex, int tabCount,
                             long long tClickUs, long long tRestoreUs);

private:
    enum class ReqKind { Snapshot, Activate, KeystrokeHop };
    struct Request {
        ReqKind      kind;
        HWND         hwnd;
        std::wstring wantedTitle;
        int          fallbackIndex = 0;
        long long    tClickUs      = 0;
        long long    tRestoreUs    = 0;
        int          targetIndex   = 0;
        int          tabCount      = 0;
        int          activeIndex   = 0;
    };

    void WorkerLoop();

    struct ExitSignal {
        std::mutex              m;
        std::condition_variable cv;
        bool                    exited = false;
    };

    HWND m_dockHwnd;
    UINT m_snapshotMsg;
    UINT m_activateMsg;

    std::thread                 m_thread;
    std::mutex                  m_mutex;
    std::condition_variable     m_cv;
    std::deque<Request>         m_queue;
    std::atomic<bool>           m_stop{false};
    std::shared_ptr<ExitSignal> m_exit;
};
