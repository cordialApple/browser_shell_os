#include "Store.h"

void Store::Set(HWND hwnd, std::wstring title)
{
    auto& w   = m_windows[hwnd];
    w.hwnd    = hwnd;
    w.title   = std::move(title);
}

void Store::SetMinimized(HWND hwnd, bool minimized)
{
    auto it = m_windows.find(hwnd);
    if (it != m_windows.end())
        it->second.minimized = minimized;
}

void Store::Remove(HWND hwnd)
{
    m_windows.erase(hwnd);
}

bool Store::Has(HWND hwnd) const
{
    return m_windows.count(hwnd) > 0;
}
