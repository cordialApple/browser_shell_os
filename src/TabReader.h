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
    // Restore+foreground must already be in flight on the UI thread; the worker
    // gates on window readiness before touching UIA. Title-first match, index tiebreak.
    void RequestActivate(HWND hwnd, std::wstring wantedTitle, int fallbackIndex);

private:
    enum class ReqKind { Snapshot, Activate };
    struct Request {
        ReqKind      kind;
        HWND         hwnd;
        std::wstring wantedTitle;
        int          fallbackIndex = 0;
    };

    void WorkerLoop();

    // Set by the worker when WorkerLoop() returns. shared_ptr so it outlives the
    // TabReader when a wedged worker is detached at teardown (F-02 bounded join).
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
