#include <math.h>
#include <stdio.h>
#include "core/cpu.h"
#include "pal/pal.h"

/* ---------------------------------------------------------------- x87 */
double cpu_fround(uint16_t fcw, double v)
{
    switch ((fcw >> 10) & 3u) {
    case 1:  return floor(v);
    case 2:  return ceil(v);
    case 3:  return trunc(v);
    default: return nearbyint(v);   /* host default mode is round-to-nearest-even */
    }
}

/* Out-of-range or NaN gives the x87 "integer indefinite" value. */
uint32_t cpu_fist32_slow(uint16_t fcw, double v)
{
    double r = cpu_fround(fcw, v);
    return (r >= -2147483648.0 && r <= 2147483647.0) ? (uint32_t)(int32_t)r : 0x80000000u;
}
uint32_t cpu_fist16(uint16_t fcw, double v)
{
    double r = cpu_fround(fcw, v);
    return (r >= -32768.0 && r <= 32767.0) ? (uint32_t)(uint16_t)(int16_t)r : 0x8000u;
}
uint64_t cpu_fist64(uint16_t fcw, double v)
{
    double r = cpu_fround(fcw, v);
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (uint64_t)(int64_t)r : 0x8000000000000000ull;
}

double cpu_f80_rd(gptr a)
{
    uint64_t m = (uint64_t)gmem_rd32(a) | (uint64_t)gmem_rd32(a + 4) << 32;
    uint32_t se = gmem_rd16(a + 8), e = se & 0x7fffu;
    double s = (se & 0x8000u) ? -1.0 : 1.0;
    if (e == 0 && m == 0) return s * 0.0;
    if (e == 0x7fff) return (m << 1) ? NAN : s * INFINITY;
    return s * ldexp((double)m, (int)e - 16383 - 63);
}

void cpu_f80_wr(gptr a, double v)
{
    uint32_t se = signbit(v) ? 0x8000u : 0;
    uint64_t m = 0;
    if (isnan(v))      { se |= 0x7fff; m = 0xc000000000000000ull; }
    else if (isinf(v)) { se |= 0x7fff; m = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double f = frexp(fabs(v), &e);           /* v = f * 2^e, f in [0.5,1) */
        m = (uint64_t)ldexp(f, 64);
        se |= (uint32_t)(e - 1 + 16383);
    }
    gmem_wr32(a, (uint32_t)m);
    gmem_wr32(a + 4, (uint32_t)(m >> 32));
    gmem_wr16(a + 8, (uint16_t)se);
}

void cpu_fxam(cpu_t *c)
{
    double v = ST(0);
    uint16_t f = c->fsw & (uint16_t)~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (signbit(v)) f |= FSW_C1;
    if (isnan(v))           f |= FSW_C0;
    else if (isinf(v))      f |= FSW_C0 | FSW_C2;
    else if (v == 0)        f |= FSW_C3;
    else if (!isnormal(v))  f |= FSW_C2 | FSW_C3;
    else                    f |= FSW_C2;
    c->fsw = f;
}

void cpu_finit(cpu_t *c)
{
    c->fcw = 0x037f;
    c->fsw = 0;
    c->ftop = 0;
}

/* 108-byte protected-mode layout: CW, SW, TW (+pad), 4 pointer dwords, 8 x 80-bit regs. */
void cpu_fsave(cpu_t *c, gptr a)
{
    gmem_wr32(a, c->fcw);
    gmem_wr32(a + 4, cpu_fsw(c));
    gmem_wr32(a + 8, 0);                       /* tag word: all valid */
    for (int k = 12; k < 28; k += 4) gmem_wr32(a + (uint32_t)k, 0);
    for (int k = 0; k < 8; k++) cpu_f80_wr(a + 28 + 10u * (uint32_t)k, ST(k));
    cpu_finit(c);
}

void cpu_frstor(cpu_t *c, gptr a)
{
    c->fcw = (uint16_t)gmem_rd32(a);
    uint32_t sw = gmem_rd32(a + 4);
    c->ftop = (sw >> 11) & 7u;
    c->fsw = (uint16_t)(sw & ~0x3800u);
    for (int k = 0; k < 8; k++) ST(k) = cpu_f80_rd(a + 28 + 10u * (uint32_t)k);
}

static const guest_fn_entry *g_tab;
static uint32_t g_ntab;
uint32_t g_cpu_unimpl_hits;
int g_fp_runfast = 1;


static cpu_extern_fn g_extern, g_trap;
void cpu_set_hooks(cpu_extern_fn ext, cpu_extern_fn trap) { g_extern = ext; g_trap = trap; }

/* Indirect calls: a 256-entry direct-mapped cache in front of the binary search over the ~3000-entry table (24 KB, bigger
 * than the ARM11's 16 KB data cache). The guest's indirect call targets are few (vtables, callbacks, jump-table cases) and
 * the HLE thunks (0xfff00000..: every Win32/COM call, ~1200 DrawPrimitive per frame) are never in the table, so they skip
 * the search entirely. Pure lookup: no semantic change. */
#define CC_N 256u
static struct { uint32_t addr; void (*fn)(cpu_t *); } g_cc[CC_N];

void cpu_set_dispatch(const guest_fn_entry *tab, uint32_t n)
{
    g_tab = tab; g_ntab = n;
    for (uint32_t i = 0; i < CC_N; i++) { g_cc[i].addr = 0xffffffffu; g_cc[i].fn = 0; }   /* 0xffffffff is in the thunk region: never a hit */
}

#ifdef TPM_FN_TIMERS
/* FN_TIMERS builds: inclusive time per cpu_call target (indirect calls, handler tables), reported with the fn timers */
#define CS_N 512
/* only the first 24 calls of a target per window are timed (the timer itself costs 2-3 us); the rest is counted and extrapolated */
static struct { uint32_t addr; uint32_t calls, timed; uint64_t us; } g_cs[CS_N];
void cpu_call_real(cpu_t *c, uint32_t target);
void cpu_call(cpu_t *c, uint32_t target)
{
    uint32_t h = (target * 2654435761u) >> 23, k;
    for (k = 0; k < CS_N; k++, h = (h + 1) & (CS_N - 1))
        if (g_cs[h].addr == target || !g_cs[h].addr) break;
    if (k == CS_N || (g_cs[h].addr == target && g_cs[h].timed >= 24)) { if (k < CS_N) g_cs[h].calls++; cpu_call_real(c, target); return; }
    g_cs[h].addr = target;
    uint64_t t0 = pal_time_us();
    cpu_call_real(c, target);
    g_cs[h].us += pal_time_us() - t0; g_cs[h].timed++; g_cs[h].calls++;
}
void cpu_call_stats_dump(double frames)
{
    for (int i = 0; i < CS_N; i++) if (g_cs[i].timed) g_cs[i].us = g_cs[i].us * g_cs[i].calls / g_cs[i].timed;
    for (int n = 0; n < 14; n++) {
        int b = -1;
        for (int i = 0; i < CS_N; i++) if (g_cs[i].addr && (b < 0 || g_cs[i].us > g_cs[b].us)) b = i;
        if (b < 0 || g_cs[b].us < 150) break;
        pal_log("  call %08lx %7.2f ms/frame %8.1f calls/frame", (unsigned long)g_cs[b].addr, (double)g_cs[b].us / 1000.0 / frames, (double)g_cs[b].calls / frames);
        g_cs[b].us = 0;
    }
    for (int i = 0; i < CS_N; i++) { g_cs[i].addr = 0; g_cs[i].us = 0; g_cs[i].calls = 0; g_cs[i].timed = 0; }
}
#define cpu_call cpu_call_real
#endif

void cpu_call(cpu_t *c, uint32_t target)
{
    if (target >= 0xfff00000u && g_extern) { g_extern(c, target); return; }      /* HLE thunk region (hle.h HLE_THUNK_BASE) */
    uint32_t h = ((target >> 2) ^ (target >> 10)) & (CC_N - 1u);
    if (g_cc[h].addr == target) { g_cc[h].fn(c); return; }
    uint32_t lo = 0, hi = g_ntab;
    while (lo < hi) {                       /* table is sorted by address */
        uint32_t mid = (lo + hi) / 2;
        if (g_tab[mid].addr < target) lo = mid + 1; else hi = mid;
    }
    if (lo < g_ntab && g_tab[lo].addr == target) { g_cc[h].fn = g_tab[lo].fn; __asm__ volatile("" ::: "memory"); g_cc[h].addr = target; g_tab[lo].fn(c); return; }   /* fn first: a guest thread switch never sees addr without fn */
    if (g_extern) { g_extern(c, target); return; }
    cpu_trap(c, target, "call to unknown guest address");
}

void cpu_unimpl(cpu_t *c, uint32_t pc, const char *what)
{
    (void)c;
    if (g_cpu_unimpl_hits++ < 32) pal_log("cpu: UNIMPL %08lx %s", (unsigned long)pc, what);
}

void cpu_trap(cpu_t *c, uint32_t pc, const char *what)
{
    pal_log("cpu: TRAP %08lx %s (esp=%08lx)", (unsigned long)pc, what, (unsigned long)c->r[ESP]);
    pal_log_flush();
    if (g_trap) g_trap(c, pc);
}

uint64_t cpu_rdtsc(cpu_t *c)
{
    (void)c;
    return (uint64_t)pal_time_ms() * 400000ull;   /* a 400 MHz Pentium II-class clock */
}

#ifdef CPU_FN_TRACE
uint32_t g_fn_ring[64];
uint32_t g_fn_ring_pos;
uint32_t g_fn_hist[0x100000];
#include <stdio.h>
#include <stdlib.h>
static void fn_hist_dump(void)
{
    FILE *f = fopen("build-host/fnhist.txt", "w");
    if (!f) return;
    for (int k = 0; k < 60; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < 0x100000; i++) if (g_fn_hist[i] > best) { best = g_fn_hist[i]; bi = i; }
        if (!best) break;
        fprintf(f, "%08x %u\n", 0x400000u + bi, best);
        g_fn_hist[bi] = 0;
    }
    fclose(f);
}
__attribute__((constructor)) static void fn_hist_init(void) { atexit(fn_hist_dump); }
#elif defined(TPM_CALL_COUNT)
uint32_t g_fn_calls;
#endif

void cpu_log_fn_ring(int n)
{
#ifdef CPU_FN_TRACE
    char line[16 * 10 + 40];
    int w = snprintf(line, sizeof line, "cpu: last functions (newest first):");
    for (int k = 1; k <= n && k <= 16; k++)
        w += snprintf(line + w, sizeof line - (size_t)w, " %08lx", (unsigned long)g_fn_ring[(g_fn_ring_pos - (uint32_t)k) & 63u]);
    pal_log("%s", line);
#else
    (void)n;
#endif
}

/* The lifted CPU implements the integer ISA and x87 only, so it reports a
 * plain Pentium (family 5): code that probes for MMX takes its non-MMX path. */
void cpu_cpuid(cpu_t *c)
{
    uint32_t leaf = c->r[EAX];
    if (leaf == 0) {
        c->r[EAX] = 1;
        c->r[EBX] = 0x756e6547u; c->r[EDX] = 0x49656e69u; c->r[ECX] = 0x6c65746eu;   /* "GenuineIntel" */
    } else if (leaf == 1) {
        c->r[EAX] = 0x00000543u;                        /* family 5, model 4, stepping 3 */
        c->r[EBX] = 0; c->r[ECX] = 0;
        c->r[EDX] = (1u << 0) | (1u << 4) | (1u << 8);  /* FPU, TSC, CX8 */
    } else {
        c->r[EAX] = c->r[EBX] = c->r[ECX] = c->r[EDX] = 0;
    }
}
