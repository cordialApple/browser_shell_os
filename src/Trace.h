#pragma once
#include <windows.h>
#include <TraceLoggingProvider.h>
#include <chrono>

// P.1 (docs/plans/profiler.md): the shell's ONLY coupling to shell_profiler
// (CLAUDE.md rule 8). Provider name + GUID must match profiler/Contract.h /
// EtwSession.cpp's ProviderGuidFromName exactly.
TRACELOGGING_DECLARE_PROVIDER(g_traceProvider);

namespace trace
{
    void Register();
    void Unregister();

    inline long long NowUs()
    {
        using namespace std::chrono;
        return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    }
}

#define TRACE_EVENT(name, ...) TraceLoggingWrite(g_traceProvider, name, __VA_ARGS__)

// RAII scope timer: on destruction, emits an event named `name` carrying duration_us.
// TraceLoggingWrite bakes the event name into a static metadata blob, so it must be a
// literal at the write site — hence the emit lives in a callback the macro instantiates
// with the literal in hand, not in a class member holding a runtime const char*.
template <class Emit>
class TraceScope
{
public:
    explicit TraceScope(Emit emit) noexcept
        : m_emit(emit), m_start(std::chrono::steady_clock::now()) {}
    ~TraceScope()
    {
        m_emit(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_start).count());
    }
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

private:
    Emit                                  m_emit;
    std::chrono::steady_clock::time_point m_start;
};

#define TRACE_SCOPE_CONCAT2(a, b) a##b
#define TRACE_SCOPE_CONCAT(a, b)  TRACE_SCOPE_CONCAT2(a, b)
#define TRACE_SCOPE(name)                                                          \
    TraceScope TRACE_SCOPE_CONCAT(_traceScope_, __LINE__)([](long long us) {       \
        TraceLoggingWrite(g_traceProvider, name, TraceLoggingInt64(us, "duration_us")); \
    })
