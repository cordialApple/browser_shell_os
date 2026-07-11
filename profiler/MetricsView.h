#pragma once

#include <windows.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "EtwSession.h"

class MetricsView {
public:
    MetricsView(std::wstring shellImageName, std::wstring csvPath, std::wstring providerName);
    ~MetricsView();

    void Ingest(const DecodedEvent& ev);
    void Tick(double intervalSec);

private:
    struct Agg {
        unsigned long long total = 0;
        unsigned long long interval = 0;
        std::vector<double> durations;
    };

    struct ProcSample {
        bool found = false;
        double cpuPct = 0.0;
        SIZE_T workingSetKB = 0;
        DWORD handles = 0;
    };

    ProcSample SampleProcess(double intervalSec);
    void OpenCsvIfNeeded();

    std::wstring m_imageName;
    std::wstring m_csvPath;
    std::wstring m_providerName;
    FILE* m_csv = nullptr;

    std::mutex m_mu;
    std::map<std::wstring, Agg> m_aggs;

    HANDLE m_proc = nullptr;
    DWORD m_pid = 0;
    ULONGLONG m_prevProcTime100ns = 0;
    bool m_havePrev = false;
};
