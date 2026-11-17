/* Suspend diagnostics: between the app's suspend and restore hooks (HOME / sleep) every kernel wait and every IPC request
 * is logged BEFORE it is made, with a flush, so a call that never returns is the last line of log.txt. Linked with
 * -Wl,--wrap for these svc stubs (see Makefile); outside the window they are plain pass-throughs. */
#include <3ds.h>
#include <stdio.h>
#include "pal/pal.h"

volatile int g_svc_trace;
static volatile int g_in_trace;

extern Result __real_svcSendSyncRequest(Handle h);
extern Result __real_svcWaitSynchronization(Handle h, s64 ns);
extern Result __real_svcWaitSynchronizationN(s32 *out, Handle *hs, s32 n, bool all, s64 ns);
extern Result __real_svcArbitrateAddress(Handle arb, u32 addr, ArbitrationType type, s32 value, s64 ns);

static int tracing(void) { return g_svc_trace && !g_in_trace; }

/* The log flush itself is an IPC request (file system) and would overwrite the command buffer the caller has already filled
 * for the request being traced: save it before logging and put it back afterwards, or the traced call sends garbage. */
#define CB_SAVE   u32 saved_cb[64]; { u32 *cbp = getThreadCommandBuffer(); for (int i_ = 0; i_ < 64; i_++) saved_cb[i_] = cbp[i_]; }
#define CB_RESTORE { u32 *cbp = getThreadCommandBuffer(); for (int i_ = 0; i_ < 64; i_++) cbp[i_] = saved_cb[i_]; }

Result __wrap_svcSendSyncRequest(Handle h)
{
    if (!tracing()) return __real_svcSendSyncRequest(h);
    g_in_trace = 1;
    CB_SAVE
    u32 *cb = saved_cb;
    pal_log("svc: SendSyncRequest handle %08lx cmd %08lx a1 %08lx a2 %08lx (tls %p)", (unsigned long)h, (unsigned long)cb[0], (unsigned long)cb[1], (unsigned long)cb[2], (void *)cb);
    pal_log_flush();
    CB_RESTORE
    g_in_trace = 0;
    Result r = __real_svcSendSyncRequest(h);
    if (g_svc_trace) { g_in_trace = 1; CB_SAVE pal_log("svc:   -> rc %08lx", (unsigned long)r); CB_RESTORE g_in_trace = 0; }
    return r;
}

Result __wrap_svcWaitSynchronization(Handle h, s64 ns)
{
    if (!tracing()) return __real_svcWaitSynchronization(h, ns);
    g_in_trace = 1;
    CB_SAVE
    pal_log("svc: WaitSynchronization handle %08lx timeout %lld", (unsigned long)h, (long long)ns);
    pal_log_flush();
    CB_RESTORE
    g_in_trace = 0;
    Result r = __real_svcWaitSynchronization(h, ns);
    if (g_svc_trace) { g_in_trace = 1; CB_SAVE pal_log("svc:   -> rc %08lx", (unsigned long)r); CB_RESTORE g_in_trace = 0; }
    return r;
}

Result __wrap_svcWaitSynchronizationN(s32 *out, Handle *hs, s32 n, bool all, s64 ns)
{
    if (!tracing()) return __real_svcWaitSynchronizationN(out, hs, n, all, ns);
    g_in_trace = 1;
    CB_SAVE
    pal_log("svc: WaitSynchronizationN n=%d all=%d h0 %08lx h1 %08lx timeout %lld", (int)n, (int)all, n > 0 ? (unsigned long)hs[0] : 0ul, n > 1 ? (unsigned long)hs[1] : 0ul, (long long)ns);
    pal_log_flush();
    CB_RESTORE
    g_in_trace = 0;
    Result r = __real_svcWaitSynchronizationN(out, hs, n, all, ns);
    if (g_svc_trace) { g_in_trace = 1; CB_SAVE pal_log("svc:   -> rc %08lx", (unsigned long)r); CB_RESTORE g_in_trace = 0; }
    return r;
}

Result __wrap_svcArbitrateAddress(Handle arb, u32 addr, ArbitrationType type, s32 value, s64 ns)
{
    if (!tracing() || type == ARBITRATION_SIGNAL) return __real_svcArbitrateAddress(arb, addr, type, value, ns);
    g_in_trace = 1;
    CB_SAVE
    pal_log("svc: ArbitrateAddress addr %08lx type %d value %ld timeout %lld", (unsigned long)addr, (int)type, (long)value, (long long)ns);
    pal_log_flush();
    CB_RESTORE
    g_in_trace = 0;
    return __real_svcArbitrateAddress(arb, addr, type, value, ns);
}
