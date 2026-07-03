#pragma once
#include <windows.h>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "Store.h"

struct TabSnapshot {
    HWND             hwnd;
    std::vector<Tab> tabs;
    bool             failed = false;
};

// All UIA tree-shape assumptions live in TabReader.cpp only (CLAUDE.md rule 6).
// Worker thread owns COM; dock thread only calls RequestSnapshot + receives kTabSnapshotMsg.
class TabReader
{
public:
    TabReader(HWND dockHwnd, UINT resultMsg);
    ~TabReader();
    TabReader(const TabReader&) = delete;
    TabReader& operator=(const TabReader&) = delete;

    void RequestSnapshot(HWND hwnd);

private:
    void WorkerLoop();

    HWND m_dockHwnd;
    UINT m_resultMsg;

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::deque<HWND>        m_queue;
    std::atomic<bool>       m_stop{false};
};
