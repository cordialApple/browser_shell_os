#include <windows.h>

#include "HostWindow.h"
#include "WindowMonitor.h"
#include "Trace.h"

namespace
{
    constexpr wchar_t kMutexName[] = L"Peekbar_SingleInstance";

    // RAII ensures provider unregisters on every exit path (like AppBar cleanup)
    struct TraceGuard
    {
        TraceGuard()  { trace::Register(); }
        ~TraceGuard() { trace::Unregister(); }
    };
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR lpCmdLine, int)
{
#ifdef _DEBUG
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

    // Second launch must exit; two overlays would fight over taskbar gap
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    const TraceGuard traceGuard;

    // Order-critical: must be before any window/UI32 call
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        CloseHandle(mutex);
        return 1;
    }

    HostWindow dock;
    if (!dock.Create(instance))
    {
        CloseHandle(mutex);
        return 1;
    }

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
