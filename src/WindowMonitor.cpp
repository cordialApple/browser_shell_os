#include "WindowMonitor.h"

namespace
{
    bool IsTargetClass(HWND hwnd)
    {
        wchar_t cls[64];
        return GetClassNameW(hwnd, cls, _countof(cls))
            && wcscmp(cls, L"Chrome_WidgetWin_1") == 0;
    }

    bool IsTargetProcess(HWND hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return false;

        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc) return false;

        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        const bool ok = QueryFullProcessImageNameW(proc, 0, path, &len);
        CloseHandle(proc);
        if (!ok) return false;

        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        return _wcsicmp(name, L"chrome.exe") == 0
            || _wcsicmp(name, L"msedge.exe") == 0;
    }
}

bool IsBrowserFrame(HWND hwnd)
{
    return IsWindow(hwnd)
        && IsWindowVisible(hwnd)
        && GetWindow(hwnd, GW_OWNER) == nullptr
        && GetWindowTextLengthW(hwnd) > 0
        && IsTargetClass(hwnd)
        && IsTargetProcess(hwnd);
}

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) noexcept
{
    auto* out = reinterpret_cast<std::vector<HWND>*>(lp);
    try { if (IsBrowserFrame(hwnd)) out->push_back(hwnd); }
    catch (...) {}
    return TRUE;
}

std::vector<HWND> ScanBrowserFrames()
{
    std::vector<HWND> result;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&result));
    return result;
}
