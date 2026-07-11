#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "Contract.h"
#include "EtwSession.h"
#include "MetricsView.h"

namespace {

std::atomic<bool> g_running{true};
HANDLE g_stopEvent = nullptr;

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_running.store(false);
        if (g_stopEvent)
            SetEvent(g_stopEvent);
        return TRUE;
    }
    return FALSE;
}

void PrintGuid(const GUID& g) {
    wprintf(L"provider GUID: {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
            g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

void PrintRaw(const DecodedEvent& ev) {
    wprintf(L"%-18s", ev.name.c_str());
    for (auto& f : ev.fields)
        wprintf(L"  %s=%s", f.first.c_str(), f.second.c_str());
    wprintf(L"\n");
}

void Usage() {
    wprintf(L"usage: shell_profiler [--raw] [--csv <path>] [--image <exe>] [--provider <name>]\n"
            L"  --raw            print decoded events as they arrive (no metrics table)\n"
            L"  --csv <path>     append one row per interval to <path>\n"
            L"  --image <exe>    shell image name to sample (default peekbar.exe)\n"
            L"  --provider <name> ETW provider name (default Peekbar.Perf)\n"
            L"Requires elevation or membership in 'Performance Log Users'.\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool raw = false;
    std::wstring csvPath;
    std::wstring image = L"peekbar.exe";
    std::wstring provider = contract::kProviderName;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--raw") {
            raw = true;
        } else if (a == L"--csv" && i + 1 < argc) {
            csvPath = argv[++i];
        } else if (a == L"--image" && i + 1 < argc) {
            image = argv[++i];
        } else if (a == L"--provider" && i + 1 < argc) {
            provider = argv[++i];
        } else {
            Usage();
            return 1;
        }
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    EtwSession session(L"PeekbarProfiler", provider);
    wprintf(L"consuming provider '%s'\n", provider.c_str());
    PrintGuid(session.ProviderGuid());

    DWORD rc = session.Start();
    if (rc != ERROR_SUCCESS) {
        wprintf(L"failed to start ETW session (error %lu). ", rc);
        if (rc == ERROR_ACCESS_DENIED)
            wprintf(L"Run elevated or as a 'Performance Log Users' member.\n");
        else
            wprintf(L"\n");
        return static_cast<int>(rc);
    }

    MetricsView view(image, csvPath, provider);
    session.Consume([&](const DecodedEvent& ev) {
        if (raw)
            PrintRaw(ev);
        else
            view.Ingest(ev);
    });

    wprintf(L"listening... Ctrl+C to stop.\n");

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    while (g_running.load()) {
        if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0)
            break;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed = static_cast<double>(now.QuadPart - prev.QuadPart) / freq.QuadPart;
        prev = now;
        if (!raw)
            view.Tick(elapsed);
    }

    session.Stop();
    wprintf(L"stopped.\n");
    if (g_stopEvent)
        CloseHandle(g_stopEvent);
    return 0;
}
