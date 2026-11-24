/* Native world_cull, guest 0x004199b0 (docs/PERF_PLAN.md section 3).
 *
 * Called once per frame from render 0x43f57a. For every polygon in the visible
 * bucket lists it culls, picks the texture, transforms and lights the vertices
 * into the vertex cache (0x5bf9f0) and stores the clip code in the list node.
 *
 * This file is a transcription of the x86 listing docs/asm/004199b0_world_cull.txt.
 * The listing is the authority; the lifted body (wmain_004199b0_ref, host only,
 * TPM_OVERRIDE_REF) is the executable reference.
 *
 * Conventions kept on purpose:
 *  - Guest frame: body ESP F = entry ESP - 0xf8 (the listing's `sub esp,0xe8`
 *    plus four pushes). Guest memory at F+0x4c, F+0x68, F+0x88, F+0x98, F+0xc8
 *    holds the arrays that are passed by address to callees. Everything else is
 *    in C locals.
 *  - Callees get cdecl arguments on the guest stack below F, as in the original
 *    (wc_call). Lifted and native callees both take cpu_t*.
 *  - x87: values are C doubles, as in the lifter. FCOM() keeps the fsw condition
 *    bits identical to the reference. ftop and fcw are never changed.
 *  - EBX/ESI/EDI/EBP are never touched (cdecl callee-saved). EAX/ECX/EDX keep their
 *    entry values except where a callee sets them (dead for the only caller).
 *
 * Verification (host, TPM_OVERRIDE_REF): env TPM_VERIFY_WC=1 runs native and
 * reference on the same input state for every call, and compares the guest arena
 * (except dead stack below the entry ESP), EBX/ESI/EDI/EBP/ESP, ftop, fsw, fcw.
 * The reference result is kept, so the run stays on the reference path.
 * Branch counters (BR_*) are printed at exit.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/gmem.h"
#include "core/prof.h"
#include "pal/pal.h"

/* ---- guest addresses (names from docs/PERF_PLAN.md section 3.2) ---- */
#define G_VIEWP    0x006f83e4u   /* -> view struct; 4x3 matrix at +8 */
#define G_OBJ_CUR  0x005bb610u   /* current object index (init 0xff) */
#define G_OBJ_ACT  0x005bb57cu   /* current object active */
#define G_OBJ_PTR  0x005bb5a8u   /* current object pointer */
#define G_SUB_CUR  0x006bf9f0u   /* current sub-object index (0xff = none) */
#define G_SUB_PTR  0x005bf9e4u   /* sub-object record pointer */
#define G_O2C      0x005bb5c8u   /* object -> camera matrix */
#define G_INV      0x005bb548u   /* inverse sub-object matrix */
#define G_CAMP     0x006bf9f8u   /* camera position copy (3 dwords) */
#define G_CAMSRC   0x00868704u   /* camera position source (3 dwords) */
#define G_CAMOBJ   0x005bb588u   /* camera position, object space */
#define G_SCR      0x005bb598u   /* scratch vector x,y,z */
#define G_VT       0x006bfa08u   /* transformed vertex (camera space) */
#define G_CACHECNT 0x005bb5c0u   /* vertex cache count */
#define G_CACHE    0x005bf9f0u   /* vertex cache slots, 64 bytes each */
#define G_BIAS     0x005bf9e8u   /* depth bias */
#define G_ONE      0x006bfa14u   /* set to 1 at entry */
#define G_CULLCNT  0x008a0064u   /* polygons culled by clip codes */
#define G_CAM      0x008a011cu   /* camera object (position at +0x24) */
#define G_TIME     0x00868718u   /* float time (sin argument) */
#define G_LIGHT    0x0089fe34u   /* lighting on */
#define G_TOG_A    0x0089fe30u   /* render toggles -> mask */
#define G_TOG_B    0x0089fe44u
#define G_TOG_C    0x0089fe48u
#define G_ORFLAGS  0x005bb594u   /* OR'd into the polygon flags */
#define G_BUCKETS  0x008bc880u   /* bucket list heads */
/* constants in .rdata: read from guest memory, not hard-coded */
#define K_ZERO     0x004a81a8u   /* 0.0 */
#define K_HALF     0x004a81b4u   /* 0.5 */
#define K_255      0x004a81bcu   /* 255.0 */
#define K_M1       0x004a81c0u   /* -1.0 */
#define K_INV64    0x004a81c4u   /* 1/64 */
#define K_240      0x004a81c8u   /* 240.0 */
#define K_MQ25     0x004a81ccu   /* -0.25 */
#define K_180      0x004a81d0u   /* 180.0 */

/* callees (lifted or hand-written; all take cpu_t*) */
void wmain_00409170(cpu_t *c);
void wmain_0041ae50(cpu_t *c);
void wmain_0041a350(cpu_t *c);
void wmain_00428615(cpu_t *c);
void wmain_00427c9a(cpu_t *c);
void wmain_00494272(cpu_t *c);           /* CRT sin: float arg, result in st0 */
void wmain_0047dad6(cpu_t *c);
void wmain_0047ceb9(cpu_t *c);
void wmain_0047e34d(cpu_t *c);
void wmain_0041af30(cpu_t *c);
void wmain_004075c6(cpu_t *c);
void wmain_004074d4(cpu_t *c);

#ifndef __3DS__
void wmain_004199b0_ref(cpu_t *c);
/* Branch counters: a verified run must hit every branch at least once. */
enum {
    BR_OBJ_CHANGE, BR_SUB_SWITCH, BR_SIDE_OBJ, BR_SIDE_WORLD, BR_SIDE_NAN,
    BR_SIDE_2SIDED_KEEP, BR_BILLBOARD, BR_BACKFACE_KILL, BR_TEX_ANIM_CHANGE,
    BR_TEX_STATIC_CHANGE, BR_TEX_FRAME_CALL, BR_YOFF, BR_DEPTH_BIAS, BR_LIGHT,
    BR_DARK, BR_DARKEN_LOOP, BR_CACHE_FULL, BR_ANDC_KILL, BR_BB_OR80_KILL,
    BR_ANIMTEX_SKIP, BR_COLOUR, BR_WATER_CALL, BR_CACHED_VERTEX, BR_WORLD_XFORM,
    BR_OBJ_XFORM, BR_BB_QUAD, BR_NODE_HIDDEN_KILL, BR_TEX_RANGE_KILL, BR_SHORT_KILL,
    BR_BACK_TEX, BR_FRONT_TEX, BR_OBJ_INACTIVE, BR_NCOUNT
};
static const char *const br_name[BR_NCOUNT] = {
    "obj_change", "sub_switch", "side_obj", "side_world", "side_nan",
    "side_2sided_keep", "billboard", "backface_kill", "tex_anim_change",
    "tex_static_change", "tex_frame_call", "yoff", "depth_bias", "light",
    "dark", "darken_loop", "cache_full", "andc_kill", "bb_or80_kill",
    "animtex_skip", "colour", "water_call", "cached_vertex", "world_xform",
    "obj_xform", "bb_quad", "node_hidden_kill", "tex_range_kill", "short_kill",
    "back_tex", "front_tex", "obj_inactive"
};
static uint64_t br_hits[BR_NCOUNT];
#define BR(i) (br_hits[(i)]++)
#else
#define BR(i) ((void)0)
#endif

/* ---- guest memory helpers (same family as native_overrides.c) ---- */
static inline float wld_f(uint8_t *gm, uint32_t gsz, gptr a) { return gmem_f32_l(gmem_rd32_l(gm, gsz, a)); }
static inline void wst_f(uint8_t *gm, uint32_t gsz, gptr a, float f) { gmem_wr32_l(gm, gsz, a, gmem_u32_f(f)); }

/* __ftol / fistp with truncation as used at 0x49a44c: 0x80000000 when out of range or NaN. */
static inline uint32_t wc_ftol(double v)
{
    if (v > -2147483649.0 && v < 2147483648.0) return (uint32_t)(int32_t)v;
    return 0x80000000u;
}

/* Calls guest function fn with cdecl args a[0..n-1]. The callee frame goes below sp,
 * which is the body ESP of world_cull. ret is the return address the original pushes. */
static void wc_call(cpu_t *cg, void (*fn)(cpu_t *), gptr sp, gptr ret, uint32_t n, const uint32_t *a)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    gptr s = sp - 4u * (n + 1u);
    uint32_t i;

    for (i = 0; i < n; i++) gmem_wr32_l(gm, gsz, s + 4u + 4u * i, a[i]);
    gmem_wr32_l(gm, gsz, s, ret);
    cg->r[ESP] = s;
    fn(cg);
    cg->r[ESP] = sp;
}

/* Same for a callee that returns its value in st0; the x87 top is restored. */
static double wc_call_x87(cpu_t *cg, void (*fn)(cpu_t *), gptr sp, gptr ret, uint32_t n, const uint32_t *a)
{
    uint32_t top = cg->ftop;
    double r;

    wc_call(cg, fn, sp, ret, n, a);
    r = cg->st[cg->ftop & 7u];
    cg->ftop = top;
    return r;
}

/* CRT sin (0x494272): float argument on the stack, result in st0.
 * The caller runs this per darkened vertex, and both natives involved (0x494272
 * then CIsin 0x49c7f0) are already hand-written, so take the same path inline:
 * d = (double)degrees * (double)pi/180 is NOT rounded to float, and the guards are
 * the ones those natives use (control word with double precision / nearest /
 * masked, finite argument below 2^63). Anything else runs the guest chain. */
static double wc_sin(cpu_t *cg, gptr sp, float x)
{
    uint32_t args[1];
    if ((cg->fcw & 0x0f3fu) == 0x023fu) {
        float k;
        uint32_t u = gmem_rd32_l(g_gmem, g_gmem_size, 0x004a8e90u);
        memcpy(&k, &u, 4);
        {
            double d = (double)x * (double)k;
            if (isfinite(d) && fabs(d) < 9.2e18) return sin(d);
        }
    }
    memcpy(&args[0], &x, 4);
    return wc_call_x87(cg, wmain_00494272, sp, 0x0041a062u, 1, args);
}

/* Direct cores (phase B). Same arithmetic as the hand-written wrappers in
 * native_overrides.c (0x47e34d, 0x4074d4), which are verified against the lifted
 * reference; no guest stack, no cpu_t. */

/* 0x47e34d(out, r, M): out[i] = r0*M[i] + r1*M[3+i] + r2*M[6+i] + M[9+i], in double */
static inline void wc_core_xform(uint8_t *gm, uint32_t gsz, gptr out, gptr rp, gptr mp)
{
    double r0 = wld_f(gm, gsz, rp), r1 = wld_f(gm, gsz, rp + 4u), r2 = wld_f(gm, gsz, rp + 8u);
    uint32_t i;
    for (i = 0; i < 3; i++) {
        double p0 = (double)wld_f(gm, gsz, mp + 4u * i) * r0;
        double p1 = (double)wld_f(gm, gsz, mp + 4u * (3u + i)) * r1;
        double acc = p0 + p1;
        double p2 = (double)wld_f(gm, gsz, mp + 4u * (6u + i)) * r2;
        acc = acc + p2;
        acc = acc + (double)wld_f(gm, gsz, mp + 4u * (9u + i));
        wst_f(gm, gsz, out + 4u * i, (float)acc);
    }
}

/* 0x4074d4(p): 0xFF000000 | clamp(p[0])<<16 | clamp(p[1])<<8 | clamp(p[2]), via fistp
 * with the control word fcw (same as the wrapper). */
static inline uint32_t wc_core_pack(uint8_t *gm, uint32_t gsz, gptr p, uint16_t fcw)
{
    uint32_t packed = 0xff000000u, k;
    for (k = 0; k < 3; k++) {
        /* k = 0 reads p[2] into bits 0..7, k = 1 reads p[1] into 8..15, k = 2 reads p[0] into 16..23 */
        float f = wld_f(gm, gsz, p + 4u * (2u - k));
        int32_t iv = (int32_t)cpu_fist32(fcw, (double)f);
        if (iv < 0) iv = 0;
        else if (iv > 0xff) iv = 0xff;
        packed |= (uint32_t)iv << (8u * k);
    }
    return packed;
}

/* 0x4075c6(a, out, s): material floats for one vertex. out[0..3] = s*255, then up to
 * three material records (0x59dffc, 0x5a5ffc, 0x5adffc tables, 128-byte rows) add
 * their terms to out. Same arithmetic as the verified wrapper in native_overrides.c.
 * s is a float value (the caller passes the bits of [w+0x60]). No calls, no flags. */
static inline void wc_core_mat(uint8_t *gm, uint32_t gsz, gptr a, gptr out, float s)
{
    double fs7;
    float tmp;
    uint32_t sel;

#define MT_LD(x) wld_f(gm, gsz, (x))
    fs7 = (double)s * (double)MT_LD(0x4a8090u);
    wst_f(gm, gsz, out, (float)fs7);
    gmem_wr32_l(gm, gsz, out + 4u, gmem_rd32_l(gm, gsz, out));
    gmem_wr32_l(gm, gsz, out + 8u, gmem_rd32_l(gm, gsz, out));
    gmem_wr32_l(gm, gsz, out + 0xcu, gmem_rd32_l(gm, gsz, out));

    /* block 1: selector a[0x1c], compare a[0x1e], scale a[0x10], table 0x59dffc */
    if (gmem_rd8_l(gm, gsz, a + 0x1cu) != 0u) {
        sel = gmem_rd8_l(gm, gsz, a + 0x1cu);
        if (gmem_rd32_l(gm, gsz, (sel << 7) + 0x59e034u) == 1u &&
            gmem_rd32_l(gm, gsz, (sel << 7) + 0x59e02cu) == gmem_rd8_l(gm, gsz, a + 0x1eu)) {
            fs7 = MT_LD((sel << 7) + 0x59e03cu) * (double)MT_LD(a + 0x10u);
            tmp = (float)fs7;
            fs7 = (double)MT_LD(out + 0xcu) + (double)tmp;
            wst_f(gm, gsz, out + 0xcu, (float)fs7);
            fs7 = MT_LD((sel << 7) + 0x59dffcu) * (double)tmp;
            fs7 = fs7 + (double)MT_LD(out);
            wst_f(gm, gsz, out, (float)fs7);
            fs7 = MT_LD((sel << 7) + 0x59e000u) * (double)tmp;
            fs7 = fs7 + (double)MT_LD(out + 4u);
            wst_f(gm, gsz, out + 4u, (float)fs7);
            fs7 = MT_LD((sel << 7) + 0x59e004u) * (double)tmp;
            fs7 = fs7 + (double)MT_LD(out + 8u);
            wst_f(gm, gsz, out + 8u, (float)fs7);
        } else {
            gmem_wr8_l(gm, gsz, a + 0x1cu, 0u);
        }
    }

    /* block 2: selector a[0x1d], compare a[0x1f], scale a[0x14], table 0x5a5ffc */
    if (gmem_rd8_l(gm, gsz, a + 0x1du) != 0u) {
        sel = gmem_rd8_l(gm, gsz, a + 0x1du);
        if (gmem_rd32_l(gm, gsz, (sel << 7) + 0x5a6034u) == 1u &&
            gmem_rd32_l(gm, gsz, (sel << 7) + 0x5a602cu) == gmem_rd8_l(gm, gsz, a + 0x1fu)) {
            fs7 = MT_LD((sel << 7) + 0x5a603cu) * (double)MT_LD(a + 0x14u);
            tmp = (float)fs7;
            fs7 = (double)MT_LD(out + 0xcu) + (double)tmp;
            wst_f(gm, gsz, out + 0xcu, (float)fs7);
            fs7 = MT_LD((sel << 7) + 0x5a5ffcu) * (double)tmp;
            fs7 = fs7 + (double)MT_LD(out);
            wst_f(gm, gsz, out, (float)fs7);
            fs7 = MT_LD((sel << 7) + 0x5a6000u) * (double)tmp;
            fs7 = fs7 + (double)MT_LD(out + 4u);
            wst_f(gm, gsz, out + 4u, (float)fs7);
            fs7 = MT_LD((sel << 7) + 0x5a6004u) * (double)tmp;
            fs7 = fs7 + (double)MT_LD(out + 8u);
            wst_f(gm, gsz, out + 8u, (float)fs7);
        } else {
            gmem_wr8_l(gm, gsz, a + 0x1du, 0u);
        }
    }

    /* block 3: selector a[0xc], value a[0x18], table 0x5adffc (no per-record compare) */
    if (gmem_rd8_l(gm, gsz, a + 0xcu) != 0u) {
        sel = gmem_rd8_l(gm, gsz, a + 0xcu);
        if (gmem_rd32_l(gm, gsz, (sel << 7) + 0x5ae034u) == 1u) {
            tmp = MT_LD(a + 0x18u);
            fs7 = (double)MT_LD(out + 0xcu) + (double)tmp;
            wst_f(gm, gsz, out + 0xcu, (float)fs7);
            fs7 = MT_LD((sel << 7) + 0x5adffcu) * (double)tmp;
            fs7 = fs7 + (double)MT_LD(out);
            wst_f(gm, gsz, out, (float)fs7);
            fs7 = MT_LD((sel << 7) + 0x5ae000u) * (double)tmp;
            fs7 = fs7 + (double)MT_LD(out + 4u);
            wst_f(gm, gsz, out + 4u, (float)fs7);
            fs7 = MT_LD((sel << 7) + 0x5ae004u) * (double)tmp;
            fs7 = fs7 + (double)MT_LD(out + 8u);
            wst_f(gm, gsz, out + 8u, (float)fs7);
        } else {
            gmem_wr8_l(gm, gsz, a + 0xcu, 0u);
        }
    }
#undef MT_LD
}

/* 0x41af30(src, dst): sprite/light descriptor of one vertex. Same arithmetic and FCOM
 * condition codes as the verified wrapper in native_overrides.c; the x87 stack and the
 * register results are dead for the caller, so only memory and cg->fsw are updated. */
static inline void wc_core_af30(cpu_t *cg, uint8_t *gm, uint32_t gsz, gptr src, gptr dst)
{
    double fs5, fs6, fs7, q;

#define AF_LD(a) wld_f(gm, gsz, (a))
    fs7 = (double)AF_LD(src + 4u);
    FCOM(cg, fs7, (double)AF_LD(K_ZERO));
    if (cg->fsw & 0x4100u) {                     /* out of range: marker byte only */
        gmem_wr8_l(gm, gsz, dst, 0x80u);
        return;
    }

    q = (double)AF_LD(0x5bf9e8u);
    q = q / (double)AF_LD(src + 4u);
    fs6 = (double)AF_LD(src);
    fs6 = fs6 * q;
    fs6 = fs6 + (double)AF_LD(0x6bfa1cu);
    wst_f(gm, gsz, dst + 0x10u, (float)fs6);
    q = q * (double)AF_LD(src + 8u);
    q = -q;
    q = q + (double)AF_LD(0x5bb580u);
    fs7 = q;
    wst_f(gm, gsz, dst + 0x14u, (float)fs7);
    gmem_wr32_l(gm, gsz, dst + 8u, 0xff000000u);

    if (gmem_rd32_l(gm, gsz, 0x5bb5bcu) != 0u) {
        fs7 = (double)AF_LD(0x5bb614u);
        FCOM(cg, fs7, (double)AF_LD(src + 4u));
        if (cg->fsw & 0x0100u) {
            gmem_wr32_l(gm, gsz, dst + 8u, 0u);
            fs7 = (double)AF_LD(0x5bb5fcu);
            FCOM(cg, fs7, (double)AF_LD(src + 4u));
            if (!(cg->fsw & 0x0100u)) {
                fs7 = (double)AF_LD(src + 4u);
                fs7 = fs7 - (double)AF_LD(0x5bb614u);
                fs7 = fs7 / (double)AF_LD(0x5bb60cu);
                fs7 = fs7 * (double)AF_LD(0x5bb578u);
                fs7 = -fs7;
                fs7 = fs7 + (double)AF_LD(0x5bb578u);
                gmem_wr32_l(gm, gsz, dst + 8u, cpu_fist32(cg->fcw, fs7) << 24);
            }
        }
    }

    if (gmem_rd32_l(gm, gsz, 0x6bfa14u) != 0u) {
        fs7 = (double)AF_LD(src + 4u);
        fs6 = (double)AF_LD(dst + 0x14u);
        fs5 = (double)AF_LD(dst + 0x10u);
        gmem_wr8_l(gm, gsz, dst, 0u);
        FCOM(cg, fs5, (double)AF_LD(0x5bf9d4u));
        if (cg->fsw & 0x0100u) gmem_wr8_l(gm, gsz, dst, (uint8_t)(gmem_rd8_l(gm, gsz, dst) | 1u));
        else {
            FCOM(cg, fs5, (double)AF_LD(0x5bb600u));
            if (!(cg->fsw & 0x4100u)) gmem_wr8_l(gm, gsz, dst, (uint8_t)(gmem_rd8_l(gm, gsz, dst) | 2u));
        }
        FCOM(cg, fs6, (double)AF_LD(0x5bf9d8u));
        if (cg->fsw & 0x0100u) gmem_wr8_l(gm, gsz, dst, (uint8_t)(gmem_rd8_l(gm, gsz, dst) | 8u));
        else {
            FCOM(cg, fs6, (double)AF_LD(0x5bb604u));
            if (!(cg->fsw & 0x4100u)) gmem_wr8_l(gm, gsz, dst, (uint8_t)(gmem_rd8_l(gm, gsz, dst) | 4u));
        }
        FCOM(cg, fs7, (double)AF_LD(0x5bb5f8u));
        if (cg->fsw & 0x0100u) gmem_wr8_l(gm, gsz, dst, (uint8_t)(gmem_rd8_l(gm, gsz, dst) | 0x10u));
    }

    fs7 = 1.0;
    fs7 = fs7 / (double)AF_LD(src + 4u);
    wst_f(gm, gsz, dst + 0x18u, (float)fs7);
    fs7 = fs7 * (double)AF_LD(0x5bb5b0u);
    if (gmem_rd32_l(gm, gsz, 0x866fc8u) != 0x10u) {
        fs7 = -fs7;
        fs6 = 1.0;
        fs7 = fs7 + fs6;
        wst_f(gm, gsz, dst + 0x1cu, (float)fs6);
    }
    wst_f(gm, gsz, dst + 0x1cu, (float)fs7);
#undef AF_LD
}

/* ---- adaptive draw distance (compromise for heavy scenes, docs/PLAN.md 46j) ----
 * The cost of a frame in a big fight is dominated by the world polygons (walk, transform, projection, draw calls). When the
 * frames stay over the 30 fps budget, world polygons (not objects/billboards) whose first vertex is farther than a radius
 * from the camera are dropped; the radius is a fraction of the farthest polygon distance seen and shrinks until the frames are
 * on time again (never below g_wc_lod_min), then slowly grows back. Light scenes never leave fraction 1.0. */
int   g_wc_lod_enable = 0;           /* main.c: on unless nolod.txt / 60 fps mode */
int   g_wc_lod_fix = 0;              /* debug: fixed fraction in percent (lodfix.txt) */
int   g_wc_lod_min = 30;             /* floor in percent of the farthest distance */
static float    g_lod_r2;            /* 0: off */
static float    g_lod_max2;          /* farthest squared distance among the polygons walked this frame */
static float    g_lod_frac = 1.0f;
static uint32_t g_lod_killed, g_lod_frame;
static uint64_t g_lod_last_us;
static uint32_t g_lod_dt[6], g_lod_head[6], g_lod_over, g_lod_n;
extern uint64_t g_sleep0_spin_us;
static float    g_lod_ref2;          /* farthest squared distance of the last measured frame */

static void wc_lod_update(void)
{
    if (!g_wc_lod_enable) return;               /* (pal_time_us advances the host's fake clock: do not call it when off) */
    uint64_t now = pal_time_us();
    uint32_t dt = (uint32_t)(now - g_lod_last_us);
    g_lod_last_us = now;
    g_lod_frame++;
    if (g_lod_max2 > 0.0f) g_lod_ref2 = g_lod_max2;
    g_lod_max2 = 0.0f;
    if (dt > 250000u) {                                       /* load / menu gap: start over */
        g_lod_frac = 1.0f; g_lod_r2 = 0.0f; for (uint32_t j = 0; j < 6; j++) { g_lod_dt[j] = 33333u; g_lod_head[j] = 0; } g_lod_killed = 0; g_sleep0_spin_us = 0;
        return;
    }
    /* headroom = what the game's limiter had to wait (it spins on Sleep(0)); 0 when the frame's work alone took 33.3 ms or more */
    uint32_t head = g_sleep0_spin_us > 100000u ? 100000u : (uint32_t)g_sleep0_spin_us;
    g_sleep0_spin_us = 0;
    g_lod_dt[g_lod_frame % 6u] = dt;
    g_lod_head[g_lod_frame % 6u] = head;
    g_lod_n++;
    if (head < 500u) g_lod_over++;
    uint32_t minh = 0xffffffffu, sum4 = 0, i;
    for (i = 0; i < 6; i++) if (g_lod_head[i] < minh) minh = g_lod_head[i];
    for (i = 0; i < 4; i++) sum4 += g_lod_head[(g_lod_frame + 6u - i) % 6u];
    if (g_wc_lod_fix) g_lod_frac = (float)g_wc_lod_fix / 100.0f;
    else if (sum4 < 4u * 700u && g_lod_frac > (float)g_wc_lod_min / 100.0f) {
        g_lod_frac *= 0.97f;                                  /* no headroom over the last four frames: draw less */
        if (g_lod_frac < (float)g_wc_lod_min / 100.0f) g_lod_frac = (float)g_wc_lod_min / 100.0f;
    } else if (minh > 2500u && g_lod_frac < 1.0f) {
        g_lod_frac *= 1.004f;                                 /* 2.5 ms of headroom in each of six frames: give the distance back, slowly */
        if (g_lod_frac > 1.0f) g_lod_frac = 1.0f;
    }
    if (g_lod_frac >= 0.999f || g_lod_ref2 <= 0.0f) g_lod_r2 = 0.0f;
    else g_lod_r2 = g_lod_ref2 * g_lod_frac * g_lod_frac;
    if (g_lod_frame % 150u == 0 && (g_lod_frac < 0.999f || g_lod_killed))
        pal_log("lod: distance %.0f%% (%.0f units), %lu world polygons dropped per frame, %lu of %lu frames without headroom", (double)(g_lod_frac * 100.0f),
                (double)sqrtf(g_lod_r2 > 0.0f ? g_lod_r2 : g_lod_ref2), (unsigned long)(g_lod_killed / 150u), (unsigned long)g_lod_over, (unsigned long)g_lod_n);
    if (g_lod_frame % 150u == 0) { g_lod_killed = 0; g_lod_over = 0; g_lod_n = 0; }
}

static void wc_native(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    const gptr F = E - 0xf8u;                     /* body ESP after the prologue */
    const gptr w = gmem_rd32_l(gm, gsz, E + 4u);  /* world argument */
    const gptr phase = gmem_rd32_l(gm, gsz, w + 0xa0u);
    const double kZero = wld_f(gm, gsz, K_ZERO), kHalf = wld_f(gm, gsz, K_HALF);
    const double k255 = wld_f(gm, gsz, K_255), kM1 = wld_f(gm, gsz, K_M1);
    const double kInv64 = wld_f(gm, gsz, K_INV64), k240 = wld_f(gm, gsz, K_240);
    const double kMQ25 = wld_f(gm, gsz, K_MQ25), k180 = wld_f(gm, gsz, K_180);
    uint32_t mask, m, b, k, n, pf, lm, bb, andc, orc, animTex, tex, cnt, i;
    gptr node, p, vtx, vs;
    gptr vp[4];
    uint32_t ci[4];
    double side;
    float yoff_f;
    int front;
    uint32_t packed = 0;

    wc_lod_update();

#define LD32(a)    gmem_rd32_l(gm, gsz, (a))
#define LD16(a)    gmem_rd16_l(gm, gsz, (a))
#define LD8(a)     gmem_rd8_l(gm, gsz, (a))
#define WR32(a, v) gmem_wr32_l(gm, gsz, (a), (v))
#define WR16(a, v) gmem_wr16_l(gm, gsz, (a), (v))
#define WR8(a, v)  gmem_wr8_l(gm, gsz, (a), (v))
#define LDF(a)     wld_f(gm, gsz, (a))
#define STF(a, f)  wst_f(gm, gsz, (a), (f))

    /* 0x4199b6..0x419a26: render toggles -> mask */
    m = 0;
    if (LD32(G_TOG_A) == 0) m = 4;
    if (LD32(G_TOG_B) == 0) m |= 1;
    if (LD32(G_TOG_C) == 0) m |= 0x800;
    mask = ~m;

    /* view matrix copy (rep movsd, 12 dwords) to F+0xc8 */
    vs = LD32(G_VIEWP) + 8u;
    for (i = 0; i < 12; i++) WR32(F + 0xc8u + 4u * i, LD32(vs + 4u * i));

    WR32(G_OBJ_CUR, 0xffu);
    WR32(G_OBJ_ACT, 0u);
    WR32(G_ONE, 1u);

    if ((int32_t)LD32(w + 0xbcu) < 0) goto epilogue;

    for (b = 0; (int32_t)b <= (int32_t)LD32(w + 0xbcu); b++) {
        node = LD32(G_BUCKETS + 4u * b);
        if (!node) continue;

        do {
            /* ---- one polygon node (listing 0x419a53) ---- */
            prof_sub(PROF_SUB_WC_NODE);
            p = LD32(node);
            n = LD8(p + 0x25u);
            if (n < 3) { BR(BR_SHORT_KILL); goto kill; }

            pf = ((uint32_t)LD16(p + 0x2cu) | LD32(G_ORFLAGS)) & mask;
            lm = LD8(p + 0x2eu);
            if (LD8(p + 0x2bu) == 0) lm &= 0xfbu;

            if (LD8(p + 0x28u) & 4u) {
                /* object polygon (listing 0x419aa3) */
                uint32_t sub_idx;
                if (LD8(p + 0x2au) != LD8(G_OBJ_CUR)) {
                    gptr o = LD32(w + 0x624u + 4u * LD8(p + 0x2au));
                    BR(BR_OBJ_CHANGE);
                    WR32(G_OBJ_ACT, 0u);
                    WR32(G_OBJ_PTR, o);
                    WR32(G_OBJ_CUR, LD8(p + 0x2au));
                    WR32(G_SUB_CUR, 0xffu);
                    if (o != 0 && LD32(o) != 0) WR32(G_OBJ_ACT, 1u);
                }

                if (LD32(G_OBJ_ACT) != 0) {
                    sub_idx = LD8(p + 0x29u);
                    if (sub_idx != LD8(G_SUB_CUR)) {
                        /* animate the object, then rebuild its matrices (listing 0x419b27) */
                        gptr sub, view;
                        uint32_t a[3];
                        BR(BR_SUB_SWITCH);
                        a[0] = LD32(G_OBJ_PTR);
                        a[1] = LD32(w + 0x54u);
                        wc_call(cg, wmain_00409170, F, 0x00419b31u, 2, a);

                        WR32(G_SUB_CUR, sub_idx);
                        sub = LD32(G_OBJ_PTR) + sub_idx * 0x9cu + 0x84u;
                        WR32(G_SUB_PTR, sub);
                        view = LD32(G_VIEWP) + 8u;
                        a[0] = G_O2C; a[1] = view; a[2] = sub + 0x14u;
                        wc_call(cg, wmain_0047dad6, F, 0x00419b6au, 3, a);
                        a[0] = G_INV; a[1] = sub + 0x14u;
                        wc_call(cg, wmain_0047ceb9, F, 0x00419b81u, 2, a);
                        WR32(G_CAMP, LD32(G_CAMSRC));
                        WR32(G_CAMP + 4u, LD32(G_CAMSRC + 4u));
                        WR32(G_CAMP + 8u, LD32(G_CAMSRC + 8u));
                        wc_core_xform(gm, gsz, G_CAMOBJ, G_CAMP, G_INV);
                    }
                } else {
                    BR(BR_OBJ_INACTIVE);
                }
            }

            if (LD8(p + 0x28u) & 0x10u) { BR(BR_NODE_HIDDEN_KILL); goto kill; }

            vtx = LD32(w + 0x9cu);
            vp[0] = vtx + 32u * LD16(p + 0u);
            vp[1] = vtx + 32u * LD16(p + 2u);
            vp[2] = vtx + 32u * LD16(p + 4u);
            WR32(F + 0x68u, vp[0]);
            WR32(F + 0x6cu, vp[1]);
            WR32(F + 0x70u, vp[2]);
            if (n == 4) {
                vp[3] = vtx + 32u * LD16(p + 6u);
                WR32(F + 0x74u, vp[3]);
            }

            bb = pf & 8u;                 /* billboard / sprite polygon */
            if (bb) BR(BR_BILLBOARD);
            if (!bb && g_wc_lod_enable && !(LD8(p + 0x28u) & 4u)) {   /* world polygon: adaptive draw distance */
                gptr cam = LD32(G_CAM);
                gptr v0 = vp[0];
                float dx = LDF(v0) - LDF(cam + 0x24u), dy = LDF(v0 + 4u) - LDF(cam + 0x28u), dz = LDF(v0 + 8u) - LDF(cam + 0x2cu);
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > g_lod_max2) g_lod_max2 = d2;
                if (g_lod_r2 > 0.0f && d2 > g_lod_r2) {
                    /* drop it only when every corner is beyond the radius (a big floor polygon with one near corner stays) */
                    uint32_t kk;
                    for (kk = 1; kk < n; kk++) {
                        gptr vk = vp[kk];
                        float ex = LDF(vk) - LDF(cam + 0x24u), ey = LDF(vk + 4u) - LDF(cam + 0x28u), ez = LDF(vk + 8u) - LDF(cam + 0x2cu);
                        float e2 = ex * ex + ey * ey + ez * ez;
                        if (e2 < d2 ) d2 = e2;
                    }
                    if (d2 > g_lod_r2) { g_lod_killed++; goto kill; }
                }
            }
            side = kM1;                   /* x87 -1.0 loaded at 0x419c07 */

            if (!bb && (!(LD8(p + 0x27u) & 0x80u) || (pf & 2u))) {
                /* direction test (listing 0x419c49) */
                int use_obj = LD32(G_OBJ_ACT) != 0 && (LD8(p + 0x28u) & 4u);
                gptr v0 = vp[0];
                double nx, ny, nz, dx, dy, dz;
                if (use_obj) {
                    gptr pv = LD32(G_SUB_PTR);
                    BR(BR_SIDE_OBJ);
                    /* d = f32(f32(v - camobj) - pivot) per component */
                    STF(G_SCR, (float)((double)(float)((double)LDF(v0) - (double)LDF(G_CAMOBJ)) - (double)LDF(pv + 8u)));
                    STF(G_SCR + 4u, (float)((double)(float)((double)LDF(v0 + 4u) - (double)LDF(G_CAMOBJ + 4u)) - (double)LDF(pv + 0xcu)));
                    STF(G_SCR + 8u, (float)((double)(float)((double)LDF(v0 + 8u) - (double)LDF(G_CAMOBJ + 8u)) - (double)LDF(pv + 0x10u)));
                } else {
                    gptr cam = LD32(G_CAM);
                    BR(BR_SIDE_WORLD);
                    STF(G_SCR, (float)((double)LDF(v0) - (double)LDF(cam + 0x24u)));
                    STF(G_SCR + 4u, (float)((double)LDF(v0 + 4u) - (double)LDF(cam + 0x28u)));
                    STF(G_SCR + 8u, (float)((double)LDF(v0 + 8u) - (double)LDF(cam + 0x2cu)));
                }
                nx = LDF(p + 0x10u); ny = LDF(p + 0x14u); nz = LDF(p + 0x18u);
                dx = LDF(G_SCR); dy = LDF(G_SCR + 4u); dz = LDF(G_SCR + 8u);
                side = (nx * dx + ny * dy) + nz * dz;
                FCOM(cg, side, kZero);       /* listing 0x419d11 */
                if (side != side) BR(BR_SIDE_NAN);
                if (!(side < 0.0) && !(pf & 2u)) {
                    BR(BR_BACKFACE_KILL);    /* single-sided, facing away (0x41a303) */
                    goto kill;
                }
                if (side >= 0.0) BR(BR_SIDE_2SIDED_KEEP);
            }
            FCOM(cg, side, kZero);           /* listing 0x419d27: fcomp [0.0] */
            front = !(side > 0.0);           /* side <= 0 or unordered: front texture */

            {
                int32_t t = front ? (int32_t)(int16_t)LD16(p + 0x20u) : (int32_t)LD16(p + 0x22u);
                if (front) BR(BR_FRONT_TEX); else BR(BR_BACK_TEX);
                if (t < 0 || t >= (int32_t)LD32(w + 0xb0u)) { BR(BR_TEX_RANGE_KILL); goto kill; }
                tex = (uint32_t)t;
            }

            if ((uint32_t)(int32_t)(int16_t)LD16(p + 8u) == tex) {
                animTex = 0;
            } else {
                animTex = LD8(LD32(w + 0xb8u) + tex * 40u + 0x24u) & 8u;
                if (animTex) {
                    BR(BR_TEX_ANIM_CHANGE);
                    WR8(p + 0xfu, (uint8_t)(LD8(p + 0xfu) | 0x40u));
                } else {
                    BR(BR_TEX_STATIC_CHANGE);
                    WR8(p + 0xfu, (uint8_t)(LD8(p + 0xfu) & 0xbfu));
                }
            }
            WR16(p + 8u, (uint16_t)tex);

            if (pf & 0x30u) {
                uint32_t a[2];
                BR(BR_TEX_FRAME_CALL);
                a[0] = w; a[1] = p;
                wc_call(cg, wmain_0041ae50, F, 0x00419d9du, 2, a);
            }
            if (bb) {
                uint32_t a[4];
                a[0] = w; a[1] = p; a[2] = F + 0x68u; a[3] = F + 0x98u;
                wc_call(cg, wmain_0041a350, F, 0x00419db8u, 4, a);
            }

            /* y offset (listing 0x419dbb): int8 / 64, exact */
            {
                int8_t yo = (int8_t)LD8(p + 0x36u);
                if (yo != 0) { BR(BR_YOFF); yoff_f = (float)((double)yo * kInv64); }
                else yoff_f = 0.0f;
            }
            andc = 0x3fu;
            orc = 0u;

            for (k = 0; k < n; k++) {
                gptr v = vp[k];
                uint32_t idx = LD16(p + 2u * k);
                int cached = LD8(v + 0xdu) != 0;
                gptr s;

                if (cached) {
                    /* listing 0x41a0f3 */
                    BR(BR_CACHED_VERTEX);
                    ci[k] = LD16(v + 0xeu);
                    s = G_CACHE + ci[k] * 64u;
                } else {
                    ci[k] = LD32(G_CACHECNT);
                    s = G_CACHE + ci[k] * 64u;

                    /* vertex copy on the frame (listing 0x419e26) */
                    WR32(F + 0x4cu, LD32(v));
                    WR32(F + 0x50u, LD32(v + 4u));
                    WR32(F + 0x54u, LD32(v + 8u));

                    if (bb) {
                        /* 0x419e46: billboard corner from the quad array */
                        gptr q = F + 0x98u + 12u * k;
                        BR(BR_BB_QUAD);
                        WR32(G_VT, LD32(q));
                        WR32(G_VT + 4u, LD32(q + 4u));
                        WR32(G_VT + 8u, LD32(q + 8u));
                    } else if (LD32(G_OBJ_ACT) != 0 && (LD8(p + 0x28u) & 4u)) {
                        /* 0x419e74: object space, d = f32(v - pivot), transform by object->camera */
                        gptr pv = LD32(G_SUB_PTR);
                        BR(BR_OBJ_XFORM);
                        STF(G_SCR, (float)((double)LDF(F + 0x4cu) - (double)LDF(pv + 8u)));
                        STF(G_SCR + 4u, (float)((double)LDF(F + 0x50u) - (double)LDF(pv + 0xcu)));
                        STF(G_SCR + 8u, (float)((double)LDF(F + 0x54u) - (double)LDF(pv + 0x10u)));
                        prof_sub(PROF_SUB_WC_XFORM); wc_core_xform(gm, gsz, G_VT, G_SCR, G_O2C); prof_sub(PROF_SUB_WC_NODE);
                    } else {
                        /* 0x419eb3: world space, transform the copy by the view matrix */
                        BR(BR_WORLD_XFORM);
                        prof_sub(PROF_SUB_WC_XFORM); wc_core_xform(gm, gsz, G_VT, F + 0x4cu, F + 0xc8u); prof_sub(PROF_SUB_WC_NODE);
                    }

                    if (lm & 3u) {
                        uint32_t a[3];
                        BR(BR_WATER_CALL);
                        a[0] = v; a[1] = G_VT; a[2] = p;
                        wc_call(cg, wmain_00428615, F, 0x00419ee2u, 3, a);
                    }

                    if (pf & 4u) {
                        /* depth-biased projection; keep the uv of the unbiased one (0x419eec) */
                        uint32_t bias_bits = LD32(G_BIAS);
                        uint32_t a[2];
                        float sv18, sv1c;
                        double r;
                        BR(BR_DEPTH_BIAS);
                        prof_sub(PROF_SUB_WC_PROJ); wc_core_af30(cg, gm, gsz, G_VT, s); prof_sub(PROF_SUB_WC_NODE);
                        sv18 = LDF(s + 0x18u);
                        sv1c = LDF(s + 0x1cu);
                        a[0] = v; a[1] = LD32(G_CAM) + 0x24u;
                        r = wc_call_x87(cg, wmain_00427c9a, F, 0x00419f21u, 2, a);
                        STF(G_BIAS, (float)(r + (double)LDF(G_BIAS)));
                        prof_sub(PROF_SUB_WC_PROJ); wc_core_af30(cg, gm, gsz, G_VT, s); prof_sub(PROF_SUB_WC_NODE);
                        STF(s + 0x18u, sv18);
                        STF(s + 0x1cu, sv1c);
                        WR32(G_BIAS, bias_bits);
                    } else {
                        /* 0x419f55 */
                        uint32_t a[2];
                        STF(G_VT + 4u, (float)((double)yoff_f + (double)LDF(G_VT + 4u)));
                        prof_sub(PROF_SUB_WC_PROJ); wc_core_af30(cg, gm, gsz, G_VT, s); prof_sub(PROF_SUB_WC_NODE);
                    }
                    WR8(s + 1u, 0u);
                    WR32(s + 4u, 0xff000000u);

                    if (LD32(G_LIGHT) != 0 && (lm & 4u)) {
                        /* lighting (listing 0x419f94) */
                        gptr cam = LD32(G_CAM);
                        double dx, dy, dz, dot, len2, r;
                        BR(BR_LIGHT);
                        STF(G_SCR, (float)((double)LDF(v) - (double)LDF(cam + 0x24u)));
                        STF(G_SCR + 4u, (float)((double)LDF(v + 4u) - (double)LDF(cam + 0x28u)));
                        STF(G_SCR + 8u, (float)((double)LDF(v + 8u) - (double)LDF(cam + 0x2cu)));
                        dx = LDF(G_SCR); dy = LDF(G_SCR + 4u); dz = LDF(G_SCR + 8u);
                        dot = (LDF(p + 0x10u) * dx + LDF(p + 0x14u) * dy) + LDF(p + 0x18u) * dz;
                        len2 = (dx * dx + dy * dy) + dz * dz;
                        r = dot / sqrt(len2);
                        FCOM(cg, r, kZero);
                        if (r < 0.0 || r != r) STF(s + 0x20u, (float)(-r));
                        else                   STF(s + 0x20u, (float)r);

                        if (LD8(p + 0xau) != 0) {
                            /* darkening (listing 0x41a036): sin argument rounded to float */
                            double arg, sn, x;
                            BR(BR_DARK);
                            arg = ((double)LDF(phase + 4u * idx) + (double)LDF(G_TIME)) * k240;
                            sn = wc_sin(cg, F, (float)arg);
                            x = (double)LD8(p + 0xau) * ((sn - kM1) * kHalf);
                            FCOM(cg, x, kZero);
                            if (x < 0.0 || x != x) x = kZero;
                            else {
                                FCOM(cg, x, k255);
                                if (x > k255) x = k255;
                            }
                            WR8(s + 7u, (uint8_t)wc_ftol(x));
                        }
                    }

                    /* 0x41a0b8: new cache slot */
                    WR8(v + 0xdu, 1u);
                    WR16(v + 0xeu, (uint16_t)ci[k]);
                    WR16(s + 2u, (uint16_t)idx);
                    cnt = ci[k] + 1u;
                    WR32(G_CACHECNT, cnt);
                    if (cnt >= 0x4000u) { BR(BR_CACHE_FULL); goto epilogue; }
                }

                /* 0x41a104: clip code of the slot */
                {
                    uint8_t clip = LD8(s);
                    andc &= clip;
                    orc |= clip;
                }
            }

            if (andc & 0xffu) {                         /* 0x41a2fb */
                BR(BR_ANDC_KILL);
                WR32(G_CULLCNT, LD32(G_CULLCNT) + 1u);
                goto kill;
            }
            if (bb && (orc & 0x80u)) { BR(BR_BB_OR80_KILL); goto kill; }
            WR32(node + 8u, orc & 0xffu);
            if (animTex) { BR(BR_ANIMTEX_SKIP); goto next; }

            /* colour pass (listing 0x41a185): 0x4075c6 then 0x4074d4 per slot */
            for (k = 0; k < n; k++) {
                gptr s = G_CACHE + ci[k] * 64u;
                if (LD8(s + 1u) != 0) continue;
                {
                    BR(BR_COLOUR);
                    prof_sub(PROF_SUB_WC_COLOUR);
                    wc_core_mat(gm, gsz, vp[k], F + 0x88u, LDF(w + 0x60u));
                    packed = wc_core_pack(gm, gsz, F + 0x88u, cg->fcw);
                    prof_sub(PROF_SUB_WC_NODE);
                }
                WR32(s + 4u, LD32(s + 4u) | (packed & 0xffffffu));
            }

            if (pf & 0x800u) {
                /* darkening (listing 0x41a20a) */
                for (k = 0; k < n; k++) {
                    gptr s = G_CACHE + ci[k] * 64u;
                    uint32_t idx = LD16(p + 2u * k);
                    double arg, sn, x;
                    uint32_t dk, argb, c0, c1, c2, c3;
                    int32_t d0, d1, d2;
                    if (LD8(s + 1u) != 0) continue;
                    BR(BR_DARKEN_LOOP);
                    arg = ((double)LDF(phase + 4u * idx) + (double)LDF(G_TIME) - kMQ25) * k180;
                    sn = wc_sin(cg, F, (float)arg);
                    x = (double)LD8(p + 0xau) * ((sn - kM1) * kHalf);
                    dk = wc_ftol(x);
                    argb = LD32(s + 4u);
                    c3 = argb >> 24;
                    c2 = (argb >> 16) & 0xffu;
                    c1 = (argb >> 8) & 0xffu;
                    c0 = argb & 0xffu;
                    d2 = (int32_t)c2 - (int32_t)dk;
                    d1 = (int32_t)c1 - (int32_t)dk;
                    d0 = (int32_t)c0 - (int32_t)dk;
                    c2 = d2 < 0 ? 0u : (uint32_t)d2;
                    c1 = d1 < 0 ? 0u : (uint32_t)d1;
                    c0 = d0 < 0 ? 0u : (uint32_t)d0;
                    WR32(s + 4u, (c3 << 24) | (c2 << 16) | (c1 << 8) | c0);
                }
            }

            for (k = 0; k < n; k++) WR8(G_CACHE + ci[k] * 64u + 1u, 1u);   /* 0x41a2dd */

        next:
            node = LD32(node + 4u);
            continue;

        kill:
            WR32(node, 0u);
            node = LD32(node + 4u);
        } while (node);
    }

epilogue:
    prof_sub(PROF_SUB_NONE);
    cg->r[ESP] = E + 4u;
#undef LD32
#undef LD16
#undef LD8
#undef WR32
#undef WR16
#undef WR8
#undef LDF
#undef STF
}

/* ---- shadow check (host only) ---- */
#ifndef __3DS__
static int wc_verify = -1;
static uint64_t wc_checked, wc_differed, wc_diff_subsw, wc_diff_other;

static void wc_report(void)
{
    uint32_t i;
    fprintf(stderr, "world_cull verify: %llu calls checked, %llu differed (sub-switch %llu, other %llu)\n",
            (unsigned long long)wc_checked, (unsigned long long)wc_differed,
            (unsigned long long)wc_diff_subsw, (unsigned long long)wc_diff_other);
    for (i = 0; i < BR_NCOUNT; i++)
        fprintf(stderr, "  br %-18s %llu\n", br_name[i], (unsigned long long)br_hits[i]);
}

static void wc_at_exit(void)
{
    if (wc_checked) wc_report();
}

static void wc_shadow(cpu_t *cg)
{
    const uint32_t sz = g_gmem_size;
    const gptr E = cg->r[ESP];
    const cpu_t c0 = *cg;
    uint64_t sw0;
    int subsw;
    cpu_t cn;
    uint8_t *m0 = malloc(sz), *m1 = malloc(sz);
    uint32_t a, nbad = 0;
    int regs_bad = 0;

    if (!m0 || !m1) { fprintf(stderr, "world_cull verify: out of memory\n"); abort(); }
    memcpy(m0, g_gmem, sz);
    sw0 = br_hits[BR_SUB_SWITCH];
    wc_native(cg);
    subsw = br_hits[BR_SUB_SWITCH] != sw0;
    cn = *cg;
    memcpy(m1, g_gmem, sz);

    /* reference run from the same input; its result stays in place */
    memcpy(g_gmem, m0, sz);
    *cg = c0;
    wmain_004199b0_ref(cg);

    for (a = 0; a < sz; a++) {
        if (a < E) continue;                                       /* dead stack below the entry ESP */
        if (m1[a] != g_gmem[a]) {
            if (nbad < 20)
                fprintf(stderr, "world_cull verify: addr %08x native %02x ref %02x\n", a, m1[a], g_gmem[a]);
            nbad++;
        }
    }
    if (cn.r[EBX] != cg->r[EBX] || cn.r[ESI] != cg->r[ESI] || cn.r[EDI] != cg->r[EDI] ||
        cn.r[EBP] != cg->r[EBP] || cn.r[ESP] != cg->r[ESP] || cn.ftop != cg->ftop ||
        cn.fsw != cg->fsw || cn.fcw != cg->fcw) {
        regs_bad = 1;
        fprintf(stderr, "world_cull verify: regs native esp=%08x ftop=%u fsw=%04x ref esp=%08x ftop=%u fsw=%04x\n",
                cn.r[ESP], cn.ftop, cn.fsw, cg->r[ESP], cg->ftop, cg->fsw);
    }
    wc_checked++;
    if (nbad || regs_bad) { if (subsw) wc_diff_subsw++; else wc_diff_other++; }
    if (nbad || regs_bad) wc_differed++;
    free(m0);
    free(m1);
}
#endif

void wmain_004199b0(cpu_t *cg)
{
#ifdef TPM_FN_TIMERS
    uint64_t t0_ = fn_timer_begin();     /* timer 3 = ' world_cull' in FN_TIMERS */
#endif
#ifndef __3DS__
    if (wc_verify < 0) {
        const char *e = getenv("TPM_VERIFY_WC");
        wc_verify = (e && *e == '1');
        if (wc_verify) atexit(wc_at_exit);
    }
    if (wc_verify) { wc_shadow(cg); return; }
#endif
    wc_native(cg);
#ifdef TPM_FN_TIMERS
    fn_timer_end(3, t0_);
#endif
}
