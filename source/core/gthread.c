#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "core/gheap.h"
#include "core/gthread.h"
#include "core/hle/hle.h"
#include "core/guest.h"
#include "pal/pal.h"

#define GT_HOST_STACK   (1024u * 1024u)
#define GT_GUEST_STACK  (256u * 1024u)

enum { GT_FREE = 0, GT_RUN, GT_BLOCKED, GT_DONE };

typedef struct {
    int         state;
    cpu_t       own_cpu;
    cpu_t      *c;
    pal_sem    *sem;
    pal_thread *host;
    uint32_t    wake;               /* runnable when pal_time_ms() >= wake */
    uint32_t    block_key;          /* GT_BLOCKED: waiting for gthread_wake(key) */
    uint32_t    fn, arg;
    void      (*hfn)(cpu_t *, void *);
    void       *harg;
    gptr        region;             /* TIB page + guest stack */
    jmp_buf     jmp;
    uint32_t    exit_code;
    uint32_t    tls[GT_TLS];
} gt;

static gt   g_t[GT_MAX];
static int  g_cur, g_shutdown;
static int  g_stop_reason;
static char g_stop_why[160];

void gthread_init(cpu_t *main_cpu)
{
    for (int i = 0; i < GT_MAX; i++) {
        if (g_t[i].sem) pal_sem_free(g_t[i].sem);
    }
    memset(g_t, 0, sizeof g_t);
    g_cur = 0;
    g_shutdown = 0;
    g_stop_reason = 0;
    g_stop_why[0] = 0;
    g_t[0].state = GT_RUN;
    g_t[0].c = main_cpu;
    g_t[0].sem = pal_sem_new(0);
}

int      gthread_current(void)       { return g_cur; }

/* Stall monitor support (guest.c): one line per live guest thread. Racy on purpose, it only reads. */
void gthread_dump(void)
{
    pal_log("gthread: cur=%d now=%lu", g_cur, (unsigned long)pal_time_ms());
    for (int i = 0; i < GT_MAX; i++)
        if (g_t[i].state != GT_FREE)
            pal_log("gthread: t%d state=%d wake=%lu key=%08lx fn=%08lx esp=%08lx", i, g_t[i].state, (unsigned long)g_t[i].wake,
                    (unsigned long)g_t[i].block_key, (unsigned long)g_t[i].fn, g_t[i].c ? (unsigned long)g_t[i].c->r[ESP] : 0ul);
}
cpu_t   *gthread_cpu(void)           { return g_t[g_cur].c; }
uint32_t gthread_id(int t)           { return 0x1004u + 4u * (uint32_t)t; }
int      gthread_alive(int t)        { return t >= 0 && t < GT_MAX && (g_t[t].state == GT_RUN || g_t[t].state == GT_BLOCKED); }
uint32_t gthread_exit_code(int t)    { return t >= 0 && t < GT_MAX ? g_t[t].exit_code : 0; }
int      gthread_shutting_down(void) { return g_shutdown; }
uint32_t *gthread_tls(void)          { return g_t[g_cur].tls; }

/* Next thread to run after `me`, round-robin, `me` last. -1 if none is ready now. */
volatile int g_gthread_idle;

static int pick(int me, uint32_t now)
{
    for (int k = 1; k <= GT_MAX; k++) {
        int i = (me + k) % GT_MAX;
        if (g_t[i].state == GT_RUN && (int32_t)(now - g_t[i].wake) >= 0) return i;
    }
    return -1;
}

static uint64_t g_run_us[GT_MAX], g_run_start, g_run_log;
static void hand_to(int next)
{
    uint64_t now = pal_time_us();
    if (g_run_start) g_run_us[g_cur] += now - g_run_start;
    g_run_start = now;
    if (now - g_run_log > 4000000u) {            /* CPU share per guest thread, every 4 s */
        if (g_run_log) {
            char line[200]; int k = 0;
            for (int i = 0; i < GT_MAX && k < 180; i++)
                if (g_run_us[i] > 1000u) k += snprintf(line + k, sizeof line - (size_t)k, " t%d=%lu%%", i, (unsigned long)(g_run_us[i] * 100u / (now - g_run_log)));
            pal_log("gthread cpu share:%s", line);
        }
        memset(g_run_us, 0, sizeof g_run_us);
        g_run_log = now;
    }
    g_cur = next;
    pal_sem_post(g_t[next].sem);
}

/* After resuming: a thread being shut down leaves; the main thread honours a stop
 * recorded by another thread. */
static void after_resume(int me)
{
    if (me != 0 && g_shutdown) longjmp(g_t[me].jmp, 1);
    if (me == 0 && g_stop_reason) {
        char why[160];
        snprintf(why, sizeof why, "%s", g_stop_why);
        guest_stop(g_t[0].c, (guest_stop_reason)g_stop_reason, why);
    }
}

static int ready(int i, uint32_t now) { return g_t[i].state == GT_RUN && (int32_t)(now - g_t[i].wake) >= 0; }

/* Gives the token to the next ready thread (sleeping until the earliest wake-up
 * if nobody is ready) and returns once the caller is ready and holds the token. */
static void schedule(int me)
{
    for (;;) {
        uint32_t now = pal_time_ms();
        int n = pick(me, now);
        if (n == me) break;
        if (n >= 0) {
            uint32_t hle = g_hle_cur;
            hand_to(n);
            pal_sem_wait(g_t[me].sem);
            g_hle_cur = hle;
            after_resume(me);
            if (ready(me, pal_time_ms())) break;
            continue;
        }
        uint32_t soonest = 0;
        int any = 0;
        for (int i = 0; i < GT_MAX; i++)
            if (g_t[i].state == GT_RUN && (!any || (int32_t)(g_t[i].wake - soonest) < 0)) { soonest = g_t[i].wake; any = 1; }
        if (!any) {
            g_t[me].state = GT_RUN;          /* report from a sane state */
            if (me == 0) guest_stop(g_t[0].c, GUEST_STOP_TRAP, "deadlock: every guest thread is blocked");
            gthread_stop_from_thread(GUEST_STOP_TRAP, "deadlock: every guest thread is blocked");
        }
        if (me == 0) guest_watchdog(g_t[0].c);
        g_gthread_idle = 1;
        pal_sleep_ms((uint32_t)(soonest - now));
        g_gthread_idle = 0;
    }
    after_resume(me);
}

/* Host code (the 3DS present path) can be tens of milliseconds of straight-line work; Bink's
 * IO/decode thread starves without a yield point inside it. */
void pal_yield_to_guest(void) { gthread_yield(); }

void gthread_yield(void)
{
    int me = g_cur;
    g_t[me].wake = pal_time_ms();
    schedule(me);
}

/* Windows preempts anywhere; we switch only at API calls. Every HLE call checks
 * whether another thread is due, so a thread polling a flag through API calls
 * (timeGetTime, PeekMessage, ...) still lets timers and workers run. */
void gthread_preempt(void)
{
    int me = g_cur, other = 0;
    for (int i = 0; i < GT_MAX; i++) if (i != me && g_t[i].state == GT_RUN) { other = 1; break; }
    if (!other) return;
    uint32_t now = pal_time_ms();
    for (int i = 0; i < GT_MAX; i++)
        if (i != me && ready(i, now)) { gthread_yield(); return; }
}

void gthread_sleep(uint32_t ms)
{
    int me = g_cur;
    if (ms == 0) { gthread_yield(); return; }
    g_t[me].wake = pal_time_ms() + ms;
    schedule(me);
}

void gthread_block_on(uint32_t key)
{
    int me = g_cur;
    g_t[me].state = GT_BLOCKED;
    g_t[me].block_key = key;
    schedule(me);
}

void gthread_wake(uint32_t key)
{
    uint32_t now = pal_time_ms();
    for (int i = 0; i < GT_MAX; i++)
        if (g_t[i].state == GT_BLOCKED && g_t[i].block_key == key) { g_t[i].state = GT_RUN; g_t[i].wake = now; }
}

/* A thread has ended (returned, stopped or shut down): pass the token on for good,
 * to a ready thread or else the earliest sleeper (which waits out its own sleep). */
static void finish(int me)
{
    g_t[me].state = GT_DONE;
    if (g_shutdown) { hand_to(0); return; }
    uint32_t now = pal_time_ms();
    int n = pick(me, now);
    if (n < 0) {
        n = 0;                           /* main never ends first; if blocked it reports the deadlock */
        for (int i = 0; i < GT_MAX; i++)
            if (g_t[i].state == GT_RUN && (g_t[n].state != GT_RUN || (int32_t)(g_t[i].wake - g_t[n].wake) < 0)) n = i;
    }
    hand_to(n);
}

static void tramp(void *p)
{
    int me = (int)(intptr_t)p;
    gt *t = &g_t[me];
    cpu_fp_runfast();
    pal_sem_wait(t->sem);                /* not before the scheduler picks it */
    if (!setjmp(t->jmp) && !g_shutdown) {
        cpu_t *c = t->c;
        if (t->hfn) t->hfn(c, t->harg);
        else {
            PUSH(c, t->arg);
            PUSH(c, GUEST_RET_SENTINEL);
            cpu_call(c, t->fn);
            t->exit_code = c->r[EAX];
        }
    }
    finish(me);
}

static int create(uint32_t fn, uint32_t arg, void (*hfn)(cpu_t *, void *), void *harg, uint32_t stack)
{
    int i = 1;
    while (i < GT_MAX && g_t[i].state != GT_FREE && g_t[i].state != GT_DONE) i++;
    if (i == GT_MAX) { pal_log("gthread: no free thread slot"); return -1; }
    gt *t = &g_t[i];
    if (t->host) { pal_thread_wait(t->host); t->host = NULL; }
    if (t->region) gvirt_release(t->region);
    if (t->sem) pal_sem_free(t->sem);
    memset(t, 0, sizeof *t);

    if (stack < GT_GUEST_STACK) stack = GT_GUEST_STACK;
    stack = (stack + 0xffffu) & ~0xffffu;
    t->region = gvirt_reserve(0, stack + 0x10000u);
    if (!t->region) { pal_log("gthread: no guest memory for a %lu byte stack", (unsigned long)stack); return -1; }
    gptr tib = t->region, lo = t->region + 0x10000u, hi = lo + stack;
    gmem_wr32(tib + 0x00, 0xffffffffu);
    gmem_wr32(tib + 0x04, hi);
    gmem_wr32(tib + 0x08, lo);
    gmem_wr32(tib + 0x18, tib);
    gmem_wr32(tib + 0x20, 0x1000);
    gmem_wr32(tib + 0x24, gthread_id(i));

    t->c = &t->own_cpu;
    cpu_finit(t->c);
    t->c->fs_base = tib;
    t->c->r[ESP] = hi - 64;
    t->c->eflags = 0x202;
    t->c->fop = FL_RAW;
    t->fn = fn;
    t->arg = arg;
    t->hfn = hfn;
    t->harg = harg;
    t->wake = pal_time_ms();
    t->sem = pal_sem_new(0);
    t->state = GT_RUN;
    t->host = pal_thread_spawn(tramp, (void *)(intptr_t)i, GT_HOST_STACK);
    if (!t->host) { t->state = GT_FREE; return -1; }
    pal_log("gthread: thread %d (id %lx) at %08lx arg %08lx, stack %08lx-%08lx", i, (unsigned long)gthread_id(i),
            (unsigned long)fn, (unsigned long)arg, (unsigned long)lo, (unsigned long)hi);
    return i;
}

int gthread_create(uint32_t fn, uint32_t arg, uint32_t stack) { return create(fn, arg, NULL, NULL, stack); }
int gthread_create_host(void (*fn)(cpu_t *, void *), void *arg) { return create(0, 0, fn, arg, 0); }

void gthread_stop_from_thread(int reason, const char *why)
{
    int me = g_cur;
    if (!g_stop_reason) {
        g_stop_reason = reason;
        snprintf(g_stop_why, sizeof g_stop_why, "thread %d: %s", me, why ? why : "");
    }
    longjmp(g_t[me].jmp, 1);
}

int gthread_pending_stop(char *why, uint32_t n)
{
    if (why && n) snprintf(why, n, "%s", g_stop_why);
    return g_stop_reason;
}

void gthread_shutdown(void)
{
    g_shutdown = 1;
    for (int i = 1; i < GT_MAX; i++) {
        if (g_t[i].state == GT_RUN || g_t[i].state == GT_BLOCKED) {
            g_t[i].state = GT_RUN;
            hand_to(i);
            pal_sem_wait(g_t[0].sem);
        }
    }
    g_cur = 0;
    for (int i = 1; i < GT_MAX; i++) {
        if (g_t[i].host) { pal_thread_wait(g_t[i].host); g_t[i].host = NULL; }
        if (g_t[i].sem) { pal_sem_free(g_t[i].sem); g_t[i].sem = NULL; }
        g_t[i].state = GT_FREE;
    }
    g_shutdown = 0;
}
