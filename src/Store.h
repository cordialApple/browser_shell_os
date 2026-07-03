#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>

struct Tab {
    std::wstring title;
};

struct TrackedWindow {
    HWND             hwnd      = nullptr;
    std::wstring     title;
    bool             minimized = false;
    std::vector<Tab> tabs;
};

// UI-thread-only writer. No locking.
class Store
{
public:
    void Set(HWND hwnd, std::wstring title);
    void SetMinimized(HWND hwnd, bool minimized);
    void Remove(HWND hwnd);
    bool Has(HWND hwnd) const;
    bool Empty() const { return m_windows.empty(); }

    const std::unordered_map<HWND, TrackedWindow>& All() const { return m_windows; }

private:
    std::unordered_map<HWND, TrackedWindow> m_windows;
};
