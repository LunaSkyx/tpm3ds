/* Threads, TLS, critical sections, interlocked ops and multimedia timers
 * (KERNEL32/WINMM), on top of core/gthread. Used by the lifted BINKW32.DLL
 * (background I/O thread) and IMUSE.DLL (timeSetEvent music clock). */
#include <stdio.h>
#include <string.h>
#include "core/gthread.h"
#include "core/guest.h"
#include "core/hle/hle.h"
#include "core/hle/hle_handles.h"
#include "pal/pal.h"

/* ---- threads ---- */

/* CreateThread(sec, stack, fn, arg, flags, *tid) */
static void k_CreateThread(cpu_t *c)
{
    if (ARG(4) & 4u) pal_log("hle: CreateThread CREATE_SUSPENDED not supported, starting it");
    int t = gthread_create(ARG(2), ARG(3), ARG(1));
    if (t < 0) { g_hle_last_error = 8; hle_ret(c, 0, 6); return; }
    uint32_t h = hh_alloc(HT_THREAD);
    hle_handle *hh = hh_get(h, HT_THREAD);
    if (hh) hh->index = t;
    if (ARG(5)) gmem_wr32(ARG(5), gthread_id(t));
    hle_ret(c, h, 6);
}
static void k_GetCurrentThreadId(cpu_t *c) { hle_ret(c, gthread_id(gthread_current()), 0); }
static void k_GetCurrentProcessId(cpu_t *c) { hle_ret(c, 0x1000, 0); }
static void k_DisableThreadLibraryCalls(cpu_t *c) { hle_ret(c, 1, 1); }
static void k_GetExitCodeThread(cpu_t *c)
{
    hle_handle *h = hh_get(ARG(0), HT_THREAD);
    if (!h) { hle_ret(c, 0, 2); return; }
    gmem_wr32(ARG(1), gthread_alive(h->index) ? 259u : gthread_exit_code(h->index));   /* STILL_ACTIVE */
    hle_ret(c, 1, 2);
}

/* ---- TLS: indices are global, values per thread ---- */
static uint8_t g_tls_used[GT_TLS];
static void k_TlsAlloc(cpu_t *c)
{
    for (uint32_t i = 0; i < GT_TLS; i++)
        if (!g_tls_used[i]) { g_tls_used[i] = 1; hle_ret(c, i, 0); return; }
    hle_ret(c, 0xffffffffu, 0);
}
static void k_TlsFree(cpu_t *c)
{
    if (ARG(0) < GT_TLS) g_tls_used[ARG(0)] = 0;
    hle_ret(c, ARG(0) < GT_TLS, 1);
}
static void k_TlsGetValue(cpu_t *c)
{
    g_hle_last_error = 0;
    hle_ret(c, ARG(0) < GT_TLS ? gthread_tls()[ARG(0)] : 0, 1);
}
static void k_TlsSetValue(cpu_t *c)
{
    if (ARG(0) < GT_TLS) gthread_tls()[ARG(0)] = ARG(1);
    hle_ret(c, ARG(0) < GT_TLS, 2);
}

/* ---- CRITICAL_SECTION {DebugInfo, LockCount, RecursionCount, OwningThread, LockSemaphore, SpinCount} ---- */
static void k_InitializeCriticalSection(cpu_t *c)
{
    gptr cs = ARG(0);
    for (uint32_t k = 0; k < 24; k += 4) gmem_wr32(cs + k, 0);
    gmem_wr32(cs + 4, 0xffffffffu);
    hle_ret(c, 0, 1);
}
static void k_DeleteCriticalSection(cpu_t *c) { hle_ret(c, 0, 1); }
static void k_EnterCriticalSection(cpu_t *c)
{
    gptr cs = ARG(0);
    uint32_t me = gthread_id(gthread_current());
    while (gmem_rd32(cs + 12) && gmem_rd32(cs + 12) != me) gthread_block_on(cs);
    gmem_wr32(cs + 12, me);
    gmem_wr32(cs + 8, gmem_rd32(cs + 8) + 1);
    gmem_wr32(cs + 4, gmem_rd32(cs + 4) + 1);
    hle_ret(c, 0, 1);
}
static void k_LeaveCriticalSection(cpu_t *c)
{
    gptr cs = ARG(0);
    uint32_t rec = gmem_rd32(cs + 8);
    if (rec && gmem_rd32(cs + 12) == gthread_id(gthread_current())) {
        gmem_wr32(cs + 8, rec - 1);
        gmem_wr32(cs + 4, gmem_rd32(cs + 4) - 1);
        if (rec == 1) { gmem_wr32(cs + 12, 0); gthread_wake(cs); }
    } else pal_log("hle: LeaveCriticalSection(%08lx) by a thread that does not own it", (unsigned long)cs);
    hle_ret(c, 0, 1);
}

/* ---- interlocked: only one guest thread runs at a time ---- */
static void k_InterlockedIncrement(cpu_t *c) { uint32_t v = gmem_rd32(ARG(0)) + 1; gmem_wr32(ARG(0), v); hle_ret(c, v, 1); }
static void k_InterlockedDecrement(cpu_t *c) { uint32_t v = gmem_rd32(ARG(0)) - 1; gmem_wr32(ARG(0), v); hle_ret(c, v, 1); }
static void k_InterlockedExchange(cpu_t *c) { uint32_t v = gmem_rd32(ARG(0)); gmem_wr32(ARG(0), ARG(1)); hle_ret(c, v, 2); }

/* ---- WINMM multimedia timers: each runs its callback on its own guest thread ---- */
#define MAX_TIMERS 8
typedef struct { int used, killed; uint32_t id, delay, cb, user, periodic; } mm_timer;
static mm_timer g_timers[MAX_TIMERS];
static uint32_t g_next_timer_id = 1;

static void timer_body(cpu_t *c, void *p)
{
    mm_timer *t = p;
    uint32_t next = pal_time_ms() + t->delay;
    while (!t->killed) {
        int32_t wait = (int32_t)(next - pal_time_ms());
        gthread_sleep(wait > 0 ? (uint32_t)wait : 0);
        if (t->killed) break;
        uint32_t args[5] = {t->id, 0, t->user, 0, 0};          /* TIMECALLBACK(id, msg, user, dw1, dw2) */
        uint64_t tc0 = pal_time_us();
        guest_callback(c, t->cb, 5, args);
        {   /* audiodbg.txt: iMUSE tick cost, to see what delays the start of the music */
            extern int g_aud_dbg_on; extern uint64_t pal_time_us(void);
            uint64_t d = pal_time_us() - tc0;
            if (g_aud_dbg_on && d > 300u) pal_log("tick: timer %lu callback took %lu us (late by %ld ms)", (unsigned long)t->id, (unsigned long)d, (long)(pal_time_ms() - (next - 0)));
        }
        if (!t->periodic) break;
        next += t->delay ? t->delay : 1;
        if ((int32_t)(pal_time_ms() - next) > 200) next = pal_time_ms();   /* fell behind (debugger, load): resync */
    }
    t->used = 0;
}

/* timeSetEvent(delay, resolution, callback, user, flags) */
static void w_timeSetEvent(cpu_t *c)
{
    if (ARG(4) & 0x10u) { pal_log("hle: timeSetEvent with event handle not supported"); hle_ret(c, 0, 5); return; }
    mm_timer *t = NULL;
    for (int i = 0; i < MAX_TIMERS; i++) if (!g_timers[i].used) { t = &g_timers[i]; break; }
    if (!t) { hle_ret(c, 0, 5); return; }
    *t = (mm_timer){1, 0, g_next_timer_id++, ARG(0), ARG(2), ARG(3), ARG(4) & 1u};
    if (gthread_create_host(timer_body, t) < 0) { t->used = 0; hle_ret(c, 0, 5); return; }
    pal_log("hle: timeSetEvent(%lu ms, cb %08lx, %s) -> %lu", (unsigned long)t->delay, (unsigned long)t->cb,
            t->periodic ? "periodic" : "once", (unsigned long)t->id);
    hle_ret(c, t->id, 5);
}
static void w_timeKillEvent(cpu_t *c)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].used && g_timers[i].id == ARG(0)) { g_timers[i].killed = 1; hle_ret(c, 0, 1); return; }
    hle_ret(c, 97, 1);                                          /* MMSYSERR_INVALPARAM */
}
static void w_timeGetDevCaps(cpu_t *c)
{
    if (ARG(1) >= 8) { gmem_wr32(ARG(0), 1); gmem_wr32(ARG(0) + 4, 1000000); }
    hle_ret(c, 0, 2);
}
static void w_timeBeginPeriod(cpu_t *c) { hle_ret(c, 0, 1); }
static void w_timeEndPeriod(cpu_t *c) { hle_ret(c, 0, 1); }

static const hle_def defs[] = {
    {"KERNEL32.dll", "CreateThread", k_CreateThread},
    {"KERNEL32.dll", "GetCurrentThreadId", k_GetCurrentThreadId},
    {"KERNEL32.dll", "GetCurrentProcessId", k_GetCurrentProcessId},
    {"KERNEL32.dll", "DisableThreadLibraryCalls", k_DisableThreadLibraryCalls},
    {"KERNEL32.dll", "GetExitCodeThread", k_GetExitCodeThread},
    {"KERNEL32.dll", "TlsAlloc", k_TlsAlloc},
    {"KERNEL32.dll", "TlsFree", k_TlsFree},
    {"KERNEL32.dll", "TlsGetValue", k_TlsGetValue},
    {"KERNEL32.dll", "TlsSetValue", k_TlsSetValue},
    {"KERNEL32.dll", "InitializeCriticalSection", k_InitializeCriticalSection},
    {"KERNEL32.dll", "DeleteCriticalSection", k_DeleteCriticalSection},
    {"KERNEL32.dll", "EnterCriticalSection", k_EnterCriticalSection},
    {"KERNEL32.dll", "LeaveCriticalSection", k_LeaveCriticalSection},
    {"KERNEL32.dll", "InterlockedIncrement", k_InterlockedIncrement},
    {"KERNEL32.dll", "InterlockedDecrement", k_InterlockedDecrement},
    {"KERNEL32.dll", "InterlockedExchange", k_InterlockedExchange},
    {"WINMM.dll", "timeSetEvent", w_timeSetEvent},
    {"WINMM.dll", "timeKillEvent", w_timeKillEvent},
    {"WINMM.dll", "timeGetDevCaps", w_timeGetDevCaps},
    {"WINMM.dll", "timeBeginPeriod", w_timeBeginPeriod},
    {"WINMM.dll", "timeEndPeriod", w_timeEndPeriod},
};

void hle_thread_init(void)
{
    memset(g_tls_used, 0, sizeof g_tls_used);
    memset(g_timers, 0, sizeof g_timers);
    g_next_timer_id = 1;
    hle_register(defs, (int)(sizeof defs / sizeof defs[0]));
}
