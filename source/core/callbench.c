/* Perf plan A1: cost of the guest call marshalling (docs/PERF_PLAN.md §7).
 * sdmc:/3ds/tpm/callbench.txt present: before the game starts, run four call-site
 * variants N times each and log CPU ticks per call:
 *   empty     C call to an empty function, no marshalling (floor)
 *   full      today's typical lifted call site (7 regs + eflags each way) + typical leaf
 *   full_x87  the same plus fsw/fcw/2 x87 slots/ftop each way (4453 of 8770 call sites)
 *   lean      lean ABI: callee read set in, write set out (perf plan A2 target)
 * Uses a private 64 KB guest-memory buffer, the game arena is not touched.
 * The call sites copy the shape of tools/lifter/lift.py output. */
#include <stdint.h>
#include <string.h>
#include "core/cpu.h"
#include "core/gmem.h"
#include "pal/pal.h"
#ifdef __3DS__
#include <3ds.h>
#define CB_TICKS() svcGetSystemTick()
#define CB_HZ      268111856.0
#else
#define CB_TICKS() ((uint64_t)pal_time_us())
#define CB_HZ      1000000.0
#endif

uint8_t g_cb_mem[0x10000 + 16] __attribute__((aligned(8)));
#define CB_LOCAL uint8_t *const gm_base = g_cb_mem; const uint32_t gm_size = 0x10000u

void cb_callee_full(cpu_t *cg);
void cb_callee_lean(cpu_t *cg);
void cb_callee_empty(cpu_t *cg);

#define CB_PRE                                                                          \
    cpu_t lc, *const c = &lc;                                                           \
    CB_LOCAL;                                                                           \
    uint32_t ft0 = cg->ftop; double fs_[8];                                             \
    for (int k = 0; k < 8; k++) lc.r[k] = cg->r[k];                                     \
    lc.eflags = cg->eflags; lc.fsw = cg->fsw; lc.fcw = cg->fcw;                         \
    fs_[6] = cg->st[(ft0 + 6u) & 7u]; fs_[7] = cg->st[(ft0 + 7u) & 7u];                 \
    (void)fs_; (void)gm_size
/* caller-side work between calls that keeps the registers live, like real code */
#define CB_WORK                                                                         \
    { uint32_t ea = (uint32_t)(R(ESI) + 0x10u); GWR32(ea, R(EBX) + 0x2000u); }           \
    R(EBX) += R(EAX); R(EDI) ^= R(EBX); R(ESI) = (R(ESI) + 4u) & 0x3ffcu;
#define CB_POST                                                                         \
    for (int k = 0; k < 8; k++) cg->r[k] = lc.r[k];                                     \
    cg->eflags = lc.eflags; cg->fsw = lc.fsw; cg->fcw = lc.fcw; cg->ftop = ft0

__attribute__((noinline)) static void cb_loop_empty(cpu_t *cg, uint32_t n)
{
    CB_PRE;
    for (uint32_t i = 0; i < n; i++) {
        CB_WORK
        { LPUSH(c, 0x00401111u); cg->r[ESP] = lc.r[ESP]; cb_callee_empty(cg); R(ESP) += 4u; }
    }
    CB_POST;
}

__attribute__((noinline)) static void cb_loop_full(cpu_t *cg, uint32_t n)
{
    CB_PRE;
    for (uint32_t i = 0; i < n; i++) {
        CB_WORK
        { LPUSH(c, 0x00401111u); cg->r[EAX] = lc.r[EAX]; cg->r[ECX] = lc.r[ECX]; cg->r[EDX] = lc.r[EDX]; cg->r[EBX] = lc.r[EBX]; cg->r[ESP] = lc.r[ESP]; cg->r[EBP] = lc.r[EBP]; cg->r[ESI] = lc.r[ESI]; cg->r[EDI] = lc.r[EDI]; cg->eflags = lc.eflags; cb_callee_full(cg); lc.r[EAX] = cg->r[EAX]; lc.r[ECX] = cg->r[ECX]; lc.r[EDX] = cg->r[EDX]; lc.r[EBX] = cg->r[EBX]; lc.r[ESP] = cg->r[ESP]; lc.r[EBP] = cg->r[EBP]; lc.r[ESI] = cg->r[ESI]; lc.r[EDI] = cg->r[EDI]; lc.eflags = cg->eflags; }
    }
    CB_POST;
}

__attribute__((noinline)) static void cb_loop_full_x87(cpu_t *cg, uint32_t n)
{
    CB_PRE;
    for (uint32_t i = 0; i < n; i++) {
        CB_WORK
        { LPUSH(c, 0x00401111u); cg->r[EAX] = lc.r[EAX]; cg->r[ECX] = lc.r[ECX]; cg->r[EDX] = lc.r[EDX]; cg->r[EBX] = lc.r[EBX]; cg->r[ESP] = lc.r[ESP]; cg->r[EBP] = lc.r[EBP]; cg->r[ESI] = lc.r[ESI]; cg->r[EDI] = lc.r[EDI]; cg->eflags = lc.eflags; cg->fsw = lc.fsw; cg->fcw = lc.fcw; cg->st[(ft0 + 6u) & 7u] = fs_[6]; cg->st[(ft0 + 7u) & 7u] = fs_[7]; cg->ftop = (ft0 - 0u) & 7u; cb_callee_full(cg); lc.r[EAX] = cg->r[EAX]; lc.r[ECX] = cg->r[ECX]; lc.r[EDX] = cg->r[EDX]; lc.r[EBX] = cg->r[EBX]; lc.r[ESP] = cg->r[ESP]; lc.r[EBP] = cg->r[EBP]; lc.r[ESI] = cg->r[ESI]; lc.r[EDI] = cg->r[EDI]; lc.eflags = cg->eflags; lc.fsw = cg->fsw; lc.fcw = cg->fcw; ft0 = (cg->ftop - 0u) & 7u; fs_[6] = cg->st[(ft0 + 6u) & 7u]; fs_[7] = cg->st[(ft0 + 7u) & 7u]; }
    }
    cg->st[(ft0 + 6u) & 7u] = fs_[6]; cg->st[(ft0 + 7u) & 7u] = fs_[7];
    CB_POST;
}

__attribute__((noinline)) static void cb_loop_lean(cpu_t *cg, uint32_t n)
{
    CB_PRE;
    for (uint32_t i = 0; i < n; i++) {
        CB_WORK
        { LPUSH(c, 0x00401111u); cg->r[ESP] = lc.r[ESP]; cb_callee_lean(cg); lc.r[EAX] = cg->r[EAX]; lc.r[ECX] = cg->r[ECX]; lc.r[ESP] = cg->r[ESP]; }
    }
    CB_POST;
}

static double cb_run(void (*fn)(cpu_t *, uint32_t), uint32_t n)
{
    cpu_t cpu;
    memset(&cpu, 0, sizeof cpu);
    memset(g_cb_mem, 0, sizeof g_cb_mem);
    { uint32_t v = 0x1000u; memcpy(g_cb_mem + 0x8000u, &v, 4); }   /* [esp+4] after the push */
    cpu.r[ESP] = 0x8000u; cpu.r[ESI] = 0x100u;
    fn(&cpu, n / 16);                           /* warm caches */
    uint64_t t0 = CB_TICKS();
    fn(&cpu, n);
    uint64_t t1 = CB_TICKS();
    return (double)(t1 - t0) / n * (268111856.0 / CB_HZ);   /* in 268 MHz cycles */
}

void callbench_run(void)
{
    const uint32_t n = 1000000u;
    for (int rep = 0; rep < 2; rep++) {
        double e = cb_run(cb_loop_empty, n), f = cb_run(cb_loop_full, n);
        double x = cb_run(cb_loop_full_x87, n), l = cb_run(cb_loop_lean, n);
        pal_log("callbench rep %d (cycles/call @268MHz, incl. caller work): empty %.1f  full %.1f  full_x87 %.1f  lean %.1f"
                "  | full-lean %.1f  full_x87-lean %.1f", rep, e, f, x, l, f - l, x - l);
    }
    pal_log_flush();
}
