#include "Trace.h"

#pragma comment(lib, "advapi32.lib")

// GUID must match profiler/EtwSession.cpp; do not hand-edit without re-deriving both sides
TRACELOGGING_DEFINE_PROVIDER(
    g_traceProvider,
    "Peekbar.Perf",
    (0xc943a625, 0x2d01, 0x532a, 0xb9, 0xe9, 0x19, 0x61, 0x39, 0x74, 0xd9, 0xad));

namespace trace
{
    void Register()   { TraceLoggingRegister(g_traceProvider); }
    void Unregister()  { TraceLoggingUnregister(g_traceProvider); }
}
