#include "EtwSession.h"

#include "Contract.h"

#include <evntcons.h>
#include <tdh.h>
#include <bcrypt.h>

#include <cwctype>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

std::vector<BYTE> Sha1(const std::vector<BYTE>& data) {
    std::vector<BYTE> digest;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return digest;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(hash, const_cast<PUCHAR>(data.data()),
                           static_cast<ULONG>(data.size()), 0) == 0) {
            digest.resize(20);
            BCryptFinishHash(hash, digest.data(), 20, 0);
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return digest;
}

}  // namespace

GUID ProviderGuidFromName(const std::wstring& name) {
    // Fixed namespace from the TraceLogging / EventSource spec.
    static const BYTE kNamespace[16] = {
        0x48, 0x2C, 0x2D, 0xB2, 0xC3, 0x90, 0x47, 0xC8,
        0x87, 0xF8, 0x1A, 0x15, 0xBF, 0xC1, 0x30, 0xFB,
    };

    std::vector<BYTE> buf(kNamespace, kNamespace + 16);
    for (wchar_t c : name) {
        wchar_t u = static_cast<wchar_t>(std::towupper(c));
        buf.push_back(static_cast<BYTE>((u >> 8) & 0xFF));  // UTF-16 big-endian
        buf.push_back(static_cast<BYTE>(u & 0xFF));
    }

    std::vector<BYTE> h = Sha1(buf);
    GUID g{};
    if (h.size() < 16)
        return g;
    h[7] = static_cast<BYTE>((h[7] & 0x0F) | 0x50);  // RFC 4122 v5; Data1/2/3 byte order mirrors .NET Guid(byte[]) little-endian read
    g.Data1 = static_cast<DWORD>(h[0]) | (static_cast<DWORD>(h[1]) << 8) |
              (static_cast<DWORD>(h[2]) << 16) | (static_cast<DWORD>(h[3]) << 24);
    g.Data2 = static_cast<WORD>(h[4] | (h[5] << 8));
    g.Data3 = static_cast<WORD>(h[6] | (h[7] << 8));
    std::memcpy(g.Data4, h.data() + 8, 8);
    return g;
}

EtwSession::EtwSession(std::wstring sessionName, std::wstring providerName)
    : m_sessionName(std::move(sessionName)),
      m_providerName(std::move(providerName)) {
    m_providerGuid = ProviderGuidFromName(m_providerName);
}

EtwSession::~EtwSession() {
    Stop();
}

DWORD EtwSession::StartSessionOnce() {
    const size_t bytes =
        sizeof(EVENT_TRACE_PROPERTIES) + (m_sessionName.size() + 1) * sizeof(wchar_t);
    m_props.assign(bytes, 0);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(m_props.data());
    p->Wnode.BufferSize = static_cast<ULONG>(bytes);
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;  // QPC timestamps
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    return StartTraceW(&m_sessionHandle, m_sessionName.c_str(), p);
}

DWORD EtwSession::Start() {
    DWORD rc = StartSessionOnce();
    if (rc == ERROR_ALREADY_EXISTS) {
        std::vector<BYTE> stop(m_props.size(), 0);
        auto* sp = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(stop.data());
        sp->Wnode.BufferSize = static_cast<ULONG>(stop.size());
        sp->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(0, m_sessionName.c_str(), sp, EVENT_TRACE_CONTROL_STOP);
        rc = StartSessionOnce();
    }
    if (rc != ERROR_SUCCESS)
        return rc;

    rc = EnableTraceEx2(m_sessionHandle, &m_providerGuid,
                        EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE,
                        0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        Stop();
        return rc;
    }

    EVENT_TRACE_LOGFILEW log{};
    log.LoggerName = const_cast<LPWSTR>(m_sessionName.c_str());
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = &EtwSession::EventRecordThunk;
    log.Context = this;

    m_traceHandle = OpenTraceW(&log);
    if (m_traceHandle == INVALID_PROCESSTRACE_HANDLE) {
        rc = GetLastError();
        Stop();
        return rc ? rc : ERROR_INVALID_HANDLE;
    }
    return ERROR_SUCCESS;
}

void EtwSession::Consume(EventSink sink) {
    m_sink = std::move(sink);
    TRACEHANDLE h = m_traceHandle;
    m_consumer = std::thread([h]() { ProcessTrace(const_cast<PTRACEHANDLE>(&h), 1, nullptr, nullptr); });
}

void EtwSession::Stop() {
    if (m_stopped)
        return;
    m_stopped = true;

    if (m_traceHandle != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(m_traceHandle);
        m_traceHandle = INVALID_PROCESSTRACE_HANDLE;
    }
    if (m_consumer.joinable())
        m_consumer.join();

    if (m_sessionHandle != 0 && !m_props.empty()) {
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(m_props.data());
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(m_sessionHandle, nullptr, p, EVENT_TRACE_CONTROL_STOP);
        m_sessionHandle = 0;
    }
}

void WINAPI EtwSession::EventRecordThunk(PEVENT_RECORD record) {
    if (record && record->UserContext)
        static_cast<EtwSession*>(record->UserContext)->OnEvent(record);
}

void EtwSession::OnEvent(PEVENT_RECORD record) {
    if (!m_sink)
        return;

    ULONG size = 0;
    if (TdhGetEventInformation(record, 0, nullptr, nullptr, &size) != ERROR_INSUFFICIENT_BUFFER)
        return;
    std::vector<BYTE> infoBuf(size);
    auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(infoBuf.data());
    if (TdhGetEventInformation(record, 0, nullptr, info, &size) != ERROR_SUCCESS)
        return;

    DecodedEvent ev;
    if (info->TaskNameOffset)
        ev.name = reinterpret_cast<const wchar_t*>(infoBuf.data() + info->TaskNameOffset);
    if (ev.name.empty())
        ev.name = L"(unnamed)";

    const ULONG ptrSize = (record->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
    auto* userData = static_cast<PBYTE>(record->UserData);
    USHORT remaining = record->UserDataLength;

    std::vector<wchar_t> value(256);
    for (ULONG i = 0; i < info->TopLevelPropertyCount && remaining > 0; ++i) {
        const EVENT_PROPERTY_INFO& epi = info->EventPropertyInfoArray[i];
        if (epi.Flags & (PropertyStruct | PropertyParamCount))
            break;  // nested/array data — out of scope for the flat perf events

        const wchar_t* pname =
            reinterpret_cast<const wchar_t*>(infoBuf.data() + epi.NameOffset);
        USHORT propLen = epi.length;
        USHORT consumed = 0;

        ULONG bufBytes = static_cast<ULONG>(value.size() * sizeof(wchar_t));
        TDHSTATUS st = TdhFormatProperty(
            info, nullptr, ptrSize, epi.nonStructType.InType, epi.nonStructType.OutType,
            propLen, remaining, userData, &bufBytes, value.data(), &consumed);
        if (st == ERROR_INSUFFICIENT_BUFFER) {
            value.resize(bufBytes / sizeof(wchar_t) + 1);
            bufBytes = static_cast<ULONG>(value.size() * sizeof(wchar_t));
            st = TdhFormatProperty(info, nullptr, ptrSize, epi.nonStructType.InType,
                                   epi.nonStructType.OutType, propLen, remaining,
                                   userData, &bufBytes, value.data(), &consumed);
        }
        if (st != ERROR_SUCCESS || consumed == 0)
            break;

        std::wstring val = value.data();
        if (std::wcscmp(pname, contract::kDurationField) == 0) {
            ev.durationUs = _wtoi64(val.c_str());
            ev.hasDuration = true;
        }
        ev.fields.emplace_back(pname, std::move(val));

        userData += consumed;
        remaining = (remaining > consumed) ? static_cast<USHORT>(remaining - consumed) : 0;
    }

    m_sink(ev);
}
