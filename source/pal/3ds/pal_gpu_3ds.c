/* 3DS GPU backend (citro3d) for the core's Direct3D front end (core/render/r3d_gpu.c).
 *
 * One offscreen render target (RGB565 + 16-bit depth in VRAM) of the game's
 * resolution. Vertices are copied into a linear-memory ring that is reset when
 * a new GPU frame starts (C3D_FrameBegin waits for the previous one). Texture
 * memory the GPU may still read in the current frame is never overwritten: a
 * re-upload then gets a fresh C3D_Tex and the old one is freed next frame.
 * Fog: per-vertex factor in texcoord1.s, looked up in a 64x8 A8 ramp on unit 1
 * and blended over the stage-0 result in TEV stage 1. */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_vsh_shbin.h"
#include "core/prof.h"
#include "pal/pal.h"

/* Stall diagnostics (guest.c stall monitor): where the GPU path last blocked. */
static volatile const char *g_wh_sdt, *g_wh_main;
static volatile int g_sdt_busy;
#define C3D_SyncDisplayTransfer(...) (g_wh_sdt = __func__, g_sdt_busy++, C3D_SyncDisplayTransfer(__VA_ARGS__), g_sdt_busy--)
void pal_gpu_where(char *buf, int n)
{
    snprintf(buf, (size_t)n, "gpu: sdt=%s busy=%d main=%s", g_wh_sdt ? (const char *)g_wh_sdt : "-", g_sdt_busy, g_wh_main ? (const char *)g_wh_main : "-");
}

extern void *pal_3ds_top_fb(void);
extern void pal_3ds_top_swap(void);
extern void pal_3ds_vsync_guard(void);

#define VB_BYTES   (2u << 20)
#define MAX_TEX    2048

typedef struct { C3D_Tex t; int used; uint32_t w, h; int fmt; uint32_t frame; } gtex;

static struct {
    int ok;
    uint32_t w, h;
    C3D_RenderTarget *rt[PAL_GPU_MAX_TARGETS];
    int n_slots, cur_slot;
    int staged_valid, staged_slot;
    DVLB_s *dvlb;
    shaderProgram_s prog;
    int u_cx, u_cy, u_cz, u_cw;
    int rot;                         /* 800x480: targets are stored rotated (480x800) so the transfer writes the screen directly */
    uint32_t rw, rh;                 /* target dimensions in memory */
    pal_gpu_vertex *vb;
    uint32_t vpos;
    int in_frame;
    uint32_t frame;
    uint8_t *staging;
    uint8_t *disp;                   /* (w/2)x(h/2) packed copy made by the display transfer's 2x2 downscale */
    gtex tex[MAX_TEX];
    C3D_Tex dead[64];
    int ndead;
    C3D_Tex fog;
    /* last state applied to citro3d, so repeated draws skip unchanged setters */
    int           cs_valid;
    pal_gpu_state cs;
    uint32_t      tex_vis;          /* texture id bound at unit 0 (0: none) */
    int           tex_filter_valid; /* filter/wrap of tex_vis already applied */
    /* diagnostics: vertex cache maintenance, one flush per submitted frame */
    uint32_t      flush_calls;      /* GSPGPU_FlushDataCache calls in end_frame */
    uint32_t      flush_bytes;      /* bytes covered by those calls */
    uint32_t      frame_ends;       /* C3D_FrameEnd submissions */
    float         cmd_usage_max;    /* peak citro3d command-buffer use (fraction) */
} g;

/* Orientation (verified on the menu): clip y is negated, so window row 0 is the
 * top row of the game's frame; DisplayTransfer's linear output runs bottom-up
 * (row 0 = window row h-1), hence the row flip in load/store. Texture rows are
 * stored top-down (tile row 0 first) and sampled at t = 1 - v; checked by
 * gpu_selftest.c. */
#define CONV_Y (-1.0f)
#define ROT_SX (1.0f)
#define ROT_SY (-1.0f)

static void morton_put16(uint16_t *dst, uint32_t w, uint32_t x, uint32_t y, uint16_t v)
{
    uint32_t tile = (y >> 3) * (w >> 3) + (x >> 3);
    uint32_t xi = x & 7, yi = y & 7;
    uint32_t m = (xi & 1) | ((yi & 1) << 1) | ((xi & 2) << 1) | ((yi & 2) << 2) | ((xi & 4) << 2) | ((yi & 4) << 3);
    dst[tile * 64 + m] = v;
}

static void free_dead(void)
{
    for (int i = 0; i < g.ndead; i++) C3D_TexDelete(&g.dead[i]);
    g.ndead = 0;
}

#define XFER(tiled) (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(tiled) | GX_TRANSFER_RAW_COPY(0) | \
                     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
                     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* ---- asynchronous present ----
 * The 800x480 -> 400x240 display transfer blocks ~3.3 ms per frame (constant, it is the transfer itself). A worker on core 2 runs
 * that blocking C3D_SyncDisplayTransfer plus the stream re-alignment while the main thread already runs the next frame's game
 * logic; the main thread joins it (pal_gpu_present_finish) before it touches citro3d, a transfer or the top framebuffer again,
 * and does the buffer swap itself. The picture appears when the next frame's first draw starts (about the logic's duration after
 * the Flip) instead of right at the Flip: ~13 ms later than before, ~3.5 ms of CPU time saved per frame. Without a core 2 thread
 * (Old 3DS) or for screenshots the original synchronous path runs. */
int g_xf_off = 0;                             /* on by default (needs core 2; noasync.txt in main.c turns it off) */
static struct {
    Thread th; LightEvent job, done;
    volatile int quit, pending;
    int ok, slot;
} xf;
static void screen_nudge(uint16_t *fb, int dx, int dy);
extern int g_scr_dx, g_scr_dy;
/* Debug (fbdump.txt = presentation number): the 8 frames the display really shows from that presentation on, as fbd_N.bmp
 * next to the log. Taken from the top framebuffer after the transfer, i.e. on the path every present takes. */
int g_fbdump_start;
static uint16_t *g_fbd; static int g_fbd_n; static uint32_t g_fbd_cnt;
static void fbd_write(void)
{
    for (int k = 0; k < 8; k++) {
        char path[96]; snprintf(path, sizeof path, "sdmc:/3ds/tpm/fbd_%d.bmp", k);
        FILE *f = fopen(path, "wb"); if (!f) continue;
        uint8_t hdr[54] = {'B', 'M'};
        uint32_t row = 400 * 3, size = 54 + row * 240;
        hdr[2] = (uint8_t)size; hdr[3] = (uint8_t)(size >> 8); hdr[4] = (uint8_t)(size >> 16); hdr[10] = 54; hdr[14] = 40;
        hdr[18] = 400 & 255; hdr[19] = 400 >> 8; hdr[22] = 240; hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        static uint8_t line[400 * 3];
        for (int y = 239; y >= 0; y--) {
            for (int x = 0; x < 400; x++) {
                uint16_t v = g_fbd[(size_t)k * 400 * 240 + (size_t)y * 400 + (size_t)x];
                line[3 * x] = (uint8_t)((v & 0x1f) << 3); line[3 * x + 1] = (uint8_t)(((v >> 5) & 0x3f) << 2); line[3 * x + 2] = (uint8_t)((v >> 11) << 3);
            }
            fwrite(line, 1, sizeof line, f);
        }
        fclose(f);
    }
    pal_log("fbdump: 8 frames written");
}
static void xf_work(int slot)
{
    C3D_SyncDisplayTransfer((u32 *)g.rt[slot]->frameBuf.colorBuf, GX_BUFFER_DIM(g.rw, g.rh), (u32 *)pal_3ds_top_fb(),
                            GX_BUFFER_DIM(g.rw, g.rh),
                            (XFER(0) & ~GX_TRANSFER_SCALING(3)) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY));
    uint16_t *fb = (uint16_t *)pal_3ds_top_fb();
    GSPGPU_InvalidateDataCache((void *)fb, 400 * 240 * 2);    /* the transfer wrote it behind the CPU cache */
    uint32_t *w = (uint32_t *)fb;                              /* re-align the column stream by one element (see pal_gpu_present_direct) */
    for (uint32_t i = 400u * 240u / 2u - 1u; i > 0; i--) w[i] = (w[i - 1u] >> 16) | (w[i] << 16);
    w[0] <<= 16;
    if (g_scr_dx || g_scr_dy) screen_nudge(fb, g_scr_dx, g_scr_dy);
    if (g_fbdump_start && ++g_fbd_cnt >= (uint32_t)g_fbdump_start && g_fbd_n < 8) {
        if (!g_fbd) g_fbd = malloc(8u * 400u * 240u * 2u);
        if (g_fbd) {
            uint16_t *d = g_fbd + (size_t)g_fbd_n * 400 * 240;
            for (uint32_t y = 0; y < 240; y++) for (uint32_t x = 0; x < 400; x++) d[y * 400 + x] = fb[x * 240 + (239 - y)];
            if (++g_fbd_n == 8) fbd_write();
        }
    }
}
static void xf_thread(void *arg)
{
    (void)arg;
    for (;;) {
        LightEvent_Wait(&xf.job);
        if (xf.quit) break;
        xf_work(xf.slot);
        LightEvent_Signal(&xf.done);
    }
}
static void xf_init(void)
{
    LightEvent_Init(&xf.job, RESET_ONESHOT);
    LightEvent_Init(&xf.done, RESET_ONESHOT);
    xf.th = threadCreate(xf_thread, NULL, 16 * 1024, 0x18, 2, false);   /* core 2: New 3DS only */
    xf.ok = xf.th != NULL;
    pal_log("gpu: async present %s", xf.ok ? "on (worker on core 2)" : "off (no core 2)");
}
static uint64_t xf_wait_ticks; static uint32_t xf_frames;
static LightLock xf_lock = 1;     /* the draw thread (begin_frame) and the game thread (video_blit) may both finish a present */
/* The lock only matters when the draw thread (opt-in) and the game thread can both finish a present. A console run froze for minutes
 * right here with the lock apparently never released (diagnosis: bench/console log), so the wait is bounded: after 300 ms the lock is
 * taken to be orphaned, logged with its state, and the present is finished without it (the game thread is the only user by default). */
static volatile uint32_t g_xf_holder;
void pal_gpu_present_finish(void)
{
    if (!xf.pending) return;
    int have = 0;
    for (int i = 0; i < 3000 && !have; i++) {
        if (LightLock_TryLock(&xf_lock) == 0) have = 1;
        else svcSleepThread(100000ll);
    }
    if (!have) {
        static int warned;
        if (warned++ < 3) { pal_log("present_finish: xf_lock stuck (value %d, pending %d, holder tid %lu, me %lu), continuing without it", (int)xf_lock, (int)xf.pending, (unsigned long)g_xf_holder, (unsigned long)(uintptr_t)threadGetCurrent()); pal_log_flush(); }
    } else { Handle th = 0; (void)th; g_xf_holder = (uint32_t)(uintptr_t)threadGetCurrent(); }
    pal_where("present_finish: locked");
    if (!xf.pending) { if (have) { g_xf_holder = 0; LightLock_Unlock(&xf_lock); } return; }
    uint64_t t0 = svcGetSystemTick();
    g_wh_main = "present_finish: wait for the async transfer";
    LightEvent_Wait(&xf.done);
    g_wh_main = "present_finish: done";
    xf_wait_ticks += svcGetSystemTick() - t0;
    xf.pending = 0;
    pal_3ds_top_swap();
    if (++xf_frames % 300 == 0) {
        pal_log("present: async, main thread waited %lu us per frame for the transfer", (unsigned long)(xf_wait_ticks * 1000 / CPU_TICKS_PER_MSEC / 300));
        xf_wait_ticks = 0;
    }
    if (have) { g_xf_holder = 0; LightLock_Unlock(&xf_lock); }
}

/* vertices after which a part of the frame is handed to the GPU (0 disables: one submit per frame as before) */
static uint32_t g_split_verts = 0xffffffffu;      /* default off (unverified on hardware): split.txt turns it on */
void pal_gpu_set_split(uint32_t v) { g_split_verts = v ? v : 0xffffffffu; }

static void begin_frame(void);
static void end_frame(void)
{
    if (!g.in_frame) return;
    /* One cache flush for the whole vertex ring this frame. citro3d appends
     * commands with GPUCMD_Add (which panics instead of submitting when the
     * buffer is full); the command list reaches the GPU only from here, or from
     * C3D_SyncDisplayTransfer while a frame is open, which this backend never
     * does. So every vertex the GPU can read is final at this point. */
    if (g.vpos) {
        uint32_t prev = g_prof_sub;
        prof_sub(PROF_SUB_PAL_CACHE);
        GSPGPU_FlushDataCache(g.vb, g.vpos * sizeof *g.vb);
        prof_sub(prev);
        g.flush_calls++;
        g.flush_bytes += g.vpos * sizeof *g.vb;
    }
    g_wh_main = "end_frame: FrameEnd";
    C3D_FrameEnd(0);
    g_wh_main = "end_frame: done";
    g.frame_ends++;
    float usage = C3D_GetCmdBufUsage();
    if (usage > g.cmd_usage_max) g.cmd_usage_max = usage;
    g.in_frame = 0;
}

/* Before the app is suspended (HOME): submit the open frame so the GPU queue is idle when the system takes the GPU. */
void pal_gpu_quiesce(void) { if (g.ok) { pal_gpu_present_finish(); end_frame(); } }

static void begin_frame(void)
{
    if (g.in_frame) return;
    pal_gpu_present_finish();
    g_wh_main = "begin_frame: FrameBegin";
    C3D_FrameBegin(0);                       /* waits until the GPU finished the previous frame */
    g_wh_main = "begin_frame: done";
    free_dead();
    C3D_FrameDrawOn(g.rt[g.cur_slot]);
    g.vpos = 0;
    g.frame++;
    g.in_frame = 1;
    if (g.frame % 300 == 0 && g.flush_calls)
        pal_log("gpu: %lu frames: %lu vertex flushes (%lu bytes, %.2f/frame), %lu submits, cmd buf peak %.1f%%",
                (unsigned long)g.frame, (unsigned long)g.flush_calls, (unsigned long)g.flush_bytes,
                (double)g.flush_calls / g.frame, (unsigned long)g.frame_ends, g.cmd_usage_max * 100.0f);
    C3D_BindProgram(&g.prog);
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);            /* position */
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);            /* texcoord 0 */
    AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);    /* colour */
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 1);            /* fog */
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, g.vb, sizeof(pal_gpu_vertex), 4, 0x2310);   /* memory order: pos, tc, fog, rgba */
    if (g.rot) {                              /* target x <- game y, target y <- game x (reversed) */
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g.u_cx, 0.0f, ROT_SX, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g.u_cy, ROT_SY, 0.0f, 0.0f, 0.0f);
    } else {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g.u_cx, 1.0f, 0.0f, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g.u_cy, 0.0f, CONV_Y, 0.0f, 0.0f);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.u_cz, 0.0f, 0.0f, -1.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.u_cw, 0.0f, 0.0f, 0.0f, 1.0f);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthMap(true, -1.0f, 0.0f);
}

int pal_gpu_init(uint32_t w, uint32_t h)
{
    if (g.ok) return g.w == w && g.h == h;
    if (w % 8 || h % 8 || w > 1024 || h > 1024) { pal_log("gpu: %lux%lu target not possible", (unsigned long)w, (unsigned long)h); return 0; }
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) { pal_log("gpu: C3D_Init failed"); return 0; }
    g.rot = (w == 800 && h == 480);
    g.rw = g.rot ? h : w;
    g.rh = g.rot ? w : h;
    for (g.n_slots = 0; g.n_slots < PAL_GPU_MAX_TARGETS; g.n_slots++) {
        g.rt[g.n_slots] = C3D_RenderTargetCreate((int)g.rw, (int)g.rh, GPU_RB_RGB565, GPU_RB_DEPTH16);
        if (!g.rt[g.n_slots]) break;
    }
    g.vb = linearAlloc(VB_BYTES);
    g.staging = linearAlloc(w * h * 2);
    g.disp = linearAlloc(w * h * 2);   /* rotated-target transfers */
    if (!g.n_slots || !g.vb || !g.staging) { pal_log("gpu: out of VRAM/linear memory"); return 0; }
    g.dvlb = DVLB_ParseFile((u32 *)gpu_vsh_shbin, gpu_vsh_shbin_size);
    shaderProgramInit(&g.prog);
    shaderProgramSetVsh(&g.prog, &g.dvlb->DVLE[0]);
    g.u_cx = shaderInstanceGetUniformLocation(g.prog.vertexShader, "cx");
    g.u_cy = shaderInstanceGetUniformLocation(g.prog.vertexShader, "cy");
    g.u_cz = shaderInstanceGetUniformLocation(g.prog.vertexShader, "cz");
    g.u_cw = shaderInstanceGetUniformLocation(g.prog.vertexShader, "cw");
    /* fog ramp: alpha = s */
    if (!C3D_TexInit(&g.fog, 64, 8, GPU_A8)) { pal_log("gpu: fog texture failed"); return 0; }
    uint8_t *f = g.fog.data;
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint32_t tile = (y >> 3) * 8 + (x >> 3), xi = x & 7, yi = y & 7;
            uint32_t m = (xi & 1) | ((yi & 1) << 1) | ((xi & 2) << 1) | ((yi & 2) << 2) | ((xi & 4) << 2) | ((yi & 4) << 3);
            f[tile * 64 + m] = (uint8_t)(x * 255 / 63);
        }
    C3D_TexFlush(&g.fog);
    C3D_TexSetFilter(&g.fog, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g.fog, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    for (int i = 2; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));   /* unused stages: set once */
    g.w = w;
    g.h = h;
    g.ok = 1;
    xf_init();
    pal_log("gpu: citro3d %d target(s) %lux%lu, linear free %lu KB", g.n_slots, (unsigned long)w, (unsigned long)h,
            (unsigned long)(linearSpaceFree() >> 10));
    return 1;
}

/* ---- textures ---- */
uint32_t pal_gpu_tex_create(uint32_t w, uint32_t h, int fmt)
{
    for (int i = 0; i < MAX_TEX; i++) {
        gtex *t = &g.tex[i];
        if (t->used) continue;
        static const GPU_TEXCOLOR f[3] = {GPU_RGB565, GPU_RGBA5551, GPU_RGBA4};
        if (!C3D_TexInit(&t->t, (u16)w, (u16)h, f[fmt])) {
            pal_log("gpu: texture %lux%lu: no linear memory (%lu KB free)", (unsigned long)w, (unsigned long)h,
                    (unsigned long)(linearSpaceFree() >> 10));
            return 0;
        }
        t->used = 1; t->w = w; t->h = h; t->fmt = fmt; t->frame = 0;
        return (uint32_t)i + 1;
    }
    pal_log("gpu: texture table full");
    return 0;
}

void pal_gpu_tex_upload(uint32_t id, const uint16_t *texels)
{
    if (!id || id > MAX_TEX || !g.tex[id - 1].used) return;
    gtex *t = &g.tex[id - 1];
    if (t->frame == g.frame) {                           /* the GPU may still read it (frame open or already submitted by a split): replace the memory */
        if (g.ndead == (int)(sizeof g.dead / sizeof g.dead[0])) { end_frame(); begin_frame(); }
        g.dead[g.ndead++] = t->t;
        static const GPU_TEXCOLOR f[3] = {GPU_RGB565, GPU_RGBA5551, GPU_RGBA4};
        if (!C3D_TexInit(&t->t, (u16)t->w, (u16)t->h, f[t->fmt])) { t->used = 0; return; }
        if (id == g.tex_vis) g.tex_filter_valid = 0;   /* re-init reset filter/wrap */
    }
    uint16_t *d = t->t.data;
    for (uint32_t y = 0; y < t->h; y++)
        for (uint32_t x = 0; x < t->w; x++) morton_put16(d, t->w, x, y, texels[y * t->w + x]);
    C3D_TexFlush(&t->t);
}

void pal_gpu_tex_free(uint32_t id)
{
    if (!id || id > MAX_TEX || !g.tex[id - 1].used) return;
    gtex *t = &g.tex[id - 1];
    if (t->frame == g.frame && g.ndead < (int)(sizeof g.dead / sizeof g.dead[0])) g.dead[g.ndead++] = t->t;      /* freed at the next begin_frame, after the GPU is done */
    else { end_frame(); if (t->frame == g.frame) { begin_frame(); } C3D_TexDelete(&t->t); }
    if (id == g.tex_vis) g.tex_filter_valid = 0;
    t->used = 0;
}

/* ---- target <-> guest memory ---- */

int pal_gpu_target_count(void) { return g.ok ? g.n_slots : 0; }

void pal_gpu_target_select(int slot)
{
    if (!g.ok || slot < 0 || slot >= g.n_slots || slot == g.cur_slot) return;
    end_frame();                     /* finish the old target before drawing to the new one */
    g.cur_slot = slot;
}

/* Rotated targets (g.rot): the transferred (linear, bottom-up) image has g.w rows of g.h pixels;
 * staging[(h-1-y)*w + x] = rot[x*h + (h-1-y)]. */
static void rot_to_staging(uint16_t *dst, const uint16_t *rot)
{
    const uint32_t w = g.w, h = g.h;
    /* 16x16 blocks: both the strided reads and the strided writes stay inside a few cache lines per block */
    for (uint32_t r0 = 0; r0 < h; r0 += 16u) {
        const uint32_t r1 = r0 + 16u < h ? r0 + 16u : h;
        for (uint32_t x0 = 0; x0 < w; x0 += 16u) {
            const uint32_t x1 = x0 + 16u < w ? x0 + 16u : w;
            for (uint32_t r = r0; r < r1; r++) {
                uint16_t *d = dst + r * w;
                const uint16_t *s0 = rot + r;
                for (uint32_t x = x0; x < x1; x++) d[x] = s0[x * h];
            }
        }
    }
}
static void staging_to_rot(uint16_t *rot, const uint16_t *src)
{
    const uint32_t w = g.w, h = g.h;
    for (uint32_t r0 = 0; r0 < h; r0 += 16u) {
        const uint32_t r1 = r0 + 16u < h ? r0 + 16u : h;
        for (uint32_t x0 = 0; x0 < w; x0 += 16u) {
            const uint32_t x1 = x0 + 16u < w ? x0 + 16u : w;
            for (uint32_t r = r0; r < r1; r++) {
                const uint16_t *s0 = src + r * w;
                uint16_t *d = rot + r;
                for (uint32_t x = x0; x < x1; x++) d[x * h] = s0[x];
            }
        }
    }
}

/* Fused guest-memory <-> rotated-target transposes (the game surface is top-down, the staging image bottom-up):
 * one pass instead of a row copy plus a transpose. */
static void px_to_rot(uint16_t *rot, const uint8_t *px, uint32_t pitch)
{
    const uint32_t w = g.w, h = g.h;
    if (!((uintptr_t)px & 3u) && !(pitch & 3u) && !((uintptr_t)rot & 3u) && !(w & 15u) && !(h & 15u)) {
        /* 2x2 blocks through 32-bit words: half the memory operations of the scalar loop */
        for (uint32_t r0 = 0; r0 < h; r0 += 16u)
            for (uint32_t x0 = 0; x0 < w; x0 += 16u)
                for (uint32_t r = r0; r < r0 + 16u; r += 2u) {
                    const uint32_t *A = (const uint32_t *)(px + (h - 1u - r) * pitch) + (x0 >> 1);
                    const uint32_t *B = (const uint32_t *)(px + (h - 2u - r) * pitch) + (x0 >> 1);
                    uint32_t *d = (uint32_t *)(rot + x0 * h + r);
                    for (uint32_t k = 0; k < 8u; k++, d += h) {      /* d += h words = two columns of h halfwords */
                        uint32_t a = A[k], b = B[k];
                        d[0] = (a & 0xffffu) | (b << 16);
                        d[h >> 1] = (a >> 16) | (b & 0xffff0000u);
                    }
                }
        return;
    }
    for (uint32_t r0 = 0; r0 < h; r0 += 16u) {
        const uint32_t r1 = r0 + 16u < h ? r0 + 16u : h;
        for (uint32_t x0 = 0; x0 < w; x0 += 16u) {
            const uint32_t x1 = x0 + 16u < w ? x0 + 16u : w;
            for (uint32_t r = r0; r < r1; r++) {
                const uint16_t *s0 = (const uint16_t *)(px + (h - 1u - r) * pitch);
                uint16_t *d = rot + r;
                for (uint32_t x = x0; x < x1; x++) d[x * h] = s0[x];
            }
        }
    }
}
static void rot_to_px(uint8_t *px, uint32_t pitch, const uint16_t *rot)
{
    const uint32_t w = g.w, h = g.h;
    if (!((uintptr_t)px & 3u) && !(pitch & 3u) && !((uintptr_t)rot & 3u) && !(w & 15u) && !(h & 15u)) {
        for (uint32_t r0 = 0; r0 < h; r0 += 16u)
            for (uint32_t x0 = 0; x0 < w; x0 += 16u)
                for (uint32_t r = r0; r < r0 + 16u; r += 2u) {
                    uint32_t *A = (uint32_t *)(px + (h - 1u - r) * pitch) + (x0 >> 1);
                    uint32_t *B = (uint32_t *)(px + (h - 2u - r) * pitch) + (x0 >> 1);
                    const uint32_t *s0 = (const uint32_t *)(rot + x0 * h + r);
                    for (uint32_t k = 0; k < 8u; k++, s0 += h) {
                        uint32_t c0 = s0[0], c1 = s0[h >> 1];
                        A[k] = (c0 & 0xffffu) | (c1 << 16);
                        B[k] = (c0 >> 16) | (c1 & 0xffff0000u);
                    }
                }
        return;
    }
    for (uint32_t r0 = 0; r0 < h; r0 += 16u) {
        const uint32_t r1 = r0 + 16u < h ? r0 + 16u : h;
        for (uint32_t x0 = 0; x0 < w; x0 += 16u) {
            const uint32_t x1 = x0 + 16u < w ? x0 + 16u : w;
            for (uint32_t r = r0; r < r1; r++) {
                uint16_t *d = (uint16_t *)(px + (h - 1u - r) * pitch);
                const uint16_t *s0 = rot + r;
                for (uint32_t x = x0; x < x1; x++) d[x] = s0[x * h];
            }
        }
    }
}

void pal_gpu_target_load(int slot, const uint8_t *px, uint32_t pitch)
{
    if (!g.ok || slot < 0 || slot >= g.n_slots) return;
    pal_gpu_present_finish();
    end_frame();
    if (!g.rot) for (uint32_t y = 0; y < g.h; y++) memcpy(g.staging + (g.h - 1 - y) * g.w * 2, px + y * pitch, g.w * 2);
    if (g.rot) {
        px_to_rot((uint16_t *)g.disp, px, pitch);
        GSPGPU_FlushDataCache(g.disp, g.w * g.h * 2);
        C3D_SyncDisplayTransfer((u32 *)g.disp, GX_BUFFER_DIM(g.rw, g.rh), (u32 *)g.rt[slot]->frameBuf.colorBuf,
                                GX_BUFFER_DIM(g.rw, g.rh), XFER(1));
    } else {
        GSPGPU_FlushDataCache(g.staging, g.w * g.h * 2);
        C3D_SyncDisplayTransfer((u32 *)g.staging, GX_BUFFER_DIM(g.w, g.h), (u32 *)g.rt[slot]->frameBuf.colorBuf,
                                GX_BUFFER_DIM(g.w, g.h), XFER(1));
    }
    g.staged_valid = 0;              /* staging now holds guest input, not a target capture */
}

/* GPU target -> staging only, so a Flip can present without a guest-memory copy. */
int pal_gpu_target_stage(int slot)
{
    if (!g.ok || slot < 0 || slot >= g.n_slots) return 0;
    prof_sub(PROF_SUB_FLIP_STAGE);
    pal_gpu_present_finish();
    end_frame();
    if (g.rot) {
        C3D_SyncDisplayTransfer((u32 *)g.rt[slot]->frameBuf.colorBuf, GX_BUFFER_DIM(g.rw, g.rh), (u32 *)g.disp,
                                GX_BUFFER_DIM(g.rw, g.rh), XFER(0));
        GSPGPU_InvalidateDataCache(g.disp, g.w * g.h * 2);
        rot_to_staging((uint16_t *)g.staging, (const uint16_t *)g.disp);
    } else {
        C3D_SyncDisplayTransfer((u32 *)g.rt[slot]->frameBuf.colorBuf, GX_BUFFER_DIM(g.w, g.h), (u32 *)g.staging,
                                GX_BUFFER_DIM(g.w, g.h), XFER(0));
        GSPGPU_InvalidateDataCache(g.staging, g.w * g.h * 2);
    }
    prof_sub(PROF_SUB_NONE);
    g.staged_valid = 1;
    g.staged_slot = slot;
    return 1;
}

/* Picture position calibration (bottom-screen UI, Game tab): the real LCD shows the picture a pixel or so off what the
 * transfer produced, so the finished top framebuffer (240x400, column-major, row 0 = BOTTOM of the screen) can be moved by
 * whole pixels: dx > 0 = right, dy > 0 = up. The vacated edge repeats its neighbour. Aligned 32-bit accesses only (a
 * halfword-offset memmove is an unaligned access and crashes the console). */
int g_scr_dx = -1, g_scr_dy = -1;    /* calibrated on the console: fixed: X -1, Y -1 is the exact picture position */
static void screen_nudge(uint16_t *fb, int dx, int dy)
{
    GSPGPU_InvalidateDataCache((void *)fb, 400 * 240 * 2);    /* the transfer wrote it behind the CPU cache */
    if (dx > 0) {
        memmove(fb + 240 * dx, fb, (size_t)(400 - dx) * 240 * 2);
        for (int c = 0; c < dx; c++) memcpy(fb + 240 * c, fb + 240 * dx, 240 * 2);
    } else if (dx < 0) {
        int n = -dx;
        memmove(fb, fb + 240 * n, (size_t)(400 - n) * 240 * 2);
        for (int c = 400 - n; c < 400; c++) memcpy(fb + 240 * c, fb + 240 * (400 - n - 1), 240 * 2);
    }
    if (dy) {
        uint32_t *w = (uint32_t *)fb;
        const uint32_t nw = 400u * 240u / 2u;
        for (int s = 0; s < (dy < 0 ? -dy : dy); s++) {
            if (dy > 0) {         /* element i takes element i-1: one row up */
                for (uint32_t i = nw - 1u; i > 0; i--) w[i] = (w[i - 1u] >> 16) | (w[i] << 16);
                w[0] <<= 16;
            } else {              /* element i takes element i+1: one row down */
                for (uint32_t i = 0; i + 1u < nw; i++) w[i] = (w[i] >> 16) | (w[i + 1u] << 16);
                w[nw - 1u] >>= 16;
            }
        }
        int n = dy < 0 ? -dy : dy;
        for (int c = 0; c < 400; c++) {
            uint16_t *col = fb + c * 240;
            if (dy > 0) { for (int r = 0; r < n; r++) col[r] = col[n]; }
            else        { for (int r = 0; r < n; r++) col[239 - r] = col[239 - n]; }
        }
    }
}

/* Rotated 800x480 target -> the top screen framebuffer: the display transfer's 2x2 downscale writes the
 * 240x400 column-major image directly (no CPU pass). The output dimension is given as the INPUT size: the
 * engine packs the scaled image with a stride of half the input width (checked in Azahar). */
int pal_gpu_direct_ok(void) { return g.ok && g.rot && xf.ok && !g_xf_off; }

int pal_gpu_present_direct(int slot, uint16_t *copy)
{
    if (!g.ok || !g.rot || slot < 0 || slot >= g.n_slots) return 0;
    { void pal3ds_count_frame(void); pal3ds_count_frame(); }   /* fake-clock frame count, pal_3ds.c */
    prof_sub(PROF_SUB_FLIP_STAGE);
    pal_gpu_present_finish();
    uint64_t tq0 = svcGetSystemTick();
    end_frame();
    pal_3ds_vsync_guard();                    /* two swaps within one blank would make this write hit the displayed buffer */
    if (xf.ok && !g_xf_off) {                 /* hand the transfer to the worker; the swap happens in pal_gpu_present_finish() */
        xf.slot = slot;
        xf.pending = 1;
        LightEvent_Signal(&xf.job);
        if (copy) {                           /* screenshot: wait for the worker and de-rotate its result before the swap (same data as the sync path) */
            LightEvent_Wait(&xf.done);
            const uint16_t *fb = (const uint16_t *)pal_3ds_top_fb();
            for (uint32_t y = 0; y < 240; y++)
                for (uint32_t x = 0; x < 400; x++) copy[y * 400 + x] = fb[x * 240 + (239 - y)];
            xf.pending = 0;
            pal_3ds_top_swap();
        }
        prof_sub(PROF_SUB_NONE);
        g.staged_valid = 0;
        return 1;
    }
    uint64_t tq1 = svcGetSystemTick();
    C3D_SyncDisplayTransfer((u32 *)g.rt[slot]->frameBuf.colorBuf, GX_BUFFER_DIM(g.rw, g.rh), (u32 *)pal_3ds_top_fb(),
                            GX_BUFFER_DIM(g.rw, g.rh),
                            (XFER(0) & ~GX_TRANSFER_SCALING(3)) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY));
    {   /* The LCD's raw output is one element late in the column-major stream: every column's top row holds the NEXT column's
         * bottom row (the stray line along the top of the screen) and the picture sits one row low. Re-aligning the stream by
         * one element (element i takes element i-1; aligned 32-bit words only) restores every pixel exactly. */
        uint16_t *fb = (uint16_t *)pal_3ds_top_fb();
        GSPGPU_InvalidateDataCache((void *)fb, 400 * 240 * 2);    /* the transfer wrote it behind the CPU cache */
        uint32_t *w = (uint32_t *)fb;
        for (uint32_t i = 400u * 240u / 2u - 1u; i > 0; i--) w[i] = (w[i - 1u] >> 16) | (w[i] << 16);
        w[0] <<= 16;
        if (g_scr_dx || g_scr_dy) screen_nudge(fb, g_scr_dx, g_scr_dy);
    }
    if (copy) {                       /* screenshot: de-rotate the 240x400 column-major image before the swap */
        const uint16_t *fb = (const uint16_t *)pal_3ds_top_fb();
        for (uint32_t y = 0; y < 240; y++)
            for (uint32_t x = 0; x < 400; x++) copy[y * 400 + x] = fb[x * 240 + (239 - y)];
    }
    uint64_t tq2 = svcGetSystemTick();
    pal_3ds_top_swap();
    {
        static uint64_t a1, a2, a3; static unsigned n;
        a1 += tq1 - tq0; a2 += tq2 - tq1; a3 += svcGetSystemTick() - tq2;
        if (++n % 300 == 0) pal_log("present: frame_end %lu us, transfer wait %lu us, swap %lu us", (unsigned long)(a1 * 1000 / CPU_TICKS_PER_MSEC / n), (unsigned long)(a2 * 1000 / CPU_TICKS_PER_MSEC / n), (unsigned long)(a3 * 1000 / CPU_TICKS_PER_MSEC / n));
    }
    prof_sub(PROF_SUB_NONE);
    g.staged_valid = 0;
    return 1;
}

const uint16_t *pal_gpu_staging(void) { return (const uint16_t *)g.staging; }

void pal_gpu_target_store(int slot, uint8_t *px, uint32_t pitch)
{
    if (!g.ok || slot < 0 || slot >= g.n_slots) return;
    pal_where("store: enter");
    pal_gpu_present_finish();
    pal_where("store: after present_finish");
    if (g.rot && (!g.staged_valid || g.staged_slot != slot)) {   /* no Flip capture to reuse: transpose straight into the guest surface */
        end_frame();
        C3D_SyncDisplayTransfer((u32 *)g.rt[slot]->frameBuf.colorBuf, GX_BUFFER_DIM(g.rw, g.rh), (u32 *)g.disp,
                                GX_BUFFER_DIM(g.rw, g.rh), XFER(0));
        GSPGPU_InvalidateDataCache(g.disp, g.w * g.h * 2);
        rot_to_px(px, pitch, (const uint16_t *)g.disp);
        return;
    }
    if (!g.staged_valid || g.staged_slot != slot) {   /* reuse the Flip transfer when possible */
        if (!pal_gpu_target_stage(slot)) return;
    }
    for (uint32_t y = 0; y < g.h; y++) memcpy(px + y * pitch, g.staging + (g.h - 1 - y) * g.w * 2, g.w * 2);
}

/* ---- drawing ---- */
static GPU_TESTFUNC test_func(uint8_t d3d)
{
    static const GPU_TESTFUNC m[9] = {GPU_ALWAYS, GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL,
                                      GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
    return d3d <= 8 ? m[d3d] : GPU_ALWAYS;
}

static GPU_BLENDFACTOR blend_factor(uint8_t d3d)
{
    static const GPU_BLENDFACTOR m[14] = {GPU_ONE, GPU_ZERO, GPU_ONE, GPU_SRC_COLOR, GPU_ONE_MINUS_SRC_COLOR,
                                          GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA,
                                          GPU_DST_COLOR, GPU_ONE_MINUS_DST_COLOR, GPU_SRC_ALPHA_SATURATE,
                                          GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA};
    return d3d < 14 ? m[d3d] : GPU_ONE;
}

/* One TEV stage half (RGB or alpha) from a D3DTEXTUREOP and its two arguments. */
static void tev(C3D_TexEnv *e, C3D_TexEnvMode mode, uint8_t op, uint8_t a1, uint8_t a2, int textured)
{
    GPU_TEVSRC s1 = textured && a1 == PAL_TA_TEXTURE ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR;
    GPU_TEVSRC s2 = textured && a2 == PAL_TA_TEXTURE ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR;
    GPU_COMBINEFUNC f = GPU_MODULATE;
    GPU_TEVSCALE sc = GPU_TEVSCALE_1;
    if (!textured) op = 1;                     /* no texture: the stage passes the diffuse colour (as r3d_soft) */
    switch (op) {
    case 1: case 2: f = GPU_REPLACE; if (op == 1) s1 = GPU_PRIMARY_COLOR; break;   /* DISABLE, SELECTARG1 */
    case 3: f = GPU_REPLACE; s1 = s2; break;                                        /* SELECTARG2 */
    case 5: sc = GPU_TEVSCALE_2; break;
    case 6: sc = GPU_TEVSCALE_4; break;
    case 7: f = GPU_ADD; break;
    case 8: f = GPU_ADD_SIGNED; break;
    default: break;                                                                 /* MODULATE */
    }
    C3D_TexEnvSrc(e, mode, s1, s2, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, mode, f);
    C3D_TexEnvScale(e, (int)mode, sc);
}

void pal_gpu_draw(const pal_gpu_state *st, const pal_gpu_vertex *v, uint32_t n)
{
    if (!g.ok || !n) return;
    pal_where("draw: enter");
    prof_sub(PROF_SUB_PAL_FRAME);
    begin_frame();
    if (g.staged_valid && g.staged_slot == g.cur_slot) g.staged_valid = 0;
    if ((g.vpos + n) * sizeof(pal_gpu_vertex) > VB_BYTES) { end_frame(); begin_frame(); }
    if ((g.vpos + n) * sizeof(pal_gpu_vertex) > VB_BYTES) { prof_sub(PROF_SUB_NONE); return; }
    pal_gpu_vertex *dst = g.vb + g.vpos;
    prof_sub(PROF_SUB_PAL_VCOPY);
    for (uint32_t i = 0; i < n; i++) { dst[i] = v[i]; dst[i].v = 1.0f - v[i].v; }
    prof_sub(PROF_SUB_PAL_BIND);
    int first = !g.cs_valid;
    gtex *t = st->tex && st->tex <= MAX_TEX && g.tex[st->tex - 1].used ? &g.tex[st->tex - 1] : NULL;
    uint32_t tid = t ? st->tex : 0;
    if (first || tid != g.tex_vis || !g.tex_filter_valid) {
        if (t) {
            GPU_TEXTURE_FILTER_PARAM fl = st->linear ? GPU_LINEAR : GPU_NEAREST;
            C3D_TexSetFilter(&t->t, fl, fl);
            C3D_TexSetWrap(&t->t, st->clamp_u ? GPU_CLAMP_TO_EDGE : GPU_REPEAT, st->clamp_v ? GPU_CLAMP_TO_EDGE : GPU_REPEAT);
            C3D_TexBind(0, &t->t);
        } else C3D_TexBind(0, &g.fog);   /* never NULL: citro3d 1.7.1 dereferences it (data abort on hardware); TEV does not sample it */
        g.tex_vis = tid;
        g.tex_filter_valid = 1;
    }
    if (t) t->frame = g.frame;

    prof_sub(PROF_SUB_PAL_TEV);
    if (first || (t != NULL) != (g.cs.tex != 0) || st->color_op != g.cs.color_op ||
        st->color_a1 != g.cs.color_a1 || st->color_a2 != g.cs.color_a2 || st->alpha_op != g.cs.alpha_op ||
        st->alpha_a1 != g.cs.alpha_a1 || st->alpha_a2 != g.cs.alpha_a2) {
        C3D_TexEnv *e0 = C3D_GetTexEnv(0);
        C3D_TexEnvInit(e0);
        tev(e0, C3D_RGB, st->color_op, st->color_a1, st->color_a2, t != NULL);
        tev(e0, C3D_Alpha, st->alpha_op, st->alpha_a1, st->alpha_a2, t != NULL);
    }
    if (first || st->fog != g.cs.fog || (st->fog && st->fog_rgb != g.cs.fog_rgb)) {
        C3D_TexEnv *e1 = C3D_GetTexEnv(1);
        C3D_TexEnvInit(e1);
        if (st->fog) {
            C3D_TexBind(1, &g.fog);
            C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEXTURE1);
            C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
            C3D_TexEnvFunc(e1, C3D_RGB, GPU_INTERPOLATE);
            uint32_t c = st->fog_rgb;
            C3D_TexEnvColor(e1, ((c >> 16) & 0xffu) | (c & 0xff00u) | ((c & 0xffu) << 16) | 0xff000000u);
        } else C3D_TexBind(1, &g.fog);   /* see unit 0: never bind NULL; stage 1 does not sample it without fog */
    }

    if (first || st->alpha_test != g.cs.alpha_test || st->alpha_func != g.cs.alpha_func ||
        st->alpha_ref != g.cs.alpha_ref)
        C3D_AlphaTest(st->alpha_test, test_func(st->alpha_func), st->alpha_ref);
    if (first || st->blend != g.cs.blend ||
        (st->blend && (st->src_blend != g.cs.src_blend || st->dst_blend != g.cs.dst_blend))) {
        if (st->blend)
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, blend_factor(st->src_blend), blend_factor(st->dst_blend),
                           blend_factor(st->src_blend), blend_factor(st->dst_blend));
        else
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }
    if (first || st->z_test != g.cs.z_test || st->z_write != g.cs.z_write || st->z_func != g.cs.z_func ||
        st->color_write != g.cs.color_write) {
        GPU_WRITEMASK wm = (GPU_WRITEMASK)((st->color_write ? GPU_WRITE_COLOR : 0) | (st->z_write ? GPU_WRITE_DEPTH : 0));
        C3D_DepthTest(true, st->z_test ? test_func(st->z_func) : GPU_ALWAYS, wm);
    }
    /* scissor in window coordinates: with clip y negated (CONV_Y), window row 0 is the top */
    if (first || st->clip_x0 != g.cs.clip_x0 || st->clip_y0 != g.cs.clip_y0 ||
        st->clip_x1 != g.cs.clip_x1 || st->clip_y1 != g.cs.clip_y1)
    {
        if (g.rot) {      /* game (x, y) -> target (tx, ty): tx = ROT_SX>0 ? h-y : y, ty = ROT_SY>0 ? x : w-x */
            uint32_t ax = st->clip_y0, bx = st->clip_y1, ay = st->clip_x0, by = st->clip_x1;
            uint32_t sl = ROT_SX > 0 ? g.h - bx : ax, sr = ROT_SX > 0 ? g.h - ax : bx;
            uint32_t stp = ROT_SY > 0 ? ay : g.w - by, sbt = ROT_SY > 0 ? by : g.w - ay;
            C3D_SetScissor(GPU_SCISSOR_NORMAL, sl, stp, sr, sbt);
        } else
            C3D_SetScissor(GPU_SCISSOR_NORMAL, st->clip_x0, st->clip_y0, st->clip_x1, st->clip_y1);
    }
    g.cs = *st;
    g.cs_valid = 1;

    prof_sub(PROF_SUB_PAL_DRAW);
    C3D_DrawArrays(GPU_TRIANGLES, (int)g.vpos, (int)n);
    g.vpos += n;
    /* Submit a part of the frame early so the GPU renders it while the CPU builds the rest (before: the whole list went out at the
     * Flip and the CPU then waited ~3 ms for the GPU). begin_frame() of the next part waits for this one lazily. */
    if (g.vpos >= g_split_verts) end_frame();
    prof_sub(PROF_SUB_NONE);
}
