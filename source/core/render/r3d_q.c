/* Draw thread queue, see r3d_q.h. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "core/render/r3d.h"
#include "core/render/r3d_gpu.h"
#include "core/render/r3d_q.h"
#include "core/cpu.h"
#include "pal/pal.h"

/* r3d_gpu.c: the state the worker draws with, and the worker-side entry points */
extern r3d_state *g_rp;
extern r3d_state  g_r3d_w;
void r3d_gpu_draw_h(uint32_t prim, uint32_t fvf, const uint8_t *vsrc, uint32_t nverts, const uint8_t *isrc, uint32_t nindices);
int  r3d_gpu_clear_h(uint32_t nrects, const uint32_t *rects4, uint32_t flags, uint32_t color, float z);

int g_r3d_q_on;
static uint64_t st_enq_us, st_enq_bytes, st_enq_n, st_state_n; static uint64_t st_fence_us, st_space_us, st_busy_us, st_last_log; static uint32_t st_fences, st_spacewaits;

enum { Q_WRAP = 1, Q_STATE, Q_DRAW, Q_CLEAR, Q_FLUSH, Q_FENCE, Q_PRESENT, Q_FLIP };
typedef struct { uint32_t a, b, c, d; } q4;
uint32_t g_r3d_q_since_present;
void r3d_gpu_present_w(uint32_t pix, uint32_t ver, uint32_t pitch, uint32_t h);
void r3d_gpu_flip_w(uint32_t fpix, uint32_t fver, uint32_t bpix, uint32_t bver);
typedef struct { uint32_t type, len; } qhdr;                          /* len: bytes of the whole entry, multiple of 8 */
typedef struct { uint32_t prim, fvf, nverts, nindices; } qdraw;
typedef struct { uint32_t nrects, flags, color, zbits; } qclear;
typedef struct { r3d_state st; dd_surface target, zbuf; uint32_t has_t, has_z; } qstate;
static dd_surface w_target, w_zbuf;                                   /* worker side copies */
static gptr snap_tpix, snap_zpix; static uint32_t snap_tver, snap_zver;

#define Q_SIZE (1u << 20)                                             /* power of two */
#define Q_MASK (Q_SIZE - 1u)
static uint8_t           *q_buf;
static volatile uint32_t  q_w, q_r;                                   /* monotonic byte counters (producer / consumer) */
static volatile uint32_t  q_posted, q_done;                           /* real entries queued / processed */
static volatile int       q_waiting, q_quit;
static pal_sem           *q_sem, *q_space, *q_fence_sem;
static pal_thread        *q_thr;
static __thread int        q_in_worker;                                /* set on the worker thread only (thread-local) */

#define PAD8(n) (((n) + 7u) & ~7u)

int r3d_q_on_worker(void) { return q_in_worker; }

/* the front state last sent to the worker; compared field-wise so the queue never depends on every writer bumping g_r3d.gen */
static r3d_state snap;
static int snap_valid;

void r3d_q_state_reset(void) { snap_valid = 0; }

static void q_kick(void);
static uint8_t *q_alloc(uint32_t type, uint32_t payload)
{
    uint32_t len = PAD8((uint32_t)sizeof(qhdr) + payload);
    for (;;) {
        uint32_t w = q_w, r = q_r, pos = w & Q_MASK, pad = (pos + len > Q_SIZE) ? Q_SIZE - pos : 0;
        if ((w - r) + pad + len <= Q_SIZE) {
            if (pad) { qhdr *p = (qhdr *)(q_buf + pos); p->type = Q_WRAP; p->len = pad; w += pad; pos = 0; }
            qhdr *h = (qhdr *)(q_buf + pos);
            h->type = type; h->len = len;
            q_w = w;                                                   /* start of this entry (the wrap pad is accounted) */
            return (uint8_t *)(h + 1);
        }
        q_waiting = 1;
        __sync_synchronize();
        if ((q_w - q_r) + pad + len <= Q_SIZE) { q_waiting = 0; continue; }
        q_kick();
        pal_sem_wait(q_space); st_spacewaits++;
    }
}

/* The worker is woken in batches: a semaphore post is a syscall plus a thread switch (~40 us in the emulator), more than a draw costs.
 * It drains everything published when it wakes. Kicks: 16 KB queued, and every entry that someone waits for (fence/flush/present/flip). */
static uint32_t q_kicked_w;
static void q_kick(void)
{
    __sync_synchronize();
    q_kicked_w = q_w;
    pal_sem_post(q_sem);
}
static void q_commit(uint8_t *payload)
{
    qhdr *h = (qhdr *)payload - 1;
    __sync_synchronize();
    q_w += h->len;
    q_posted++;
    if (q_w - q_kicked_w >= 16384u) q_kick();
}
static void q_commit_k(uint8_t *payload) { q_commit(payload); q_kick(); }

int g_r3d_q_stress_ms;                  /* debug: drawthread.txt contains a number -> the worker sleeps that many ms after every 8th entry (widens race windows) */
static void q_worker(void *arg)
{
    (void)arg;
    q_in_worker = 1;
    if (g_fp_runfast) cpu_fp_runfast();
    for (;;) {
        pal_sem_wait(q_sem);
        __sync_synchronize();
        if (q_quit) break;
        for (;;) {
            uint32_t r = q_r;
            if (r == q_w) break;
            qhdr *h = (qhdr *)(q_buf + (r & Q_MASK));
            if (h->type == Q_WRAP) { q_r = r + h->len; if (q_waiting) { q_waiting = 0; pal_sem_post(q_space); } continue; }
            const uint8_t *pl = (const uint8_t *)(h + 1);
            switch (h->type) {
            case Q_STATE: {
                /* the render target and z-buffer are copied: the game thread swaps their pixel pointers at every Flip and bumps their
                 * versions while queued draws for the previous frame are still waiting */
                const qstate *x = (const qstate *)pl;
                memcpy(&g_r3d_w, &x->st, sizeof g_r3d_w);
                w_target = x->target; w_zbuf = x->zbuf;
                g_r3d_w.target = x->has_t ? &w_target : NULL;
                g_r3d_w.zbuf = x->has_z ? &w_zbuf : NULL;
                break;
            }
            case Q_DRAW: {
                const qdraw *d = (const qdraw *)pl;
                const uint8_t *v = pl + sizeof *d;
                r3d_gpu_draw_h(d->prim, d->fvf, v, d->nverts, d->nindices ? v + PAD8(d->nverts * r3d_fvf_stride(d->fvf)) : NULL, d->nindices);
                break;
            }
            case Q_CLEAR: {
                const qclear *c = (const qclear *)pl;
                float z; memcpy(&z, &c->zbits, 4);
                r3d_gpu_clear_h(c->nrects, (const uint32_t *)(pl + sizeof *c), c->flags, c->color, z);
                break;
            }
            case Q_PRESENT: { const q4 *x = (const q4 *)pl; r3d_gpu_present_w(x->a, x->b, x->c, x->d); break; }
            case Q_FLIP:    { const q4 *x = (const q4 *)pl; r3d_gpu_flip_w(x->a, x->b, x->c, x->d); break; }
            case Q_FLUSH:
                r3d_gpu_flush();
                break;
            case Q_FENCE:
                pal_sem_post(q_fence_sem);
                break;
            default:
                break;
            }
            __sync_synchronize();
            q_r = r + h->len;
            q_done++;
            if (g_r3d_q_stress_ms && (q_done & 7u) == 0u) pal_sleep_ms((uint32_t)g_r3d_q_stress_ms);
            if (q_waiting) { q_waiting = 0; pal_sem_post(q_space); }
        }
    }
    q_in_worker = 0;
}

int r3d_q_start(void)
{
    if (g_r3d_q_on) return 1;
    q_buf = malloc(Q_SIZE);
    q_sem = pal_sem_new(0); q_space = pal_sem_new(0); q_fence_sem = pal_sem_new(0);
    if (!q_buf || !q_sem || !q_space || !q_fence_sem) { free(q_buf); q_buf = NULL; return 0; }
    q_w = q_r = q_posted = q_done = 0; q_quit = 0; q_waiting = 0;
    g_rp = &g_r3d_w;                                                   /* from here the GPU front end draws with the snapshot copy */
    memcpy(&g_r3d_w, &g_r3d, sizeof g_r3d_w);
    q_thr = pal_thread_spawn_bg(q_worker, NULL, 64 * 1024);
    if (!q_thr) { g_rp = &g_r3d; return 0; }
    snap_valid = 0;
    g_r3d_q_on = 1;
    pal_log("r3d: draw thread on core 2 (%u KB queue)", Q_SIZE >> 10);
    return 1;
}

void r3d_q_fence(void)
{
    if (!g_r3d_q_on || q_in_worker) return;
    if (q_done == q_posted) return;
    uint8_t *p = q_alloc(Q_FENCE, 0);
    q_commit_k(p);
    pal_sem_wait(q_fence_sem);
    st_fences++;
}

void r3d_q_flush(void)
{
    if (!g_r3d_q_on) { r3d_gpu_flush(); return; }
    uint8_t *p = q_alloc(Q_FLUSH, 0);
    q_commit_k(p);
    r3d_q_fence();
}

void r3d_q_shutdown(void)
{
    if (!g_r3d_q_on) return;
    r3d_q_fence();
    g_r3d_q_on = 0;
    q_quit = 1;
    q_kick();
    pal_thread_wait(q_thr);
    q_thr = NULL;
    g_rp = &g_r3d;
}

/* a full copy of the front state when it differs from what the worker holds */
static void send_state_if_changed(void)
{
    /* gen: render/stage/viewport state, xf_gen: matrices (see r3d.h); textures and targets are plain pointer writes */
    if (snap_valid && snap.gen == g_r3d.gen && snap.xf_gen == g_r3d.xf_gen && snap.legacy_blend == g_r3d.legacy_blend &&
        snap.tex[0] == g_r3d.tex[0] && snap.tex[1] == g_r3d.tex[1] && snap.target == g_r3d.target && snap.zbuf == g_r3d.zbuf &&
        (!g_r3d.target || (snap_tpix == g_r3d.target->pixels && snap_tver == g_r3d.target->version)) &&
        (!g_r3d.zbuf || (snap_zpix == g_r3d.zbuf->pixels && snap_zver == g_r3d.zbuf->version)))
        return;
    st_state_n++;
    uint8_t *p = q_alloc(Q_STATE, (uint32_t)sizeof(qstate));
    qstate *x = (qstate *)p;
    memcpy(&x->st, &g_r3d, sizeof(r3d_state));
    x->has_t = g_r3d.target != NULL; x->has_z = g_r3d.zbuf != NULL;
    memset(&x->target, 0, sizeof x->target); memset(&x->zbuf, 0, sizeof x->zbuf);
    if (g_r3d.target) { x->target = *g_r3d.target; snap_tpix = g_r3d.target->pixels; snap_tver = g_r3d.target->version; }
    if (g_r3d.zbuf)   { x->zbuf = *g_r3d.zbuf;     snap_zpix = g_r3d.zbuf->pixels;     snap_zver = g_r3d.zbuf->version; }
    q_commit(p);
    memcpy(&snap, &g_r3d, sizeof snap);
    snap_valid = 1;
}

void r3d_q_draw(uint32_t prim, uint32_t fvf, const uint8_t *verts, uint32_t nverts, const uint8_t *indices, uint32_t nindices)
{
    send_state_if_changed();
    g_r3d_q_since_present++;
    uint32_t vb = nverts * r3d_fvf_stride(fvf), ib = nindices * 2u;
    uint8_t *p = q_alloc(Q_DRAW, (uint32_t)sizeof(qdraw) + PAD8(vb) + ib);
    qdraw *d = (qdraw *)p;
    d->prim = prim; d->fvf = fvf; d->nverts = nverts; d->nindices = nindices;
    memcpy(p + sizeof *d, verts, vb);
    if (nindices) memcpy(p + sizeof *d + PAD8(vb), indices, ib);
    q_commit(p);
    st_enq_bytes += vb + ib; st_enq_n++;
}

static void q_send4(uint32_t type, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint8_t *p = q_alloc(type, sizeof(q4));
    q4 *x = (q4 *)p; x->a = a; x->b = b; x->c = c; x->d = d;
    q_commit_k(p);
}
void r3d_q_present(uint32_t pix, uint32_t ver, uint32_t pitch, uint32_t h) { q_send4(Q_PRESENT, pix, ver, pitch, h); g_r3d_q_since_present = 0; }
void r3d_q_flip(uint32_t fpix, uint32_t fver, uint32_t bpix, uint32_t bver) { q_send4(Q_FLIP, fpix, fver, bpix, bver); }

void r3d_q_clear(uint32_t nrects, const uint32_t *rects4, uint32_t flags, uint32_t color, float z)
{
    send_state_if_changed();
    g_r3d_q_since_present++;
    uint8_t *p = q_alloc(Q_CLEAR, (uint32_t)sizeof(qclear) + 16u * nrects);
    qclear *c = (qclear *)p;
    c->nrects = nrects; c->flags = flags; c->color = color; memcpy(&c->zbits, &z, 4);
    if (nrects) memcpy(p + sizeof *c, rects4, 16u * nrects);
    q_commit(p);
}
