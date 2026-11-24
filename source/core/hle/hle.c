#include <string.h>
#include "core/guest.h"
#include "core/hle/hle.h"
#include "core/gthread.h"
#include "pal/pal.h"

#define MAX_DEFS   1024
#define MAX_THUNKS 1024
/* Watchdog + preemption checks are not free (a tick query each on 3DS). 
 * 32 HLE calls is well under a millisecond of guest work, so the wall-clock
 * limit and runnable threads still make progress; Sleep/PeekMessage/Flip keep
 * their own unconditional watchdog calls. */
#define HLE_TICK_INTERVAL 32

static hle_def  g_defs[MAX_DEFS];
static int      g_ndefs;
static struct { char dll[32], name[64]; hle_fn fn; uint32_t calls; } g_thunks[MAX_THUNKS];
static uint32_t g_nthunks, g_missing;
static uint32_t g_hle_tick;

static int name_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

void hle_register(const hle_def *defs, int n)
{
    for (int i = 0; i < n && g_ndefs < MAX_DEFS; i++) g_defs[g_ndefs++] = defs[i];
}

uint32_t hle_thunk_for(const char *dll, const char *name)
{
    for (uint32_t i = 0; i < g_nthunks; i++)
        if (name_eq(g_thunks[i].dll, dll) && strcmp(g_thunks[i].name, name) == 0) return HLE_THUNK_BASE + 16u * i;
    if (g_nthunks >= MAX_THUNKS) return 0;
    uint32_t i = g_nthunks++;
    strncpy(g_thunks[i].dll, dll, sizeof g_thunks[i].dll - 1);
    strncpy(g_thunks[i].name, name, sizeof g_thunks[i].name - 1);
    for (int k = 0; k < g_ndefs; k++)
        if (name_eq(g_defs[k].dll, dll) && strcmp(g_defs[k].name, name) == 0) g_thunks[i].fn = g_defs[k].fn;
    if (!g_thunks[i].fn) g_missing++;
    return HLE_THUNK_BASE + 16u * i;
}

int hle_is_thunk(uint32_t addr)
{
    return addr >= HLE_THUNK_BASE && (addr - HLE_THUNK_BASE) / 16u < g_nthunks && (addr & 15u) == 0;
}

const char *hle_name(uint32_t addr)
{
    static char buf[100];
    if (!hle_is_thunk(addr)) return "?";
    uint32_t i = (addr - HLE_THUNK_BASE) / 16u;
    strcpy(buf, g_thunks[i].dll);
    strcat(buf, "!");
    strcat(buf, g_thunks[i].name);
    return buf;
}

volatile uint32_t g_hle_cur;

void hle_dispatch(cpu_t *c, uint32_t addr)
{
    uint32_t i = (addr - HLE_THUNK_BASE) / 16u;
    if (guest_trace_level() >= 1)
        pal_log("hle: %s(%08lx, %08lx, %08lx, %08lx) ret=%08lx", hle_name(addr),
                (unsigned long)ARG(0), (unsigned long)ARG(1), (unsigned long)ARG(2), (unsigned long)ARG(3),
                (unsigned long)gmem_rd32(c->r[ESP]));
    g_thunks[i].calls++;
    if (!g_thunks[i].fn) {
        guest_stop(c, GUEST_STOP_MISSING_HLE, hle_name(addr));
        return;
    }
    if ((++g_hle_tick & (HLE_TICK_INTERVAL - 1)) == 0) {
        guest_watchdog(c);
        gthread_preempt();
    }
    uint32_t outer = g_hle_cur;
    g_hle_cur = addr;
    g_thunks[i].fn(c);
    g_hle_cur = outer;
}

/* Native fast paths that skip the thunk dispatch of a trivial HLE call keep the tick (watchdog + thread preemption). */
void hle_fast_tick(cpu_t *c)
{
    if ((++g_hle_tick & (HLE_TICK_INTERVAL - 1)) == 0) {
        guest_watchdog(c);
        gthread_preempt();
    }
}

uint32_t hle_tick_count(void) { return g_hle_tick; }
uint32_t hle_missing_count(void) { return g_missing; }

/* Diagnostics: the busiest HLE calls since the last report (per frame). */
void hle_report_top(uint32_t frames)
{
    static uint32_t last[MAX_THUNKS];
    for (int k = 0; k < 6; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < g_nthunks; i++) {
            uint32_t d = g_thunks[i].calls - last[i];
            if (d > best) { best = d; bi = i; }
        }
        if (!best) break;
        pal_log("hle top: %s!%s %lu calls/frame", g_thunks[bi].dll, g_thunks[bi].name, (unsigned long)(best / frames));
        last[bi] = g_thunks[bi].calls;
    }
    for (uint32_t i = 0; i < g_nthunks; i++) last[i] = g_thunks[i].calls;
}

uint32_t hle_str_in(char *dst, uint32_t n, gptr src)
{
    if (!src) { if (n) dst[0] = 0; return 0; }
    return gmem_strcpy_out(dst, n, src);
}

void hle_str_out(gptr dst, uint32_t n, const char *src)
{
    if (!n) return;
    uint32_t k = 0;
    for (; k + 1 < n && src[k]; k++) gmem_wr8(dst + k, (uint8_t)src[k]);
    gmem_wr8(dst + k, 0);
}

void hle_kernel32_init(void);
void hle_kernel32_process_init(void);
void hle_user32_init(void);
void hle_mss_init(void);
void hle_mss_process_init(void);
void hle_ddraw_init(void);
void hle_ddraw_process_init(void);
void hle_d3d_init(void);
void hle_dinput_init(void);
void hle_input_init(void);
void hle_thread_init(void);
void hle_dsound_init(void);
void hle_d3d_process_init(void);
void com_reset(void);

void hle_init_all(void)
{
    g_ndefs = 0;
    g_nthunks = 0;
    g_missing = 0;
    g_hle_tick = 0;
    memset(g_thunks, 0, sizeof g_thunks);
    hle_kernel32_init();
    hle_user32_init();
    hle_mss_init();
    com_reset();
    hle_ddraw_init();
    hle_d3d_init();
    hle_dinput_init();
    hle_input_init();
    hle_thread_init();
    hle_dsound_init();
}

void hle_process_init(void)
{
    hle_kernel32_process_init();
    hle_mss_process_init();
    hle_ddraw_process_init();
    hle_d3d_process_init();
}
