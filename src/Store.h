#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>

struct Tab {
    std::wstring title;
    bool         active = false;
};

struct TrackedWindow {
    HWND             hwnd      = nullptr;
    std::wstring     title;
    bool             minimized = false;
    std::vector<Tab> tabs;
    bool             tabsStale = false;
};

class Store
{
public:
    void Set(HWND hwnd, std::wstring title);
    void SetMinimized(HWND hwnd, bool minimized);
    void SetTabs(HWND hwnd, std::vector<Tab> tabs);
    void SetActiveTab(HWND hwnd, int index);
    void MarkTabsStale(HWND hwnd);
    void Remove(HWND hwnd);
    bool Has(HWND hwnd) const;

    const std::unordered_map<HWND, TrackedWindow>& All() const { return m_windows; }

    const std::vector<HWND>& Ordered() const { return m_order; }

private:
    void TraceUpdate() const;

    std::unordered_map<HWND, TrackedWindow> m_windows;
    std::vector<HWND>                       m_order;
};
