#pragma once

#include <windows.h>
#include <evntrace.h>

#include <functional>
#include <string>
#include <thread>
#include <vector>

struct DecodedEvent {
    std::wstring name;
    long long durationUs = -1;
    bool hasDuration = false;
    std::vector<std::pair<std::wstring, std::wstring>> fields;
};

using EventSink = std::function<void(const DecodedEvent&)>;

GUID ProviderGuidFromName(const std::wstring& name);

class EtwSession {
public:
    EtwSession(std::wstring sessionName, std::wstring providerName);
    ~EtwSession();

    EtwSession(const EtwSession&) = delete;
    EtwSession& operator=(const EtwSession&) = delete;

    DWORD Start();
    void Consume(EventSink sink);
    void Stop();

    const GUID& ProviderGuid() const { return m_providerGuid; }

private:
    static void WINAPI EventRecordThunk(PEVENT_RECORD record);
    void OnEvent(PEVENT_RECORD record);
    DWORD StartSessionOnce();

    std::wstring m_sessionName;
    std::wstring m_providerName;
    GUID m_providerGuid{};
    TRACEHANDLE m_sessionHandle = 0;
    TRACEHANDLE m_traceHandle = INVALID_PROCESSTRACE_HANDLE;
    std::vector<BYTE> m_props;
    EventSink m_sink;
    std::thread m_consumer;
    bool m_stopped = false;
};
