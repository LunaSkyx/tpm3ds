#include <stdlib.h>
/* r3d front end: state + statistics; rasterisation by the software backend (r3d_soft.c). */
#include <string.h>
#include "core/gheap.h"
#include "core/render/r3d.h"
#include "core/render/r3d_gpu.h"
#include "core/render/r3d_q.h"
#include "core/render/r3d_soft.h"
#include "pal/pal.h"

r3d_state g_r3d;

/* Section tag read by the sampler (core/prof.h): the GPU front end and the
 * 3DS pal store it with plain stores. Defined here because every build links
 * the renderer, while core/prof.c is only part of the guest runtime. */
volatile uint32_t g_prof_sub;

static uint32_t g_gen_ctr;
void r3d_state_dirty(void) { g_r3d.gen = ++g_gen_ctr; }

void r3d_reset(void)
{
    memset(&g_r3d, 0, sizeof g_r3d);
    g_r3d.xf_gen = 1;     /* never let a stale MVP cache match the post-reset state */
    r3d_state_dirty();
    r3d_q_state_reset();
    g_r3d.rs[7] = 1;      /* ZENABLE */
    g_r3d.rs[14] = 1;     /* ZWRITEENABLE */
    g_r3d.rs[23] = 4;     /* ZFUNC = LESSEQUAL */
    g_r3d.rs[19] = 2;     /* SRCBLEND = ONE */
    g_r3d.rs[20] = 1;     /* DESTBLEND = ZERO */
    g_r3d.rs[22] = 3;     /* CULLMODE = CCW */
    g_r3d.rs[9] = 2;      /* SHADEMODE = GOURAUD */
    for (int k = 0; k < 16; k += 5) g_r3d.world[k] = g_r3d.view[k] = g_r3d.proj[k] = 1.0f;
    /* texture stage defaults (D3D): stage 0 modulates texture and diffuse */
    g_r3d.tss[0][1] = 4; g_r3d.tss[0][2] = 2; g_r3d.tss[0][3] = 1;      /* COLOROP MODULATE, TEXTURE, CURRENT */
    g_r3d.tss[0][4] = 2; g_r3d.tss[0][5] = 2; g_r3d.tss[0][6] = 1;      /* ALPHAOP SELECTARG1, TEXTURE, CURRENT */
    g_r3d.tss[1][1] = 1; g_r3d.tss[1][4] = 1;                           /* stage 1 DISABLE */
    for (int st = 0; st < 2; st++) { g_r3d.tss[st][13] = g_r3d.tss[st][14] = 1; g_r3d.tss[st][16] = g_r3d.tss[st][17] = 1; }
}

void r3d_set_target(dd_surface *target, dd_surface *zbuf)
{
    r3d_q_flush();
    g_r3d.target = target;
    g_r3d.zbuf = zbuf;
    r3d_state_dirty();
    if (target && target->pf.bits == 16) r3d_gpu_init(target->w, target->h);
}
extern int g_r3d_probe_scene;
int g_r3d_null;      /* draws and clears are counted, not rendered (host profiling of guest code) */
int g_r3d_prof;      /* per-draw raster timing: two time queries per call, off unless asked for */
extern void cheats_pre_render(void);
void r3d_begin_scene(void) { cheats_pre_render(); g_r3d.scenes++; g_r3d.probe_budget = (int)g_r3d.scenes == g_r3d_probe_scene ? 64 : 0; }
static uint64_t g_raster_us, g_last_log_us;
void r3d_end_scene(void)
{
    if (g_r3d.scenes % 300 == 0 && getenv("TPM_TRACE_VP")) {   /* debug: game camera view (FUN_00419800) */
        uint32_t cam = gmem_rd32(0x006f83e4u), view = cam ? gmem_rd32(cam + 4u) : 0;
        if (view) {
            uint32_t w[12];
            for (int k = 0; k < 12; k++) w[k] = gmem_rd32(view + 4u * (uint32_t)k);
            float f8, fc; memcpy(&f8, &w[2], 4); memcpy(&fc, &w[3], 4);
            float c3c, c40, c44; uint32_t u;
            u = gmem_rd32(cam + 0x3cu); memcpy(&c3c, &u, 4);
            u = gmem_rd32(cam + 0x40u); memcpy(&c40, &u, 4);
            u = gmem_rd32(cam + 0x44u); memcpy(&c44, &u, 4);
            pal_log("cam %08lx: +3c %.4f +40 %.4f +44 %.4f", (unsigned long)cam, c3c, c40, c44);
            pal_log("view %08lx: center %.1f %.1f rect %ld %ld %ld %ld | %08lx %08lx %08lx %08lx %08lx %08lx", (unsigned long)view, f8, fc,
                    (long)w[6], (long)w[7], (long)w[8], (long)w[9], (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[4],
                    (unsigned long)w[5], (unsigned long)w[10], (unsigned long)w[11]);
        }
    }
    if (g_r3d.scenes == 1 || g_r3d.scenes % 300 == 0) {
        uint64_t now = pal_time_us(), span = now - g_last_log_us;
        pal_log("r3d: scene %lu: %lu draws, %lu triangles submitted, %lu rasterised, %lu clears; "
                "%lu batches, %lu tex uploads, %lu loads, %lu stores, %lu presents (%lu failed); "
                "raster %lu of %lu ms", (unsigned long)g_r3d.scenes, (unsigned long)g_r3d.draws,
                (unsigned long)g_r3d.tris, (unsigned long)g_r3d.tris_drawn, (unsigned long)g_r3d.clears,
                (unsigned long)g_r3d_gpu_batches, (unsigned long)g_r3d_gpu_tex_uploads,
                (unsigned long)g_r3d_gpu_loads, (unsigned long)g_r3d_gpu_stores,
                (unsigned long)g_r3d_gpu_presents, (unsigned long)g_r3d_gpu_present_fail,
                (unsigned long)(g_raster_us / 1000), (unsigned long)(span / 1000));
        /* RAM trace: a leak shows up as a monotonic fall of the free counts. The guest
         * heap holds the game's own allocations, VirtualAlloc the big guest buffers and
         * linear the VRAM-backed one (GPU targets, textures, ndsp, video frames; 0 on host). */
        extern int g_pal_fh_open, g_pal_dir_open;
        pal_log("ram: guest heap %lu KB free (peak %lu KB used), VirtualAlloc %lu KB free, linear %lu KB free, host %lu KB free, open files %d dirs %d",
                (unsigned long)(gheap_free_bytes() >> 10), (unsigned long)(gheap_peak_bytes() >> 10),
                (unsigned long)(gvirt_free_bytes() >> 10), (unsigned long)(pal_linear_free() >> 10), (unsigned long)(pal_host_free() >> 10), g_pal_fh_open, g_pal_dir_open);
        if (g_r3d_prof) {
            static uint32_t lp, lf, ll, ls;
            pal_log("r3d: gpu: %lu presents (%lu fallback), %lu target loads, %lu target stores",
                    (unsigned long)(g_r3d_gpu_presents - lp), (unsigned long)(g_r3d_gpu_present_fail - lf),
                    (unsigned long)(g_r3d_gpu_loads - ll), (unsigned long)(g_r3d_gpu_stores - ls));
            lp = g_r3d_gpu_presents; lf = g_r3d_gpu_present_fail;
            ll = g_r3d_gpu_loads; ls = g_r3d_gpu_stores;
        }
#if defined(TPM_CALL_COUNT) && !defined(CPU_FN_TRACE)
        {   /* perf plan A1: lifted function entries per frame since the last scene line */
            extern uint32_t g_fn_calls;
            static uint32_t last_calls, last_scene;
            uint32_t frames = g_r3d.scenes - last_scene;
            if (frames) pal_log("calls: %lu lifted function entries/frame over %lu frames",
                                (unsigned long)((g_fn_calls - last_calls) / frames), (unsigned long)frames);
            last_calls = g_fn_calls; last_scene = g_r3d.scenes;
        }
#endif
        g_raster_us = 0;
        g_last_log_us = now;
        pal_log_flush();
    }
}

void r3d_clear(uint32_t nrects, gptr rects, uint32_t flags, uint32_t color, float z)
{
    g_r3d.clears++;
    if (g_r3d_null) return;
    uint64_t t = g_r3d_prof ? pal_time_us() : 0;
    if (g_r3d_q_on) {
        if ((flags & 3u) && r3d_gpu_front_mode(PT_TRIANGLELIST, FVF_XYZRHW) == 1) {
            uint32_t rc_s[4 * 8], *rc = rc_s, k;
            if (nrects > 8) rc = malloc(16u * nrects);
            if (rc) {
                for (k = 0; k < nrects; k++) { rc[4 * k] = gmem_rd32(rects + 16 * k); rc[4 * k + 1] = gmem_rd32(rects + 16 * k + 4); rc[4 * k + 2] = gmem_rd32(rects + 16 * k + 8); rc[4 * k + 3] = gmem_rd32(rects + 16 * k + 12); }
                r3d_q_clear(rects ? nrects : 0, rc, flags, color, z);
                if (rc != rc_s) free(rc);
                return;
            }
        }
        r3d_q_fence();
        r3d_gpu_sync(g_r3d.target);
        r3d_soft_clear(nrects, rects, flags, color, z);
    } else if (!r3d_gpu_clear(nrects, rects, flags, color, z)) {
        r3d_gpu_sync(g_r3d.target);
        r3d_soft_clear(nrects, rects, flags, color, z);
    }
    if (g_r3d_prof) g_raster_us += pal_time_us() - t;
}

/* ---- render states. IDirect3DDevice3 still accepts the DX5 texture states
 * and translates them to texture stage 0 (WMAIN uses only these, never
 * SetTextureStageState): TEXTUREADDRESS(U/V), TEXTUREMAG/MIN, TEXTUREMAPBLEND. */
enum { TSS_COLOROP = 1, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG1, TSS_ALPHAARG2,
       TSS_ADDRESSU = 13, TSS_ADDRESSV, TSS_MAGFILTER = 16, TSS_MINFILTER, TSS_MIPFILTER };

void r3d_set_stage_state(uint32_t stage, uint32_t state, uint32_t value)
{
    if (stage >= 2 || state >= 32) return;
    g_r3d.tss[stage][state] = value;
    r3d_state_dirty();
    if (stage == 0 && state >= TSS_COLOROP && state <= TSS_ALPHAARG2) g_r3d.legacy_blend = 0;
}

void r3d_set_render_state(uint32_t s, uint32_t v)
{
    if (s >= 256) return;
    if (g_r3d.rs[s] == v && s != 3 && s != 44 && s != 45 && s != 17 && s != 18 && s != 21) return;   /* no-op set: keep the draw-state cache */
    g_r3d.rs[s] = v;
    r3d_state_dirty();
    uint32_t *t0 = g_r3d.tss[0];
    switch (s) {
    case 3:  t0[TSS_ADDRESSU] = t0[TSS_ADDRESSV] = v; break;          /* TEXTUREADDRESS */
    case 44: t0[TSS_ADDRESSU] = v; break;                              /* TEXTUREADDRESSU */
    case 45: t0[TSS_ADDRESSV] = v; break;                              /* TEXTUREADDRESSV */
    case 17: t0[TSS_MAGFILTER] = v == 1 ? 1 : 2; break;                /* TEXTUREMAG: D3DFILTER -> D3DTFG */
    case 18:                                                           /* TEXTUREMIN: D3DFILTER -> D3DTFN + D3DTFP */
        t0[TSS_MINFILTER] = (v == 1 || v == 3 || v == 4) ? 1 : 2;
        t0[TSS_MIPFILTER] = v <= 2 ? 1 : (v == 3 || v == 5) ? 2 : 3;
        break;
    case 21: g_r3d.legacy_blend = v; break;                            /* TEXTUREMAPBLEND: resolved per draw */
    default: break;
    }
}

/* D3DTBLEND_* on stage 0. MODULATE takes the texture's alpha when it has one,
 * else the diffuse alpha, so it depends on the bound texture. */
static void apply_legacy_blend(void)
{
    uint32_t m = g_r3d.legacy_blend, *t = g_r3d.tss[0];
    if (!m) return;
    dd_surface *s = g_r3d.tex[0];
    int tex_alpha = s && (s->pf.flags & DDPF_ALPHAPIXELS) && s->pf.a;
    enum { DISABLE = 1, SEL1, SEL2, MOD, BLENDTEXALPHA = 13, ADD = 7 };
    enum { DIFF = 0, CUR = 1, TEX = 2 };
    uint32_t cop = MOD, c1 = TEX, c2 = DIFF, aop = SEL1, a1 = TEX, a2 = DIFF;
    switch (m) {
    case 1: case 7: cop = SEL1; aop = SEL1; break;                     /* DECAL, COPY */
    case 2: aop = tex_alpha ? SEL1 : SEL2; break;                      /* MODULATE */
    case 3: cop = BLENDTEXALPHA; aop = SEL2; break;                    /* DECALALPHA */
    case 4: aop = MOD; break;                                          /* MODULATEALPHA */
    case 8: cop = ADD; aop = SEL2; break;                              /* ADD */
    default: break;                                                    /* DECALMASK/MODULATEMASK: as MODULATE */
    }
    if (t[TSS_COLOROP] != cop || t[TSS_COLORARG1] != c1 || t[TSS_COLORARG2] != c2 ||
        t[TSS_ALPHAOP] != aop || t[TSS_ALPHAARG1] != a1 || t[TSS_ALPHAARG2] != a2) r3d_state_dirty();
    t[TSS_COLOROP] = cop; t[TSS_COLORARG1] = c1; t[TSS_COLORARG2] = c2;
    t[TSS_ALPHAOP] = aop; t[TSS_ALPHAARG1] = a1; t[TSS_ALPHAARG2] = a2;
    (void)CUR;
}

uint32_t r3d_fvf_stride(uint32_t fvf)
{
    uint32_t s = 0;
    if (fvf & FVF_XYZRHW) s += 16;
    else if (fvf & FVF_XYZ) s += 12;
    if (fvf & FVF_NORMAL) s += 12;
    if (fvf & 0x020u) s += 4;            /* D3DFVF_RESERVED1 */
    if (fvf & FVF_DIFFUSE) s += 4;
    if (fvf & FVF_SPECULAR) s += 4;
    s += 8u * FVF_TEXCOUNT(fvf);
    return s;
}

/* Diagnostics (g_r3d_diag, host env TPM_R3D_DIAG=1): one log line per distinct
 * combination of everything the GPU path maps, with counts every 300 scenes. */
int g_r3d_diag;
static void diag_combo(uint32_t prim, uint32_t fvf, uint32_t n, gptr verts, uint32_t nverts)
{
    enum { MAXC = 96 };
    static uint32_t seen[MAXC][12], cnt[MAXC], ntri[MAXC];
    static int nseen;
    const uint32_t *rs = g_r3d.rs, *t0 = g_r3d.tss[0], *t1 = g_r3d.tss[1];
    dd_surface *s0 = g_r3d.tex[0], *s1 = g_r3d.tex[1];
    uint32_t key[12] = {
        fvf | prim << 16 | (s0 ? 1u << 24 : 0) | (s1 ? 1u << 25 : 0) | (s0 && s0->has_ck ? 1u << 26 : 0),
        t0[1] | t0[2] << 8 | t0[3] << 16 | t0[4] << 24,
        t0[5] | t0[6] << 8 | t1[1] << 16 | t1[4] << 24,
        t1[2] | t1[3] << 8 | t1[5] << 16 | t1[6] << 24,
        rs[27] | rs[19] << 8 | rs[20] << 16 | rs[15] << 24,
        rs[24] | rs[25] << 8 | rs[7] << 16 | rs[14] << 24,
        rs[23] | rs[28] << 8 | rs[29] << 16 | rs[41] << 24,
        rs[34], rs[60], rs[22] | rs[9] << 8 | rs[35] << 16 | rs[137 & 255] << 24,
        s0 ? s0->pf.a | (uint32_t)s0->pf.bits << 16 : 0,
        rs[52] | rs[48] << 8 | rs[26] << 16,
    };
    int i;
    for (i = 0; i < nseen; i++) if (!memcmp(seen[i], key, sizeof key)) break;
    if (i == nseen && nseen < MAXC) {
        memcpy(seen[nseen++], key, sizeof key);
        pal_log("r3d diag #%d: prim %lu fvf %03lx tex0 %s%s(a %04lx bits %lu) tex1 %s | s0 cop %lu %lu,%lu aop %lu %lu,%lu | "
                "s1 cop %lu %lu,%lu aop %lu %lu,%lu | blend %lu %lu/%lu atest %lu ref %lu fn %lu | z %lu w %lu fn %lu | "
                "fog %lu col %06lx tbl %lu | spec %lu ck %lu tfac %08lx cull %lu shade %lu | stencil %lu edge %lu dither %lu",
                i, (unsigned long)prim, (unsigned long)fvf, s0 ? "y" : "n", s0 && s0->has_ck ? " ck" : "",
                s0 ? (unsigned long)s0->pf.a : 0ul, s0 ? (unsigned long)s0->pf.bits : 0ul, s1 ? "y" : "n",
                (unsigned long)t0[1], (unsigned long)t0[2], (unsigned long)t0[3], (unsigned long)t0[4], (unsigned long)t0[5], (unsigned long)t0[6],
                (unsigned long)t1[1], (unsigned long)t1[2], (unsigned long)t1[3], (unsigned long)t1[4], (unsigned long)t1[5], (unsigned long)t1[6],
                (unsigned long)rs[27], (unsigned long)rs[19], (unsigned long)rs[20], (unsigned long)rs[15], (unsigned long)rs[24], (unsigned long)rs[25],
                (unsigned long)rs[7], (unsigned long)rs[14], (unsigned long)rs[23],
                (unsigned long)rs[28], (unsigned long)rs[34], (unsigned long)rs[35],
                (unsigned long)rs[29], (unsigned long)rs[41], (unsigned long)rs[60], (unsigned long)rs[22], (unsigned long)rs[9],
                (unsigned long)rs[52], (unsigned long)rs[40], (unsigned long)rs[26]);
    }
    if (rs[28] && rs[35]) {   /* table fog: parameters and the depth range of fogged TL vertices */
        static uint32_t fs_seen[16][3];
        static int nfs, budget = 40;
        uint32_t fk[3] = {rs[36], rs[37], rs[38]};
        int j;
        for (j = 0; j < nfs; j++) if (!memcmp(fs_seen[j], fk, sizeof fk)) break;
        if (j == nfs && nfs < 16) {
            memcpy(fs_seen[nfs++], fk, sizeof fk);
            float a, b, d; memcpy(&a, &fk[0], 4); memcpy(&b, &fk[1], 4); memcpy(&d, &fk[2], 4);
            pal_log("r3d diag fog: mode %lu start %g end %g density %g colour %06lx", (unsigned long)rs[35], a, b, d,
                    (unsigned long)rs[34]);
        }
        if (budget > 0 && (fvf & FVF_XYZRHW) && g_r3d.scenes > 700 && (g_r3d.scenes & 15) == 0) {
            budget--;
            float zmin = 1e30f, zmax = -1e30f, wmin = 1e30f, wmax = -1e30f;
            uint32_t st = r3d_fvf_stride(fvf), smin = 0xffffffffu, smax = 0;
            for (uint32_t v = 0; v < nverts; v++) {
                float z, r; uint32_t u;
                u = gmem_rd32(verts + v * st + 8); memcpy(&z, &u, 4);
                u = gmem_rd32(verts + v * st + 12); memcpy(&r, &u, 4);
                uint32_t sp = (fvf & FVF_SPECULAR) ? gmem_rd32(verts + v * st + 16 + ((fvf & FVF_DIFFUSE) ? 4 : 0)) >> 24 : 255;
                zmin = z < zmin ? z : zmin; zmax = z > zmax ? z : zmax;
                float w = r > 0 ? 1.0f / r : 0;
                wmin = w < wmin ? w : wmin; wmax = w > wmax ? w : wmax;
                smin = sp < smin ? sp : smin; smax = sp > smax ? sp : smax;
            }
            pal_log("r3d diag fogvtx: z %g..%g w %g..%g spec.a %lu..%lu", zmin, zmax, wmin, wmax,
                    (unsigned long)smin, (unsigned long)smax);
        }
    }
    if (i < MAXC) { cnt[i]++; ntri[i] += prim == PT_TRIANGLELIST ? n / 3 : n >= 3 ? n - 2 : 0; }
    if (g_r3d.scenes % 300 == 0) {
        static uint32_t last;
        if (last != g_r3d.scenes) {
            last = g_r3d.scenes;
            for (int k = 0; k < nseen; k++)
                if (cnt[k]) pal_log("r3d diag scene %lu: #%d %lu draws %lu tris", (unsigned long)last, k,
                                    (unsigned long)cnt[k], (unsigned long)ntri[k]);
            memset(cnt, 0, sizeof cnt); memset(ntri, 0, sizeof ntri);
        }
    }
}

static int n3_ok(uint32_t n) { return n != 0; }

void r3d_draw(uint32_t prim, uint32_t fvf, gptr verts, uint32_t nverts, gptr indices, uint32_t nindices)
{
    uint32_t n = indices ? nindices : nverts;
    g_r3d.draws++;
    if (!indices && nverts == 4 && (fvf & 4u) && prim != 4u && r3d_fvf_stride(fvf) >= 8u) {
        /* The menu's mouse pointer: the game draws a 32x32 sprite at its pointer position (screen centre, 320,240 of 640x480) on the last
         * quad of every menu frame. The 3DS has no mouse, so it only sits in the middle of the picture: leave it out. */
        float x0, y0, x1, y1 = 0, y2; uint32_t u;
        u = gmem_rd32(verts); memcpy(&x0, &u, 4); u = gmem_rd32(verts + 4); memcpy(&y0, &u, 4);
        u = gmem_rd32(verts + r3d_fvf_stride(fvf)); memcpy(&x1, &u, 4);
        u = gmem_rd32(verts + 2u * r3d_fvf_stride(fvf) + 4); memcpy(&y2, &u, 4); (void)y1;
        if (x0 == 320.0f && y0 == 240.0f && x1 == 352.0f && y2 == 272.0f) return;
    }
    apply_legacy_blend();
    if (g_r3d_diag) diag_combo(prim, fvf, n, verts, nverts);
    switch (prim) {
    case PT_TRIANGLELIST: g_r3d.tris += n / 3; break;
    case PT_TRIANGLESTRIP:
    case PT_TRIANGLEFAN:  g_r3d.tris += n >= 3 ? n - 2 : 0; break;
    default: break;
    }
    if (g_r3d_null) return;
    uint64_t t = g_r3d_prof ? pal_time_us() : 0;
    if (g_r3d_q_on) {
        int m = r3d_gpu_front_mode(prim, fvf);
        if (m == 1) {
            uint32_t stride = r3d_fvf_stride(fvf);
            uint32_t nn = indices ? nindices : nverts;
            const uint8_t *vs = n3_ok(nn) ? gmem_span(verts, nverts * stride, 0) : NULL;
            const uint8_t *is = indices ? gmem_span(indices, nindices * 2u, 0) : NULL;
            if (vs && (!indices || is)) { r3d_q_draw(prim, fvf, vs, nverts, is, indices ? nindices : 0); return; }
            return;                                                   /* unreadable guest memory: the direct path skips the draw too */
        }
        if (m == 2) return;
        r3d_q_fence();
        r3d_gpu_sync(g_r3d.target);
        r3d_soft_draw(prim, fvf, verts, nverts, indices, nindices);
    } else if (!r3d_gpu_draw(prim, fvf, verts, nverts, indices, nindices)) {
        r3d_gpu_sync(g_r3d.target);
        r3d_soft_draw(prim, fvf, verts, nverts, indices, nindices);
    }
    if (g_r3d_prof) g_raster_us += pal_time_us() - t;
}
