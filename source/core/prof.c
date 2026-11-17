#include <string.h>
#include "core/prof.h"
#include "core/cpu.h"
#include "core/gthread.h"
#include "core/hle/hle.h"
#include "pal/pal.h"

#define PROF_N 4096
struct { uint32_t key, cnt; } g_slot[PROF_N];
static uint32_t g_total;

/* g_prof_sub is defined in core/render/r3d.c: the small host tests link the
 * renderer but not this file, and the renderer is its only writer. */

static const char *const g_sub_name[PROF_SUB_COUNT] = {
    "",
    "draw: r3d target bind",
    "draw: r3d state setup",
    "draw: r3d texture convert+upload",
    "draw: r3d matrix (fetch ctx)",
    "draw: r3d vertex fetch+transform",
    "draw: r3d cull+triangle assembly",
    "draw: r3d batch bookkeeping",
    "draw: pal begin frame",
    "draw: pal vertex copy",
    "draw: pal texture bind/filter",
    "draw: pal TEV+state setters",
    "draw: pal GSPGPU_FlushDataCache",
    "draw: pal C3D_DrawArrays",
    "flip: target->staging transfer",
    "flip: staging->screen blit",
};

const char *prof_sub_name(uint32_t sub) { return sub < PROF_SUB_COUNT ? g_sub_name[sub] : "?"; }

void prof_sample(void)
{
    uint32_t sub = g_prof_sub, k;
    if (sub) k = 0x80000000u | sub;        /* inside a tagged renderer section */
    else {
        k = g_gthread_idle ? 1u : g_hle_cur;
#ifdef CPU_FN_TRACE
        if (!k) k = g_fn_ring[(g_fn_ring_pos - 1) & 63u];
#endif
    }
    uint32_t h = (k * 2654435761u) % PROF_N;
    for (int n = 0; g_slot[h].key && g_slot[h].key != k; n++) {
        if (n == PROF_N) return;               /* table full */
        h = (h + 1) % PROF_N;
    }
    g_slot[h].key = k;
    g_slot[h].cnt++;
    g_total++;
}

volatile uint32_t g_prof_epoch;

void prof_reset(void)
{
    g_prof_epoch++;
    memset(g_slot, 0, sizeof g_slot);
    g_total = 0;
}

static uint32_t take(uint32_t key)
{
    for (int i = 0; i < PROF_N; i++) if (g_slot[i].key == key) { uint32_t c = g_slot[i].cnt; g_slot[i].cnt = 0; return c; }
    return 0;
}

/* Entry table (top N) + every renderer subsection in tag order: the tagged
 * sections are separate keys, so DrawPrimitive's own entry keeps only what
 * falls outside them; the subsection block is the split of that call. */
void prof_report(int top)
{
    pal_log("prof: %lu samples", (unsigned long)g_total);
    for (int r = 0; r < top; r++) {
        int best = -1;
        for (int i = 0; i < PROF_N; i++)
            if (g_slot[i].cnt && (g_slot[i].key >> 24) != 0x80u &&
                (best < 0 || g_slot[i].cnt > g_slot[best].cnt)) best = i;
        if (best < 0) break;
        uint32_t k = g_slot[best].key, n = g_slot[best].cnt;
        const char *name = k == 1 ? "(idle: all threads waiting)" : hle_is_thunk(k) ? hle_name(k) : "";
        pal_log("prof: %5.1f%% %08lx %s", 100.0 * n / (g_total ? g_total : 1), (unsigned long)k, name);
        g_slot[best].cnt = 0;
    }
    uint32_t subs = 0;
    for (uint32_t s = 1; s < PROF_SUB_COUNT; s++) {
        uint32_t n = take(0x80000000u | s);
        subs += n;
        if (n) pal_log("prof: %5.1f%% %08lx %s", 100.0 * n / (g_total ? g_total : 1),
                       (unsigned long)(0x80000000u | s), prof_sub_name(s));
    }
    if (subs) pal_log("prof: %5.1f%% draw+flip subsections (of %lu samples)", 100.0 * subs / (g_total ? g_total : 1),
                      (unsigned long)g_total);
}


/* ---- inclusive function timers (FN_TIMERS in tools/lifter/lift.py) ----
 * Wrappers exist only in builds with -DTPM_FN_TIMERS; timer 0 is the
 * per-frame function and every 300 of its calls all timers are logged as ms
 * per frame and calls per frame, then cleared. */
#ifdef TPM_FN_TIMERS
#include "gen/fn_timers_tab.h"
#define FN_TIMER_N (int)(sizeof k_fn_timer_names / sizeof k_fn_timer_names[0])
static uint64_t g_fn_us[FN_TIMER_N];
static uint32_t g_fn_calls[FN_TIMER_N];
#endif

uint64_t fn_timer_begin(void) { return pal_time_us(); }

void fn_timer_end(int idx, uint64_t t0)
{
#ifdef TPM_FN_TIMERS
    if (idx < 0 || idx >= FN_TIMER_N) return;
    g_fn_us[idx] += pal_time_us() - t0;
    g_fn_calls[idx]++;
    if (idx == 0 && g_fn_calls[0] >= 300) {
        double frames = (double)g_fn_calls[0];
        pal_log("fn timers: %u frames, %.1f ms/frame", (unsigned)g_fn_calls[0], (double)g_fn_us[0] / 1000.0 / frames);
        for (int k = 0; k < FN_TIMER_N; k++)
            pal_log("  %s %7.2f ms/frame %8.1f calls/frame", k_fn_timer_names[k],
                    (double)g_fn_us[k] / 1000.0 / frames, (double)g_fn_calls[k] / frames);
        memset(g_fn_us, 0, sizeof g_fn_us);
        memset(g_fn_calls, 0, sizeof g_fn_calls);
    }
#else
    (void)idx; (void)t0;
#endif
}
