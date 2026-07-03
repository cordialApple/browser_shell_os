#include <windows.h>
#include <shellapi.h>

#include "DockWindow.h"
#include "WindowMonitor.h"

namespace
{
    constexpr wchar_t kMutexName[] = L"BrowserShellOs_SingleInstance";

    // Crash filter: best-effort ABM_REMOVE before the default handler takes over.
    // Hard kill (Task Manager, kernel crash) still leaks the strip until Explorer
    // restarts — this is documented, expected behavior (observed in step 1.4).
    HWND g_dockHwnd = nullptr;

    LONG CALLBACK CrashFilter(EXCEPTION_POINTERS*)
    {
        if (g_dockHwnd)
        {
            APPBARDATA abd = {};
            abd.cbSize = sizeof(abd);
            abd.hWnd   = g_dockHwnd;
            SHAppBarMessage(ABM_REMOVE, &abd);
            g_dockHwnd = nullptr;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR lpCmdLine, int)
{
#ifdef _DEBUG
    // Debug scan: run IsBrowserFrame against all top-level windows, print to debugger.
    if (wcsstr(lpCmdLine, L"--scan"))
    {
        const auto frames = ScanBrowserFrames();
        wchar_t buf[512];
        swprintf_s(buf, L"[scan] %zu browser frame(s) found\n", frames.size());
        OutputDebugStringW(buf);
        for (HWND h : frames)
        {
            wchar_t title[256] = {};
            GetWindowTextW(h, title, _countof(title));
            swprintf_s(buf, L"  hwnd=0x%p  %s\n", static_cast<void*>(h), title);
            OutputDebugStringW(buf);
        }
        return 0;
    }
#endif

    // Single instance: second launch exits immediately; two appbars = geometry chaos.
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // Must be FIRST, before any window/USER32 UI call. Cannot set later.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        CloseHandle(mutex);
        return 1;
    }

    DockWindow dock;
    if (!dock.Create(instance))
    {
        CloseHandle(mutex);
        return 1;
    }

    g_dockHwnd = dock.Hwnd();
    SetUnhandledExceptionFilter(CrashFilter);

    MSG msg;
    BOOL result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) != 0)
    {
        if (result == -1)
        {
            CloseHandle(mutex);
            return 1;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
