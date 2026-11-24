/* GPU front end of the Direct3D HLE (used where pal_gpu_init succeeds, i.e. 3DS).
 *
 * The core keeps doing what is platform-free: vertex fetch, transform to clip
 * space, culling, fog factors, texture format conversion (with colour key).
 * The pal draws clip-space triangle lists with a small fixed-function state.
 *
 * Coherence with guest memory: each of the two GPU targets mirrors one guest
 * back-buffer address (g_tg[].addr as of version g_tg[].ver). Before the CPU
 * touches a surface (Lock, Blt, the software fallback) r3d_gpu_sync() stores
 * GPU results into guest memory; when the CPU changed the target since, the
 * next GPU operation loads guest memory first, unless it is a full clear. A
 * Flip adopts the swapped pointers without reloading and presents directly.
 *
 * Batching: the game issues ~1000 small DrawPrimitive calls per frame in a
 * level. Consecutive draws with identical state are collected and handed to the
 * pal as one triangle list; anything that changes what those draws would see
 * (texture upload, target load/store, clear, target switch) flushes first. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "core/prof.h"
#include "core/render/r3d.h"
#include "core/render/r3d_gpu.h"
#include "core/render/r3d_q.h"
#include "core/video/video.h"
#include "pal/pal.h"

/* The GPU front end draws with *g_rp: the game's own g_r3d normally, a snapshot copy that the draw thread keeps (r3d_q.c) when
 * the draw queue is on. g_front is the game side (r3d_gpu_front_mode). */
r3d_state  g_r3d_w;
r3d_state *g_rp = &g_r3d;
static r3d_state *const g_front = &g_r3d;
#define g_r3d (*g_rp)

enum {
    RS_ZENABLE = 7, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_SRCBLEND = 19, RS_DESTBLEND = 20,
    RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_ALPHABLENDENABLE = 27,
    RS_FOGENABLE = 28, RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGTABLESTART = 36,
    RS_FOGTABLEEND = 37, RS_FOGTABLEDENSITY = 38, RS_COLORKEYENABLE = 41,
};
enum { TSS_COLOROP = 1, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG1, TSS_ALPHAARG2,
       TSS_ADDRESSU = 13, TSS_ADDRESSV, TSS_MAGFILTER = 16 };

/* One persistent GPU target per guest back-buffer address (the 3DS has two).
 * dirty means the target holds results not yet copied to guest memory;
 * needs_load means the guest bytes are newer than the target contents. */
typedef struct {
    int      used;
    gptr     addr;          /* guest memory this GPU target mirrors */
    uint32_t ver;
    int      dirty;
    int      needs_load;
    uint32_t use;           /* LRU stamp */
} gtarget;

static gtarget g_tg[PAL_GPU_MAX_TARGETS];
static int      g_cur = -1;     /* g_tg index currently selected for drawing */
static uint32_t g_use;
/* diagnostics for the 300-scene log when raster timing is enabled */
uint32_t g_r3d_gpu_presents, g_r3d_gpu_present_fail, g_r3d_gpu_loads, g_r3d_gpu_stores;
uint32_t g_r3d_gpu_batches, g_r3d_gpu_tex_uploads;   /* pal_gpu_draw calls, texture uploads */

static struct {
    int      active;
    uint32_t w, h;
    pal_gpu_vertex *vb;     /* pending batch: vb[0..npend) drawn with pst */
    uint32_t vcap, npend;
    pal_gpu_state pst;
    struct gvtx_s *tv;      /* transformed vertices of the current draw */
    uint32_t tcap;
    uint16_t *texbuf;
    uint32_t texcap;
    uint32_t pst_gen;       /* template generation of g.pst (batch compare, replaces a memcmp) */
    int      pst_ov;        /* colour-key alpha-test override of g.pst */
    int      warned_spec, warned_fvf;
} g;

int r3d_gpu_init(uint32_t w, uint32_t h)
{
    r3d_q_fence();
    if (g.active && g.w == w && g.h == h) return 1;
    r3d_gpu_flush();
    free(g.vb); free(g.tv); free(g.texbuf);
    memset(&g, 0, sizeof g);
    memset(g_tg, 0, sizeof g_tg);
    g_cur = -1;
    g_use = 0;
    if (!pal_gpu_init(w, h)) return 0;
    g.active = 1;
    g.w = w;
    g.h = h;
    pal_log("r3d: GPU renderer for %lux%lu", (unsigned long)w, (unsigned long)h);
    return 1;
}

int r3d_gpu_active(void) { return g.active; }

void r3d_gpu_flush(void)
{
    if (!g.npend) return;
    uint32_t prev = g_prof_sub;
    prof_sub(PROF_SUB_R3D_BATCH);
    g_r3d_gpu_batches++;
    pal_where("flush: pal_gpu_draw");
    pal_gpu_draw(&g.pst, g.vb, g.npend);
    prof_sub(prev);
    g.npend = 0;
}

static int target_ok(const dd_surface *s)
{
    return s && s->pf.bits == 16 && s->pf.g == 0x07e0u && s->w == g.w && s->h == g.h;
}

static gtarget *tg_find(gptr addr)
{
    for (int i = 0; i < PAL_GPU_MAX_TARGETS; i++)
        if (g_tg[i].used && g_tg[i].addr == addr) return &g_tg[i];
    return NULL;
}

static gtarget *tg_current(void) { return g_cur >= 0 ? &g_tg[g_cur] : NULL; }

/* Pick a target slot for a guest address, evicting the least recently used
 * one (storing its results first) when both are taken. */
static gtarget *tg_alloc(void)
{
    int n = pal_gpu_target_count();
    if (n <= 0) return NULL;
    gtarget *best = NULL;
    for (int i = 0; i < n; i++) {
        if (!g_tg[i].used) { best = &g_tg[i]; break; }
        if (!best || g_tg[i].use < best->use) best = &g_tg[i];
    }
    int idx = (int)(best - g_tg);
    if (best->used && best->dirty && !best->needs_load && best->addr) {
        uint8_t *p = gmem_span(best->addr, g.w * 2u * g.h, 1);
        if (p) { pal_gpu_target_store(idx, p, g.w * 2u); g_r3d_gpu_stores++; }
    }
    memset(best, 0, sizeof *best);
    best->used = 1;
    return best;
}

void r3d_gpu_sync(dd_surface *s)
{
    pal_where("sync: fence");
    r3d_q_fence();
    if (!g.active || !s) return;
    gtarget *t = tg_find(s->pixels);
    pal_where(t ? "sync: target found" : "sync: no target");
    if (!t) return;
    r3d_gpu_flush();
    pal_where("sync: flushed");
    if (!t->dirty) return;
    pal_where("sync: dirty, store");
    uint8_t *p = gmem_span(t->addr, s->pitch * s->h, 1);
    if (p) { pal_gpu_target_store((int)(t - g_tg), p, s->pitch); g_r3d_gpu_stores++; }
    t->dirty = 0;
}

/* Makes the GPU target mirror g_r3d.target. full_clear: the next operation
 * overwrites every pixel, so the guest content need not be loaded. */
static int bind_target(int full_clear)
{
    dd_surface *s = g_r3d.target;
    if (!target_ok(s)) return 0;
    gtarget *t = tg_find(s->pixels);
    if (!t) {
        t = tg_alloc();
        if (!t) return 0;
        t->addr = s->pixels;
        t->ver = s->version;
        t->needs_load = !full_clear;
    } else if (t->ver != s->version) {
        t->ver = s->version;
        t->needs_load = !full_clear;    /* the CPU wrote the surface: guest bytes win */
    }
    if (full_clear) t->needs_load = 0;
    int idx = (int)(t - g_tg);
    if (g_cur != idx) {
        r3d_gpu_flush();
        pal_gpu_target_select(idx);
        g_cur = idx;
    }
    t->use = ++g_use;
    if (!full_clear && t->needs_load) {
        r3d_gpu_flush();
        const uint8_t *p = gmem_span(t->addr, s->pitch * s->h, 0);
        if (!p) return 0;
        pal_gpu_target_load(idx, p, s->pitch);
        g_r3d_gpu_loads++;
        t->needs_load = 0;
    }
    return 1;
}

/* The guest surface pointers swapped; contents stay with their memory
 * addresses, so the target slots only adopt the new versions (no reload). */
void r3d_gpu_flip_w(uint32_t fpix, uint32_t fver, uint32_t bpix, uint32_t bver)
{
    dd_surface f, b;
    memset(&f, 0, sizeof f); memset(&b, 0, sizeof b);
    f.pixels = fpix; f.version = fver; b.pixels = bpix; b.version = bver;
    r3d_gpu_flip(&f, &b);
}

void r3d_gpu_flip(dd_surface *f, dd_surface *b)
{
    if (g_r3d_q_on && !r3d_q_on_worker()) {          /* in order with the queued draws, without waiting for them */
        r3d_q_flip(f ? f->pixels : 0, f ? f->version : 0, b ? b->pixels : 0, b ? b->version : 0);
        return;
    }
    if (!g.active) return;
    gtarget *t = f ? tg_find(f->pixels) : NULL;
    if (t) t->ver = f->version;
    t = b ? tg_find(b->pixels) : NULL;
    if (t) t->ver = b->version;
}

/* Present straight from the GPU target through staging. The guest copy stays
 * pending (dirty) and is made on the next CPU access or target switch. */
static int g_present_w;       /* running on the draw thread: no screenshot capture, no guest bookkeeping, no staging fallback */

/* game side, before the Flip swaps the surfaces: queue the present of b instead of waiting for the draw thread.
 * 1: queued and booked as presented (the caller does not need the CPU copy) */
int r3d_gpu_present_async(dd_surface *b)
{
    extern int pal_gpu_direct_ok(void);
    extern int pal_3ds_fake_clock;
    if (!g_r3d_q_on || !g.active || !b || !g_r3d_q_since_present || g_front->target != b || !pal_gpu_direct_ok()) return 0;
    if (video_direct_begin()) return 0;                /* a screenshot is due: the synchronous path copies the frame */
    r3d_q_present(b->pixels, b->version, b->pitch, b->h);
    if (pal_3ds_fake_clock) r3d_q_fence();            /* deterministic A/B runs: the frame counts (= the clock) before the game goes on */
    video_direct_done(400, 240, 0);
    return 1;
}

void r3d_gpu_present_w(uint32_t pix, uint32_t ver, uint32_t pitch, uint32_t h)
{
    dd_surface s;
    memset(&s, 0, sizeof s);
    s.pixels = pix; s.version = ver; s.pitch = pitch; s.h = h;
    g_present_w = 1;
    if (!r3d_gpu_present(&s)) { static int n; if (n++ < 20) pal_log("r3d: draw-thread present failed (surface %08lx, %d so far)", (unsigned long)pix, n); }
    g_present_w = 0;
}

int r3d_gpu_present(dd_surface *s)
{
    r3d_q_fence();
    if (!g.active || !s) return 0;
    gtarget *t = tg_find(s->pixels);
    if (!t) { g_r3d_gpu_present_fail++; return 0; }
    /* Guest bytes win before a present. A 2D blit writes guest memory and bumps the surface's
     * version; a target that is behind it (or was created for a draw that never covered it, which
     * is how the loading screen's second back buffer ends up black) must be brought up to date
     * or the transfer shows what the target happened to hold. */
    if (t->ver != s->version || t->needs_load) {
        const uint8_t *p = gmem_span(t->addr, s->pitch * s->h, 0);
        if (!p) { g_r3d_gpu_present_fail++; return 0; }
        r3d_gpu_flush();
        { int idx0 = (int)(t - g_tg);
          if (g_cur != idx0) { pal_gpu_target_select(idx0); g_cur = idx0; }
          pal_gpu_target_load(idx0, p, s->pitch);
          g_r3d_gpu_loads++; }
        t->ver = s->version; t->needs_load = 0;
        if (g_present_w) t->dirty = 1;     /* the draw thread cannot fall back to the CPU copy: the loaded target IS the frame to show */
    }
    if (!t->dirty) { g_r3d_gpu_present_fail++; return 0; }
    r3d_gpu_flush();
    int idx = (int)(t - g_tg);
    {
        uint16_t *cap = g_present_w ? NULL : video_direct_begin();
        if (pal_gpu_present_direct(idx, cap)) { if (!g_present_w) video_direct_done(400, 240, cap != NULL); g_r3d_gpu_presents++; return 1; }
    }
    if (g_present_w) return 0;   /* 800x480 on the 3DS: the transfer writes the screen */
    prof_sub(PROF_SUB_FLIP_STAGE);
    int staged = pal_gpu_target_stage(idx);
    prof_sub(PROF_SUB_NONE);
    if (!staged) { g_r3d_gpu_present_fail++; return 0; }
    video_present_staging(pal_gpu_staging(), g.w, g.h);
    g_r3d_gpu_presents++;
    return 1;
}

/* ---- textures ---- */
static uint32_t pow2(uint32_t v) { uint32_t p = 8; while (p < v) p <<= 1; return p; }

static int mask_shift(uint32_t m) { int s = 0; if (!m) return 0; while (!((m >> s) & 1u)) s++; return s; }
static int mask_bits(uint32_t m) { int s = mask_shift(m), b = 0; while ((m >> (s + b)) & 1u) b++; return b; }

/* Uploads the bound stage-0 texture when it changed; returns its id (0: none). */
static uint32_t texture(float *su, float *sv)
{
    dd_surface *s = g_r3d.tex[0];
    *su = *sv = 1.0f;
    if (!s || s->pf.bits != 16 || !s->w || !s->h || s->w > 1024 || s->h > 1024) return 0;
    uint8_t ck = g_r3d.rs[RS_COLORKEYENABLE] && s->has_ck;
    if (s->texture_id && s->gpu_tex_version == s->version && s->gpu_tex_ck == ck) {
        /* fast path: the same texture again. The uv scale depends only on the
         * texture size, so cache it instead of running pow2() twice per draw. */
        static dd_surface *c_s; static uint32_t c_ver; static float c_su, c_sv;
        if (s == c_s && s->version == c_ver) { *su = c_su; *sv = c_sv; }
        else {
            uint32_t pw = pow2(s->w), ph = pow2(s->h);
            c_su = *su = (float)s->w / (float)pw;
            c_sv = *sv = (float)s->h / (float)ph;
            c_s = s; c_ver = s->version;
        }
        return s->texture_id;
    }
    uint32_t pw = pow2(s->w), ph = pow2(s->h);
    *su = (float)s->w / (float)pw;
    *sv = (float)s->h / (float)ph;
    r3d_gpu_flush();                                       /* pending draws use the old content */
    prof_sub(PROF_SUB_R3D_TEX);                            /* CPU conversion + upload below */

    uint32_t am = (s->pf.flags & DDPF_ALPHAPIXELS) ? s->pf.a : 0;
    int abits = mask_bits(am);
    int fmt = abits >= 2 ? PAL_TEX_RGBA4444 : (abits == 1 || ck) ? PAL_TEX_RGBA5551 : PAL_TEX_RGB565;
    uint32_t m[4] = {s->pf.r, s->pf.g, s->pf.b, am};
    int sh[4], bits[4];
    for (int k = 0; k < 4; k++) { sh[k] = mask_shift(m[k]); bits[k] = mask_bits(m[k]); }
    static const int out_bits[3][4] = {{5, 6, 5, 0}, {5, 5, 5, 1}, {4, 4, 4, 4}};
    const int *ob = out_bits[fmt];

    if (pw * ph > g.texcap) {
        free(g.texbuf);
        g.texbuf = malloc((size_t)pw * ph * 2);
        g.texcap = g.texbuf ? pw * ph : 0;
        if (!g.texbuf) { prof_sub(PROF_SUB_R3D_STATE); return 0; }
    }
    const uint8_t *src = gmem_span(s->pixels, s->pitch * s->h, 0);
    if (!src) { prof_sub(PROF_SUB_R3D_STATE); return 0; }
    for (uint32_t y = 0; y < ph; y++) {
        const uint8_t *row = src + (y < s->h ? y : s->h - 1) * s->pitch;       /* pad by repeating edges */
        uint16_t *out = g.texbuf + y * pw;
        for (uint32_t x = 0; x < pw; x++) {
            uint16_t p;
            memcpy(&p, row + 2u * (x < s->w ? x : s->w - 1), 2);
            uint32_t o = 0;
            int pos = 16;
            for (int k = 0; k < 4; k++) {
                if (!ob[k]) continue;
                uint32_t v;
                if (k == 3 && !bits[3]) v = (ck && p >= s->ck_lo && p <= s->ck_hi) ? 0 : (1u << ob[k]) - 1;
                else {
                    v = (p & m[k]) >> sh[k];
                    if (bits[k] != ob[k]) v = bits[k] ? v * ((1u << ob[k]) - 1) / ((1u << bits[k]) - 1) : 0;
                    if (k == 3 && ck && p >= s->ck_lo && p <= s->ck_hi) v = 0;
                }
                pos -= ob[k];
                o |= (v & ((1u << ob[k]) - 1)) << pos;
            }
            out[x] = (uint16_t)o;
        }
    }
    if (s->texture_id && (s->gpu_tex_ck != ck)) { pal_gpu_tex_free(s->texture_id); s->texture_id = 0; }
    if (!s->texture_id) s->texture_id = pal_gpu_tex_create(pw, ph, fmt);
    if (!s->texture_id) { prof_sub(PROF_SUB_R3D_STATE); return 0; }
    pal_gpu_tex_upload(s->texture_id, g.texbuf);
    g_r3d_gpu_tex_uploads++;
    s->gpu_tex_version = s->version;
    s->gpu_tex_ck = ck;
    prof_sub(PROF_SUB_R3D_STATE);
    return s->texture_id;
}

void r3d_gpu_surface_freed(dd_surface *s)
{
    r3d_q_fence();
    gtarget *t = g.active ? tg_find(s->pixels) : NULL;
    if (s->texture_id || t) r3d_gpu_flush();
    if (s->texture_id) pal_gpu_tex_free(s->texture_id);
    s->texture_id = 0;
    if (t) {
        int idx = (int)(t - g_tg);
        t->used = 0; t->addr = 0; t->ver = 0; t->dirty = 0; t->needs_load = 0;
        if (g_cur == idx) g_cur = -1;
    }
}

/* ---- state ---- */
static void base_state(pal_gpu_state *st)
{
    memset(st, 0, sizeof *st);
    st->color_write = 1;
    r3d_viewport *vp = &g_r3d.vp;
    uint32_t x0 = vp->x, y0 = vp->y, x1 = vp->w ? vp->x + vp->w : g.w, y1 = vp->h ? vp->y + vp->h : g.h;
    st->clip_x0 = (uint16_t)(x0 < g.w ? x0 : g.w); st->clip_y0 = (uint16_t)(y0 < g.h ? y0 : g.h);
    st->clip_x1 = (uint16_t)(x1 < g.w ? x1 : g.w); st->clip_y1 = (uint16_t)(y1 < g.h ? y1 : g.h);
}

static uint8_t ta(uint32_t a) { return (a & 0xf) == 2 ? PAL_TA_TEXTURE : PAL_TA_DIFFUSE; }

static void build_state(pal_gpu_state *st)
{
    const uint32_t *rs = g_r3d.rs;
    base_state(st);
    const uint32_t *t0 = g_r3d.tss[0];
    st->linear = t0[TSS_MAGFILTER] == 2;
    st->clamp_u = t0[TSS_ADDRESSU] == 3;
    st->clamp_v = t0[TSS_ADDRESSV] == 3;
    st->color_op = (uint8_t)(t0[TSS_COLOROP] ? t0[TSS_COLOROP] : 4);
    st->alpha_op = (uint8_t)(t0[TSS_ALPHAOP] ? t0[TSS_ALPHAOP] : 2);
    st->color_a1 = ta(t0[TSS_COLORARG1]); st->color_a2 = ta(t0[TSS_COLORARG2]);
    st->alpha_a1 = ta(t0[TSS_ALPHAARG1]); st->alpha_a2 = ta(t0[TSS_ALPHAARG2]);
    st->blend = rs[RS_ALPHABLENDENABLE] != 0;
    st->src_blend = (uint8_t)(rs[RS_SRCBLEND] ? rs[RS_SRCBLEND] : 2);
    st->dst_blend = (uint8_t)(rs[RS_DESTBLEND] ? rs[RS_DESTBLEND] : 1);
    st->alpha_test = rs[RS_ALPHATESTENABLE] != 0;
    st->alpha_func = (uint8_t)(rs[RS_ALPHAFUNC] ? rs[RS_ALPHAFUNC] : 8);
    st->alpha_ref = (uint8_t)rs[RS_ALPHAREF];
    st->z_test = rs[RS_ZENABLE] && g_r3d.zbuf;
    st->z_write = st->z_test && rs[RS_ZWRITEENABLE];
    st->z_func = (uint8_t)(rs[RS_ZFUNC] ? rs[RS_ZFUNC] : 4);
    st->fog = rs[RS_FOGENABLE] != 0;
    st->fog_rgb = rs[RS_FOGCOLOR] & 0xffffffu;
    if (rs[RS_SPECULARENABLE] && !g.warned_spec) { g.warned_spec = 1; pal_log("r3d: GPU: specular add not implemented"); }
}

/* Everything except the texture is a function of the render/stage/viewport/target state, cached per generation. */
static void draw_state(pal_gpu_state *st, int textured, float *su, float *sv, uint32_t *gen_out, int *ov_out)
{
    static pal_gpu_state tmpl;
    static uint32_t tmpl_gen, tmpl_w, tmpl_h;
    static int tmpl_ok;
    prof_sub(PROF_SUB_R3D_STATE);
    uint32_t tex = textured ? texture(su, sv) : 0;
    prof_sub(PROF_SUB_R3D_STATE);
    if (!tmpl_ok || tmpl_gen != g_r3d.gen || tmpl_w != g.w || tmpl_h != g.h) {
        build_state(&tmpl);
        tmpl_gen = g_r3d.gen; tmpl_w = g.w; tmpl_h = g.h; tmpl_ok = 1;
    }
    *st = tmpl;
    st->tex = tex;
    int ov = 0;
    if (st->tex && g_r3d.tex[0]->gpu_tex_ck && !st->alpha_test) {   /* colour-keyed texels are discarded */
        st->alpha_test = 1; st->alpha_func = 5; st->alpha_ref = 0; ov = 1;
    }
    *gen_out = tmpl_gen;
    *ov_out = ov;
}

/* ---- vertices ---- */
static void mat_mul(const float *a, const float *b, float *o)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

typedef struct gvtx_s { pal_gpu_vertex v; float sx, sy; int ok; } gvtx;   /* sx/sy: screen position for culling */

/* Per-draw constants for the vertex fetch. */
typedef struct {
    uint32_t fvf, off_col;          /* off_col: byte offset of the diffuse colour */
    float    mvp[16];
    float    ax, bx, ay, by, minz, dz, fw, fh, ix2, iy2, inv_fr;
    float    su, sv;
    uint32_t fog_mode;
    float    fog_start, fog_end, fog_dens;
} fetch_ctx;

static float ldf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
static uint32_t ldu(const uint8_t *p) { uint32_t u; memcpy(&u, p, 4); return u; }

static void fetch_ctx_init(fetch_ctx *k, uint32_t fvf, float su, float sv)
{
    k->fvf = fvf; k->su = su; k->sv = sv;
    k->fw = (float)g.w; k->fh = (float)g.h;
    k->ix2 = 2.0f / k->fw; k->iy2 = 2.0f / k->fh;
    uint32_t off = (fvf & FVF_XYZRHW) ? 16 : 12;
    if (fvf & FVF_NORMAL) off += 12;
    if (fvf & 0x020u) off += 4;
    k->off_col = off;
    if (!(fvf & FVF_XYZRHW)) {
        /* world*view*proj changes rarely between draws: reuse the last product.
         * hle_d3d's SetTransform bumps g_r3d.xf_gen, which is a single compare -
         * cheaper than the three 64-byte memcmps this used to do per draw. */
        static struct { uint32_t gen; float mvp[16]; int ok; } cache;
        if (!cache.ok || cache.gen != g_r3d.xf_gen) {
            float wv[16];
            mat_mul(g_r3d.world, g_r3d.view, wv);
            mat_mul(wv, g_r3d.proj, cache.mvp);
            cache.gen = g_r3d.xf_gen;
            cache.ok = 1;
        }
        memcpy(k->mvp, cache.mvp, 64);
        /* viewport -> whole-target clip space */
        r3d_viewport *vp = &g_r3d.vp;
        float vw = vp->w ? (float)vp->w : k->fw, vh = vp->h ? (float)vp->h : k->fh;
        k->minz = vp->minz;
        k->dz = (vp->maxz > vp->minz ? vp->maxz : 1.0f) - vp->minz;
        k->ax = vw / k->fw; k->bx = ((float)vp->x * 2.0f + vw) / k->fw - 1.0f;
        k->ay = vh / k->fh; k->by = 1.0f - ((float)vp->y * 2.0f + vh) / k->fh;
    }
    k->fog_mode = g_r3d.rs[RS_FOGTABLEMODE];
    uint32_t a = g_r3d.rs[RS_FOGTABLESTART], b = g_r3d.rs[RS_FOGTABLEEND], dn = g_r3d.rs[RS_FOGTABLEDENSITY];
    memcpy(&k->fog_start, &a, 4); memcpy(&k->fog_end, &b, 4); memcpy(&k->fog_dens, &dn, 4);
    k->inv_fr = k->fog_end > k->fog_start ? 1.0f / (k->fog_end - k->fog_start) : 0.0f;
}

static float fog_factor(const fetch_ctx *k, float spec_a, float depth)
{
    if (!k->fog_mode) return spec_a;
    float f;
    if (k->fog_mode == 3) f = k->fog_end > k->fog_start ? (k->fog_end - depth) * k->inv_fr : 1.0f;
    else { float e = depth * k->fog_dens; f = expf(-(k->fog_mode == 2 ? e * e : e)); }
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static inline __attribute__((always_inline)) void fetch_f(const fetch_ctx *k, const uint8_t *q, gvtx *o, uint32_t fvf)
{
    pal_gpu_vertex *v = &o->v;
    float W;
    if (fvf & FVF_XYZRHW) {
        float x = ldf(q), y = ldf(q + 4), z = ldf(q + 8), rhw = ldf(q + 12);
        W = rhw > 0 ? 1.0f / rhw : 1.0f;
        o->sx = x; o->sy = y; o->ok = 1;
        /* D3D samples pixel centres at integer coordinates: shift by half a pixel */
        float nx = (x + 0.5f) * k->ix2 - 1.0f, ny = 1.0f - (y + 0.5f) * k->iy2;
        v->x = nx * W; v->y = ny * W; v->z = (z < 0 ? 0 : z > 1 ? 1 : z) * W; v->w = W;
    } else {
        const float *m = k->mvp;
        float x = ldf(q), y = ldf(q + 4), z = ldf(q + 8);
        float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
        float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        float cz = x * m[2] + y * m[6] + z * m[10] + m[14];
        W = x * m[3] + y * m[7] + z * m[11] + m[15];
        v->x = cx * k->ax + W * k->bx; v->y = cy * k->ay + W * k->by;
        v->z = k->minz * W + cz * k->dz; v->w = W;
        o->ok = W > 1e-6f;
        if (o->ok) { float iw = 1.0f / W; o->sx = (v->x * iw + 1.0f) * 0.5f * k->fw; o->sy = (1.0f - v->y * iw) * 0.5f * k->fh; }
    }
    q += k->off_col;
    uint32_t diff = 0xffffffffu, spec = 0xff000000u;
    if (fvf & FVF_DIFFUSE) { diff = ldu(q); q += 4; }
    if (fvf & FVF_SPECULAR) { spec = ldu(q); q += 4; }
    v->rgba[0] = (uint8_t)(diff >> 16); v->rgba[1] = (uint8_t)(diff >> 8);
    v->rgba[2] = (uint8_t)diff;         v->rgba[3] = (uint8_t)(diff >> 24);
    v->fog = fog_factor(k, (float)(spec >> 24) / 255.0f, W);
    if (FVF_TEXCOUNT(fvf)) { v->u = ldf(q) * k->su; v->v = ldf(q + 4) * k->sv; }
    else v->u = v->v = 0;
}

static void fetch(const fetch_ctx *k, const uint8_t *q, gvtx *o) { fetch_f(k, q, o, k->fvf); }


static inline int culled(const gvtx *a, const gvtx *b, const gvtx *c, uint32_t cull)
{
    if (!a->ok || !b->ok || !c->ok) return 0;           /* crosses w = 0: the GPU clips it */
    float area = (b->sx - a->sx) * (c->sy - a->sy) - (c->sx - a->sx) * (b->sy - a->sy);
    return area == 0 || (cull == 2 && area < 0) || (cull == 3 && area > 0);
}

/* diagnostic: per-FVF draw histogram, logged once a second from the input tick */
static struct { uint32_t fvf, n, verts; } dbg_fvf[16];
static int dbg_nfvf;
void r3d_gpu_dbg_fvf_log(void)
{
    int i;
    for (i = 0; i < dbg_nfvf; i++)
        pal_log("draws: fvf %lx x%lu (verts %lu)", (unsigned long)dbg_fvf[i].fvf,
                (unsigned long)dbg_fvf[i].n, (unsigned long)dbg_fvf[i].verts);
    dbg_nfvf = 0;
}

static int draw_core(uint32_t prim, uint32_t fvf, gptr verts, uint32_t nverts, gptr indices, uint32_t nindices,
                     const uint8_t *vsrc_h, const uint8_t *isrc_h)
{
    const int indexed = isrc_h != NULL || indices != 0;
    {   int i_;
        for (i_ = 0; i_ < dbg_nfvf; i_++) if (dbg_fvf[i_].fvf == fvf) { dbg_fvf[i_].n++; dbg_fvf[i_].verts += nverts; break; }
        if (i_ == dbg_nfvf && dbg_nfvf < 16) { dbg_fvf[dbg_nfvf].fvf = fvf; dbg_fvf[dbg_nfvf].n = 1; dbg_fvf[dbg_nfvf].verts = nverts; dbg_nfvf++; }
    }
    if (!g.active || !target_ok(g_r3d.target)) return 0;
    if (prim < PT_TRIANGLELIST) return 1;                 /* points/lines: not drawn (as the software path) */
    if (!(fvf & (FVF_XYZRHW | FVF_XYZ))) {
        if (!g.warned_fvf) { g.warned_fvf = 1; pal_log("r3d: GPU: fvf %03lx not supported", (unsigned long)fvf); }
        return 0;
    }
    uint32_t n = indexed ? nindices : nverts, ntri = prim == PT_TRIANGLELIST ? n / 3 : n >= 3 ? n - 2 : 0;
    if (!ntri) return 1;
    prof_sub(PROF_SUB_R3D_TARGET);
    if (!bind_target(0)) { prof_sub(PROF_SUB_NONE); return 0; }
    pal_gpu_state st;
    float su = 1.0f, sv = 1.0f;
    uint32_t dgen = 0;
    int dov = 0;
    draw_state(&st, FVF_TEXCOUNT(fvf) != 0, &su, &sv, &dgen, &dov);
    prof_sub(PROF_SUB_R3D_BATCH);
    /* The built state is a pure function of (render-state generation, texture id,
     * colour-key override), so compare those instead of memcmp'ing the struct. */
    if (g.npend && (g.pst_gen != dgen || g.pst.tex != st.tex || g.pst_ov != dov || g.npend > 24576)) r3d_gpu_flush();
    if (g.npend + ntri * 3 > g.vcap) {
        uint32_t cap = (g.npend + ntri * 3) * 2 + 256;
        pal_gpu_vertex *nb = realloc(g.vb, cap * sizeof *nb);
        if (!nb) { r3d_gpu_flush(); prof_sub(PROF_SUB_NONE); return 1; }
        g.vb = nb;
        g.vcap = cap;
    }
    g.pst = st; g.pst_gen = dgen; g.pst_ov = dov;
    /* Each vertex is transformed once (indexed meshes share them between
     * triangles), straight from one host view of the guest vertex buffer. */
    uint32_t stride = r3d_fvf_stride(fvf), out = g.npend, first = g.npend;
    const uint8_t *vsrc = vsrc_h ? vsrc_h : gmem_span(verts, nverts * stride, 0);
    const uint8_t *isrc = isrc_h ? isrc_h : indices ? gmem_span(indices, nindices * 2u, 0) : NULL;
    if (!vsrc || (indexed && !isrc)) { prof_sub(PROF_SUB_NONE); return 1; }
    if (nverts > g.tcap) {
        gvtx *nt = realloc(g.tv, (nverts + 64) * sizeof *nt);
        if (!nt) { prof_sub(PROF_SUB_NONE); return 1; }
        g.tv = nt;
        g.tcap = nverts + 64;
    }
    fetch_ctx k;
    prof_sub(PROF_SUB_R3D_MATRIX);
    fetch_ctx_init(&k, fvf, su, sv);
    prof_sub(PROF_SUB_R3D_FETCH);
    if (fvf == 0x1c4u) for (uint32_t i = 0; i < nverts; i++) fetch_f(&k, vsrc + i * 32u, &g.tv[i], 0x1c4u);   /* the scene's usual format */
    else for (uint32_t i = 0; i < nverts; i++) fetch(&k, vsrc + i * stride, &g.tv[i]);
    prof_sub(PROF_SUB_R3D_CULL);
    const uint32_t cull_mode = g_r3d.rs[RS_CULLMODE];   /* constant for this draw */
    for (uint32_t i = 0; i + 2 < n; ) {
        uint32_t ia, ib, ic;
        if (prim == PT_TRIANGLELIST) { ia = i; ib = i + 1; ic = i + 2; i += 3; }
        else if (prim == PT_TRIANGLESTRIP) { ia = i; ib = i + 1; ic = i + 2; if (i & 1) { ia = i + 1; ib = i; } i += 1; }
        else { ia = 0; ib = i + 1; ic = i + 2; i += 1; }
        if (isrc) {
            uint16_t t;
            memcpy(&t, isrc + 2u * ia, 2); ia = t;
            memcpy(&t, isrc + 2u * ib, 2); ib = t;
            memcpy(&t, isrc + 2u * ic, 2); ic = t;
        }
        if (ia >= nverts || ib >= nverts || ic >= nverts) break;
        const gvtx *a = &g.tv[ia], *b = &g.tv[ib], *c = &g.tv[ic];
        if (culled(a, b, c, cull_mode)) continue;
        g.vb[out++] = a->v; g.vb[out++] = b->v; g.vb[out++] = c->v;
    }
    g_r3d.tris_drawn += (out - first) / 3;
    g.npend = out;
    if (out) {
        gtarget *t = tg_current();
        if (t) t->dirty = 1;
    }
    prof_sub(PROF_SUB_NONE);
    return 1;
}

int r3d_gpu_draw(uint32_t prim, uint32_t fvf, gptr verts, uint32_t nverts, gptr indices, uint32_t nindices)
{
    return draw_core(prim, fvf, verts, nverts, indices, nindices, NULL, NULL);
}

/* draw thread: the input was copied into the queue */
void r3d_gpu_draw_h(uint32_t prim, uint32_t fvf, const uint8_t *vsrc, uint32_t nverts, const uint8_t *isrc, uint32_t nindices)
{
    draw_core(prim, fvf, 0, nverts, 0, nindices, vsrc, isrc);
}

/* game side: 0 = the software path must draw, 1 = queue it, 2 = nothing to draw */
int r3d_gpu_front_mode(uint32_t prim, uint32_t fvf)
{
    if (!g.active || !target_ok(g_front->target)) return 0;
    if (prim < PT_TRIANGLELIST) return 2;
    if (!(fvf & (FVF_XYZRHW | FVF_XYZ))) {
        if (!g.warned_fvf) { g.warned_fvf = 1; pal_log("r3d: GPU: fvf %03lx not supported", (unsigned long)fvf); }
        return 0;
    }
    return 1;
}

static int clear_core(uint32_t nrects, gptr rects, const uint32_t *rc, uint32_t flags, uint32_t color, float z);
int r3d_gpu_clear(uint32_t nrects, gptr rects, uint32_t flags, uint32_t color, float z)
{
    return clear_core(nrects, rects, NULL, flags, color, z);
}

int r3d_gpu_clear_h(uint32_t nrects, const uint32_t *rects4, uint32_t flags, uint32_t color, float z)
{
    return clear_core(nrects, rects4 ? 1u : 0u, rects4, flags, color, z);
}

static int clear_core(uint32_t nrects, gptr rects, const uint32_t *rc, uint32_t flags, uint32_t color, float z)
{
#define RC(r, k) (rc ? rc[4u * (r) + (k)] : gmem_rd32(rects + 16u * (r) + 4u * (k)))
    if (!g.active || !target_ok(g_r3d.target) || !(flags & 3u)) return 0;
    int full = 0;
    if (flags & 1u) {
        if (!nrects || !rects) full = 1;
        else for (uint32_t r = 0; r < nrects; r++)
            if ((int32_t)RC(r, 0) <= 0 && (int32_t)RC(r, 1) <= 0 && RC(r, 2) >= g.w && RC(r, 3) >= g.h) full = 1;
    }
    if (!bind_target(full)) return 0;
    r3d_gpu_flush();
    pal_gpu_state st;
    base_state(&st);
    st.color_op = st.alpha_op = 3;                        /* SELECTARG2 = diffuse */
    st.color_a2 = st.alpha_a2 = PAL_TA_DIFFUSE;
    st.alpha_func = 8;
    st.color_write = (flags & 1u) != 0;
    st.z_test = (flags & 2u) != 0;
    st.z_write = st.z_test;
    st.z_func = 8;                                        /* ALWAYS */
    pal_gpu_vertex q[6];
    float zc = z < 0 ? 0 : z > 1 ? 1 : z;
    for (uint32_t r = 0; r < (nrects ? nrects : 1); r++) {
        float x0 = 0, y0 = 0, x1 = (float)g.w, y1 = (float)g.h;
        if (nrects && rects) {
            x0 = (float)(int32_t)RC(r, 0); y0 = (float)(int32_t)RC(r, 1);
            x1 = (float)(int32_t)RC(r, 2); y1 = (float)(int32_t)RC(r, 3);
        }
        float X0 = x0 * 2.0f / (float)g.w - 1.0f, X1 = x1 * 2.0f / (float)g.w - 1.0f;
        float Y0 = 1.0f - y0 * 2.0f / (float)g.h, Y1 = 1.0f - y1 * 2.0f / (float)g.h;
        const float xy[6][2] = {{X0, Y0}, {X1, Y0}, {X0, Y1}, {X1, Y0}, {X1, Y1}, {X0, Y1}};
        for (int k = 0; k < 6; k++) {
            q[k].x = xy[k][0]; q[k].y = xy[k][1]; q[k].z = zc; q[k].w = 1.0f;
            q[k].u = q[k].v = 0; q[k].fog = 1.0f;
            q[k].rgba[0] = (uint8_t)(color >> 16); q[k].rgba[1] = (uint8_t)(color >> 8);
            q[k].rgba[2] = (uint8_t)color; q[k].rgba[3] = (uint8_t)(color >> 24);
        }
        st.clip_x0 = 0; st.clip_y0 = 0; st.clip_x1 = (uint16_t)g.w; st.clip_y1 = (uint16_t)g.h;
        prof_sub(PROF_SUB_R3D_BATCH);
        g_r3d_gpu_batches++;
        pal_gpu_draw(&st, q, 6);
        prof_sub(PROF_SUB_NONE);
    }
    gtarget *ct = tg_current();
    if (ct) ct->dirty = 1;
    return 1;
#undef RC
}
