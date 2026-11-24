/* Native actor vertex lighting, guest 0x0040fc10 (perf phase 14).
 *
 * void __cdecl light(lights**, light_pos*, nlights, normals*, a5, a6, uint32_t *out, nverts)
 * (called from the actor transform 0x40f1e0, ~44 times per frame). Transcribed from the
 * x86 listing: asm order of every x87 operation is kept (the three-term sums are
 * (a+b)+c in the listing's order, not the order of the decompiler's C), values are C
 * doubles with float rounding only where the original stores to a float slot.
 *
 *  1. every live vertex (flag[j] = [0x5b8658 + 4j] != 0): colour accumulator
 *     acc[j] = {ambient x3} at 0x5b6448 + 16j,
 *  2. every light: d = light_pos - camera_object_position (rounded to floats); type 1 uses
 *     a cylinder (dx^2+dy^2 against (R*|dz|)^2), the others a sphere ((dx^2+dy^2)+dz^2
 *     against R^2). Inside (or NaN): d is normalised by the game's own 0x486245, the
 *     attenuation att = (float)(sqrt(d2) * light[0x2c]); every live vertex with
 *     n.d > 0 adds (light[0x30/34/38] - att) * (n.d) to its accumulator when positive,
 *  3. every live vertex: acc *= 0x4a80fc and out[j] = packed colour (0x4074d4's arithmetic).
 *
 * Verification (host, TPM_OVERRIDE_REF): env TPM_VERIFY_NA=1 runs native and the lifted
 * reference on identical input for every call and compares the accumulator/flag windows,
 * the output array, EBX/ESI/EDI/EBP/ESP, ftop and fcw. The reference result is kept.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/prof.h"
#include "pal/pal.h"
#include "core/gmem.h"

void wmain_00486245(cpu_t *c);           /* normalise (hand-written) */
#ifndef __3DS__
void wmain_0040fc10_ref(cpu_t *c);
void wmain_0040efd0_ref(cpu_t *c);
#endif

#define A_FLAGS   0x005b8658u
#define A_ACC     0x005b6448u            /* accumulators, 16 bytes each (x,y,z,unused) */
#define A_AMBIENT 0x005b8448u
#define A_CAMOBJ  0x005b8650u            /* -> object whose +0x58..0x60 is the camera position */
#define K_ZERO    0x004a8100u
#define K_SCALE   0x004a80fcu

static inline float ldf(uint8_t *gm, uint32_t gsz, gptr a) { return gmem_f32_l(gmem_rd32_l(gm, gsz, a)); }
static inline void  stf(uint8_t *gm, uint32_t gsz, gptr a, float f) { gmem_wr32_l(gm, gsz, a, gmem_u32_f(f)); }

static void na_light(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    gptr lights = gmem_rd32_l(gm, gsz, E + 4u);
    gptr lpos = gmem_rd32_l(gm, gsz, E + 8u);
    uint32_t nl = gmem_rd32_l(gm, gsz, E + 0xcu);
    gptr nrm0 = gmem_rd32_l(gm, gsz, E + 0x10u);
    gptr out = gmem_rd32_l(gm, gsz, E + 0x1cu);
    uint32_t n = gmem_rd32_l(gm, gsz, E + 0x20u);
    const gptr S = E - 0x40u;            /* scratch vector for the normaliser (dead stack) */
    const double zero = (double)ldf(gm, gsz, K_ZERO);
    uint32_t j;

    if (n) {
        uint32_t amb = gmem_rd32_l(gm, gsz, A_AMBIENT);
        for (j = 0; j < n; j++)
            if (gmem_rd32_l(gm, gsz, A_FLAGS + 4u * j) != 0u) {
                gmem_wr32_l(gm, gsz, A_ACC + 16u * j, amb);
                gmem_wr32_l(gm, gsz, A_ACC + 16u * j + 4u, amb);
                gmem_wr32_l(gm, gsz, A_ACC + 16u * j + 8u, amb);
            }
    }

    for (; nl; nl--, lights += 4u, lpos += 12u) {
        gptr cam = gmem_rd32_l(gm, gsz, A_CAMOBJ);
        gptr L = gmem_rd32_l(gm, gsz, lights);
        float dx = (float)((double)ldf(gm, gsz, lpos) - (double)ldf(gm, gsz, cam + 0x58u));
        float dy = (float)((double)ldf(gm, gsz, lpos + 4u) - (double)ldf(gm, gsz, cam + 0x5cu));
        float dz = (float)((double)ldf(gm, gsz, lpos + 8u) - (double)ldf(gm, gsz, cam + 0x60u));
        double r = (double)ldf(gm, gsz, L + 0x24u);
        double x2 = (double)dx * (double)dx, y2 = (double)dy * (double)dy;
        double z2 = (double)dz * (double)dz;
        float d2;
        double lim;

        if (gmem_rd32_l(gm, gsz, L) == 1u) {
            d2 = (float)(x2 + y2);
            lim = !((double)dz >= zero) ? r * -(double)dz : r * (double)dz;   /* r*|dz| (NaN: negated) */
        } else {
            d2 = (float)((x2 + y2) + z2);
            lim = r;
        }
        if ((double)d2 >= lim * lim) continue;                /* C0 clear and ordered: outside */

        {
            uint32_t a[1];
            float att;
            double sq;
            gptr sp = E - 0x50u;

            stf(gm, gsz, S, dx); stf(gm, gsz, S + 4u, dy); stf(gm, gsz, S + 8u, dz);
            a[0] = S;
            {   /* guest call: normalise S in place (return value on the x87 stack is dropped) */
                uint32_t top = cg->ftop, k;
                gptr s = sp - 8u;
                gmem_wr32_l(gm, gsz, s + 4u, a[0]);
                gmem_wr32_l(gm, gsz, s, 0x0040fd08u);
                cg->r[ESP] = s;
                wmain_00486245(cg);
                cg->r[ESP] = E;
                cg->ftop = top;
                (void)k;
            }
            dx = ldf(gm, gsz, S); dy = ldf(gm, gsz, S + 4u); dz = ldf(gm, gsz, S + 8u);
            sq = sqrt((double)d2);
            att = (float)(sq * (double)ldf(gm, gsz, L + 0x2cu));

            {
                double c0 = (double)ldf(gm, gsz, L + 0x30u) - (double)att;
                double c1 = (double)ldf(gm, gsz, L + 0x34u) - (double)att;
                double c2 = (double)ldf(gm, gsz, L + 0x38u) - (double)att;
                gptr nrm = nrm0;
                /* a vertex adds (c_k * n.d) only when that product is > 0 and n.d > 0 is required first: with no positive colour
                 * term (or NaN) nothing is ever written, so the whole vertex loop can be skipped (~30 % of the lights) */
                if (!(c0 > zero) && !(c1 > zero) && !(c2 > zero)) continue;
                for (j = 0; j < n; j++, nrm += 12u) {
                    double nd, t;
                    gptr acc;
                    if (gmem_rd32_l(gm, gsz, A_FLAGS + 4u * j) == 0u) continue;
                    {
                        double nx = (double)ldf(gm, gsz, nrm), ny = (double)ldf(gm, gsz, nrm + 4u),
                               nz = (double)ldf(gm, gsz, nrm + 8u);
                        nd = ((nx * (double)dx) + (nz * (double)dz)) + (ny * (double)dy);
                    }
                    if (!(nd > zero)) continue;              /* C0|C3 set or unordered: skip */
                    acc = A_ACC + 16u * j;
                    t = c0 * nd;
                    if (t > zero) stf(gm, gsz, acc, (float)(t + (double)ldf(gm, gsz, acc)));
                    t = c1 * nd;
                    if (t > zero) stf(gm, gsz, acc + 4u, (float)(t + (double)ldf(gm, gsz, acc + 4u)));
                    t = c2 * nd;
                    if (t > zero) stf(gm, gsz, acc + 8u, (float)(t + (double)ldf(gm, gsz, acc + 8u)));
                }
            }
        }
    }

    if (n) {
        const double k = (double)ldf(gm, gsz, K_SCALE);
        const uint16_t fcw = (uint16_t)cg->fcw;
        for (j = 0; j < n; j++) {
            gptr acc = A_ACC + 16u * j;
            uint32_t packed = 0xff000000u, i;
            if (gmem_rd32_l(gm, gsz, A_FLAGS + 4u * j) == 0u) continue;
            stf(gm, gsz, acc, (float)((double)ldf(gm, gsz, acc) * k));
            stf(gm, gsz, acc + 4u, (float)((double)ldf(gm, gsz, acc + 4u) * k));
            stf(gm, gsz, acc + 8u, (float)((double)ldf(gm, gsz, acc + 8u) * k));
            for (i = 0; i < 3; i++) {                        /* p[2] -> bits 0..7, p[1] -> 8..15, p[0] -> 16..23 */
                int32_t iv = (int32_t)cpu_fist32(fcw, (double)ldf(gm, gsz, acc + 4u * (2u - i)));
                if (iv < 0) iv = 0;
                else if (iv > 0xff) iv = 0xff;
                packed |= (uint32_t)iv << (8u * i);
            }
            gmem_wr32_l(gm, gsz, out + 4u * j, packed);
        }
    }
    cg->r[ESP] = E + 4u;                                     /* ret */
}


/* ===== 0x0040efd0: project vertices =====================================================
 * void __cdecl project(float *src, float *dst, int *idx, uint8_t *code, int n, float *M)
 * For each of n entries (src consecutive, or src[(idx[i]-1)] with idx[i]==0 skipping the
 * entry; idx may be NULL): p = M (4x3) * v, then the perspective-ish mapping
 *   t = c438/p.y; dst.x = p.x*t + c8e5c; dst.y = -(t*p.z) + c5fe8; dst.z = p.y(old y)
 * (dst is stored as floats after every step), optional clip code byte (0x80 when z<=0,
 * else bits 1/2 x<min/>max, 8/4 y<min/>max, 0x10 z<near) and dst.w = depth term.
 * Listing order of every x87 operation kept; constants c438/c8e5c/c5fe8 loaded once. */
static void na_project(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    gptr base = gmem_rd32_l(gm, gsz, E + 4u);
    gptr dst = gmem_rd32_l(gm, gsz, E + 8u);
    gptr idxp = gmem_rd32_l(gm, gsz, E + 0xcu);
    gptr code = gmem_rd32_l(gm, gsz, E + 0x10u);
    int32_t n = (int32_t)gmem_rd32_l(gm, gsz, E + 0x14u);
    gptr M = gmem_rd32_l(gm, gsz, E + 0x18u);
    gptr src = base;

    if (n != 0) {
        const double c8 = (double)ldf(gm, gsz, 0x005b6438u);      /* t numerator */
        const double c9 = (double)ldf(gm, gsz, 0x005b8e5cu);      /* x offset */
        const double c7 = (double)ldf(gm, gsz, 0x005b5fe8u);      /* y offset */
        double m[12];
        int k;
        for (k = 0; k < 12; k++) m[k] = (double)ldf(gm, gsz, M + 4u * (uint32_t)k);   /* the matrix does not change while the entries are projected */
        do {
            if (idxp) {
                uint32_t ix = gmem_rd32_l(gm, gsz, idxp);
                if (ix == 0u) goto next;
                src = base + (ix - 1u) * 12u;
            }
            {
                double vx = (double)ldf(gm, gsz, src), vy = (double)ldf(gm, gsz, src + 4u), vz = (double)ldf(gm, gsz, src + 8u);
                float o0 = (float)((((m[0] * vx) + (m[3] * vy)) + (m[6] * vz)) + m[9]);
                float o1 = (float)((((m[1] * vx) + (m[4] * vy)) + (m[7] * vz)) + m[10]);
                float o2 = (float)((((m[2] * vx) + (m[5] * vy)) + (m[8] * vz)) + m[11]);
                double t = c8 / (double)o1;
                float n0 = (float)(((double)o0 * t) + c9);
                float n1 = (float)(-(t * (double)o2) + c7);
                uint32_t o1bits = gmem_u32_f(o1);
                float d;
                double depth;
                stf(gm, gsz, dst, n0);
                stf(gm, gsz, dst + 4u, n1);
                gmem_wr32_l(gm, gsz, dst + 8u, o1bits);
                d = o1;                                          /* dst.z as stored */
                if (code) {
                    if (0.0 >= (double)d) goto clip_neg;         /* FCOMP 0,z: C0 clear (ordered, z<=0) -> 0x80; NaN counts as C0 set */
                    {
                        double x = (double)n0, y = (double)n1, z = (double)d;
                        uint8_t cc = 0;
                        if (!(x >= (double)ldf(gm, gsz, 0x005b6030u))) cc |= 1;
                        else if (x > (double)ldf(gm, gsz, 0x005b6004u)) cc |= 2;
                        if (!(y >= (double)ldf(gm, gsz, 0x005b6034u))) cc |= 8;
                        else if (y > (double)ldf(gm, gsz, 0x005b6008u)) cc |= 4;
                        if (!(z >= (double)ldf(gm, gsz, 0x005b6000u))) cc |= 0x10;
                        gmem_wr8_l(gm, gsz, code, cc);
                    }
                    goto clip_done;
clip_neg:
                    gmem_wr8_l(gm, gsz, code, 0x80u);
clip_done:;
                }
                {
                    uint32_t mode = gmem_rd32_l(gm, gsz, 0x00866fc8u);
                    if (o1bits != 0u) {
                        double r = 1.0 / (double)d;
                        stf(gm, gsz, dst + 8u, (float)r);
                        depth = r * (double)ldf(gm, gsz, 0x005b5ffcu);
                        stf(gm, gsz, dst + 0xcu, mode == 0x10u ? (float)depth : (float)(1.0 - depth));
                    } else {
                        stf(gm, gsz, dst + 0xcu, mode == 0x10u ? 0.0f : 1.0f);
                    }
                }
            }
next:
            src += 12u;
            dst += 16u;
            if (idxp) idxp += 4u;
            if (code) code += 1u;
        } while (--n > 0);
    }
    cg->r[ESP] = E + 4u;
}

#ifndef __3DS__
static int na_verify = -1;
static uint64_t na_checked, na_differed;
static void na_at_exit(void)
{
    fprintf(stderr, "actor_light verify: %llu calls checked, %llu differed\n",
            (unsigned long long)na_checked, (unsigned long long)na_differed);
}

#define NA_WIN_A 0x005b6000u
#define NA_WIN_N 0x3400u
static void na_shadow(cpu_t *cg)
{
    const gptr E = cg->r[ESP];
    const cpu_t c0 = *cg;
    uint32_t n = gmem_rd32(E + 0x20u), nl = gmem_rd32(E + 0xcu);
    gptr out = gmem_rd32(E + 0x1cu);
    uint32_t outlen = (n > 4096u ? 4096u : n) * 4u, bad = 0, i;
    uint8_t win0[NA_WIN_N], win1[NA_WIN_N], o0[16384], o1[16384];
    cpu_t cn;
    uint8_t *w = gmem_span(NA_WIN_A, NA_WIN_N, 1);
    uint8_t *ov = outlen ? gmem_span(out, outlen, 1) : NULL;

    (void)nl;
    if (!w || (outlen && !ov)) { na_light(cg); return; }
    memcpy(win0, w, NA_WIN_N);
    if (outlen) memcpy(o0, ov, outlen);
    na_light(cg);
    cn = *cg;
    memcpy(win1, w, NA_WIN_N);
    if (outlen) memcpy(o1, ov, outlen);
    memcpy(w, win0, NA_WIN_N);                              /* reference from the same input */
    if (outlen) memcpy(ov, o0, outlen);
    *cg = c0;
    wmain_0040fc10_ref(cg);
    for (i = 0; i < NA_WIN_N; i++)
        if (win1[i] != w[i]) { if (bad < 8) fprintf(stderr, "actor_light verify: %08x native %02x ref %02x\n", NA_WIN_A + i, win1[i], w[i]); bad++; }
    for (i = 0; i < outlen; i++)
        if (o1[i] != ov[i]) { if (bad < 8) fprintf(stderr, "actor_light verify: out+%x native %02x ref %02x\n", i, o1[i], ov[i]); bad++; }
    if (cn.r[EBX] != cg->r[EBX] || cn.r[ESI] != cg->r[ESI] || cn.r[EDI] != cg->r[EDI] || cn.r[EBP] != cg->r[EBP] ||
        cn.r[ESP] != cg->r[ESP] || cn.ftop != cg->ftop || cn.fcw != cg->fcw) {
        fprintf(stderr, "actor_light verify: regs native esp=%08x ftop=%u ref esp=%08x ftop=%u\n", cn.r[ESP], cn.ftop, cg->r[ESP], cg->ftop);
        bad++;
    }
    na_checked++;
    if (bad) na_differed++;
}
#endif

void wmain_0040fc10(cpu_t *cg)
{
    uint32_t prof_prev_ = g_prof_sub; prof_sub(PROF_SUB_ACT_LIGHT);
#ifndef __3DS__
    if (na_verify < 0) {
        const char *e = getenv("TPM_VERIFY_NA");
        na_verify = (e && *e == '1');
        if (na_verify) atexit(na_at_exit);
    }
    if (na_verify) { na_shadow(cg); return; }
#endif
    na_light(cg);
    prof_sub(prof_prev_);
}

#ifndef __3DS__
static int np_verify = -1;
static uint64_t np_checked, np_differed;
static void np_at_exit(void)
{
    fprintf(stderr, "actor_project verify: %llu calls checked, %llu differed\n",
            (unsigned long long)np_checked, (unsigned long long)np_differed);
}
static void np_shadow(cpu_t *cg)
{
    const gptr E = cg->r[ESP];
    const cpu_t c0 = *cg;
    gptr dst = gmem_rd32(E + 8u), code = gmem_rd32(E + 0x10u);
    int32_t n = (int32_t)gmem_rd32(E + 0x14u);
    uint32_t dl = (n > 0 && n <= 1024) ? (uint32_t)n * 16u : 0u, cl = (code && dl) ? (uint32_t)n : 0u, bad = 0, i;
    static uint8_t d0[16384], d1[16384], k0[1024], k1[1024];
    cpu_t cn;
    uint8_t *dv = dl ? gmem_span(dst, dl, 1) : NULL, *kv = cl ? gmem_span(code, cl, 1) : NULL;

    if (!dl || !dv || (cl && !kv)) { na_project(cg); return; }
    memcpy(d0, dv, dl);
    if (cl) memcpy(k0, kv, cl);
    na_project(cg);
    cn = *cg;
    memcpy(d1, dv, dl);
    if (cl) memcpy(k1, kv, cl);
    memcpy(dv, d0, dl);
    if (cl) memcpy(kv, k0, cl);
    *cg = c0;
    wmain_0040efd0_ref(cg);
    for (i = 0; i < dl; i++)
        if (d1[i] != dv[i]) { if (bad < 8) fprintf(stderr, "actor_project verify: dst+%x native %02x ref %02x\n", i, d1[i], dv[i]); bad++; }
    for (i = 0; i < cl; i++)
        if (k1[i] != kv[i]) { if (bad < 8) fprintf(stderr, "actor_project verify: code+%x native %02x ref %02x\n", i, k1[i], kv[i]); bad++; }
    if (cn.r[EBX] != cg->r[EBX] || cn.r[ESI] != cg->r[ESI] || cn.r[EDI] != cg->r[EDI] || cn.r[EBP] != cg->r[EBP] ||
        cn.r[ESP] != cg->r[ESP] || cn.ftop != cg->ftop || cn.fcw != cg->fcw) {
        fprintf(stderr, "actor_project verify: regs native esp=%08x ftop=%u ref esp=%08x ftop=%u\n", cn.r[ESP], cn.ftop, cg->r[ESP], cg->ftop);
        bad++;
    }
    np_checked++;
    if (bad) np_differed++;
}
#endif

void wmain_0040efd0(cpu_t *cg)
{
    uint32_t prof_prev_ = g_prof_sub; prof_sub(PROF_SUB_ACT_PROJ);
#ifndef __3DS__
    if (np_verify < 0) {
        const char *e = getenv("TPM_VERIFY_NA");
        np_verify = (e && *e == '1');
        if (np_verify) atexit(np_at_exit);
    }
    if (np_verify) { np_shadow(cg); return; }
#endif
    na_project(cg);
    prof_sub(prof_prev_);
}

/* ------------------------------------------------------------------ 0x00402155
 * Deferred actor polygon flush (void __cdecl f(void), once per frame, ~6 queued batches of
 * ~100 polygons). Walks the slot table at 0x5956f8 (12-byte slots: head, list, spare); for
 * every polygon of a slot's list it builds the 32-byte vertex records the game's D3D layer
 * takes (position+rhw copied from the source vertex array, colour, specular, uv scaled to the
 * texture size) in a buffer on the guest stack and submits it through 0x487260 (plain) or
 * 0x487c50 (textured/lit). The guest functions it calls are invoked as usual; only this
 * function's own body (formerly thousands of stack-slot loads/stores per frame) is native.
 * Float arithmetic follows the listing (float products rounded to float on store). */
#define AE_CALL(cg, target, ret) do { \
    nf_wr32x(esp - 4u, (ret)); esp -= 4u; \
    cg->r[ESP] = esp; cg->r[EBP] = ebp; \
    cpu_call(cg, (target)); \
    esp = cg->r[ESP]; \
} while (0)
#define nf_wr32x(a, v) gmem_wr32_l(gm, gsz, (a), (v))

static void na_emit(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr esp0 = cg->r[ESP];
    const uint32_t ebp_in = cg->r[EBP];
    uint32_t ebp = esp0 - 4u;
    uint32_t esp = ebp - 0x860u;
    const gptr B = ebp - 0x834u;               /* vertex record buffer (guest stack) */
    int32_t i;

    gmem_wr32_l(gm, gsz, ebp, ebp_in);
    if ((int32_t)gmem_rd32_l(gm, gsz, 0x005959f8u) <= 0) goto done;

    gmem_wr32_l(gm, gsz, esp - 4u, 0); /* (stack hygiene only) */
    gmem_wr32_l(gm, gsz, esp - 4u, gmem_rd32_l(gm, gsz, 0x004dd6e4u));
    esp -= 4u;
    AE_CALL(cg, 0x00488210u, 0x00402177u);
    esp += 4u;

    for (i = 0; i < (int32_t)gmem_rd32_l(gm, gsz, 0x005959f8u); i++) {
        const gptr slot = 0x005956f8u + (uint32_t)i * 12u;
        gptr p = gmem_rd32_l(gm, gsz, slot);
        gptr l28 = gmem_rd32_l(gm, gsz, p + 0xb0u);
        gptr lp;
        uint32_t sxb, syb;
        gptr q;
        uint32_t r;

        esp -= 12u;
        gmem_wr32_l(gm, gsz, esp, p);
        gmem_wr32_l(gm, gsz, esp + 4u, l28);
        gmem_wr32_l(gm, gsz, esp + 8u, 0);
        AE_CALL(cg, 0x0047b9bdu, 0x004021eau);
        r = cg->r[EAX];
        esp += 12u;
        if (r == 0) {
            lp = 0;
            sxb = syb = 0x3f800000u;
        } else {
            gptr tex = gmem_rd32_l(gm, gsz, l28 + 0x1cu);
            double k = (double)ldf(gm, gsz, 0x004a8018u);
            lp = l28 + 0x2cu;
            sxb = gmem_u32_f((float)(k / (double)gmem_rd32_l(gm, gsz, tex + 0xcu)));
            tex = gmem_rd32_l(gm, gsz, l28 + 0x1cu);
            syb = gmem_u32_f((float)(k / (double)gmem_rd32_l(gm, gsz, tex + 0x10u)));
        }
        {
            float sx = gmem_f32_l(sxb), sy = gmem_f32_l(syb);
            esp -= 4u;
            gmem_wr32_l(gm, gsz, esp, lp);
            AE_CALL(cg, 0x00488510u, 0x00402269u);
            esp += 4u;

            for (q = gmem_rd32_l(gm, gsz, slot + 4u); q != 0; q = gmem_rd32_l(gm, gsz, q)) {
                uint32_t col = gmem_rd8_l(gm, gsz, q + 0x16u) != 0xffu;
                gptr d = gmem_rd32_l(gm, gsz, q + 4u);
                uint32_t cnt, k2;
                gptr src, uv, colp, buf = B;
                if (d == 0) continue;
                cnt = gmem_rd8_l(gm, gsz, q + 0x14u);
                src = gmem_rd32_l(gm, gsz, q + 8u);
                uv = gmem_rd32_l(gm, gsz, q + 0xcu);
                colp = gmem_rd32_l(gm, gsz, q + 0x10u);
                for (k2 = 0; k2 < cnt; k2++, buf += 32u) {
                    uint32_t idx = gmem_rd32_l(gm, gsz, q + 0x18u + 4u * k2);
                    uint32_t uvi = gmem_rd32_l(gm, gsz, q + 0x68u + 4u * k2);
                    uint32_t vi, c;
                    gptr sv = src + (idx << 4);
                    gmem_wr32_l(gm, gsz, buf, gmem_rd32_l(gm, gsz, sv));
                    gmem_wr32_l(gm, gsz, buf + 4u, gmem_rd32_l(gm, gsz, sv + 4u));
                    gmem_wr32_l(gm, gsz, buf + 0xcu, gmem_rd32_l(gm, gsz, sv + 8u));
                    gmem_wr32_l(gm, gsz, buf + 8u, gmem_rd32_l(gm, gsz, sv + 0xcu));
                    vi = gmem_rd32_l(gm, gsz, gmem_rd32_l(gm, gsz, d + 0x18u) + 4u * k2);
                    gmem_wr32_l(gm, gsz, buf + 0x18u, gmem_u32_f((float)((double)ldf(gm, gsz, uv + uvi * 8u) * (double)sx)));
                    gmem_wr32_l(gm, gsz, buf + 0x1cu, gmem_u32_f((float)((double)ldf(gm, gsz, uv + uvi * 8u + 4u) * (double)sy)));
                    if (colp) {
                        c = gmem_rd32_l(gm, gsz, colp + vi * 4u);
                        if (col) c = (c & 0xffffffu) | ((uint32_t)gmem_rd8_l(gm, gsz, q + 0x16u) << 24);
                    } else {
                        c = 0xffffffffu;
                    }
                    gmem_wr32_l(gm, gsz, buf + 0x10u, c);
                    if (gmem_rd32_l(gm, gsz, 0x004dd6d0u) != 0)
                        gmem_wr32_l(gm, gsz, buf + 0x14u,
                                    gmem_rd32_l(gm, gsz, 0x00595a00u + ((uint32_t)(((int32_t)(src - 0x4bd6c0u)) >> 4) + vi) * 4u));
                    else
                        gmem_wr32_l(gm, gsz, buf + 0x14u, 0);
                }
                if (col) {
                    uint32_t l4 = 0, mask = 0xffffffffu, fl;
                    if (gmem_rd32_l(gm, gsz, d + 4u) & 0x10u) l4 = 0x40000u;
                    l4 |= 0x600u;
                    if (gmem_rd8_l(gm, gsz, q + 0x17u)) mask &= 0xffffffbfu;
                    fl = (gmem_rd32_l(gm, gsz, 0x004dd6e4u) | gmem_rd32_l(gm, gsz, 0x004dd6c8u) | l4) & mask;
                    esp -= 20u;
                    gmem_wr32_l(gm, gsz, esp, lp);
                    gmem_wr32_l(gm, gsz, esp + 4u, fl);
                    gmem_wr32_l(gm, gsz, esp + 8u, B);
                    gmem_wr32_l(gm, gsz, esp + 12u, cnt);
                    gmem_wr32_l(gm, gsz, esp + 16u, gmem_rd8_l(gm, gsz, q + 0x15u));
                    AE_CALL(cg, 0x00487c50u, 0x004024fcu);
                    esp += 20u;
                } else {
                    uint32_t st = gmem_rd32_l(gm, gsz, 0x004dd6e4u);
                    if (gmem_rd8_l(gm, gsz, q + 0x17u)) st &= 0xffffffbfu;
                    esp -= 4u;
                    gmem_wr32_l(gm, gsz, esp, st);
                    AE_CALL(cg, 0x00488210u, 0x0040251fu);
                    esp += 4u;
                    esp -= 12u;
                    gmem_wr32_l(gm, gsz, esp, B);
                    gmem_wr32_l(gm, gsz, esp + 4u, cnt);
                    gmem_wr32_l(gm, gsz, esp + 8u, gmem_rd8_l(gm, gsz, q + 0x15u));
                    AE_CALL(cg, 0x00487260u, 0x0040254eu);
                    esp += 12u;
                }
            }
        }
        gmem_wr32_l(gm, gsz, slot, 0);
        gmem_wr32_l(gm, gsz, slot + 4u, 0);
        gmem_wr32_l(gm, gsz, slot + 8u, 0);
    }
    gmem_wr32_l(gm, gsz, 0x005959f8u, 0);
    gmem_wr32_l(gm, gsz, 0x004dd6c0u, 0);
    gmem_wr32_l(gm, gsz, 0x004dd6e0u, 0);
    gmem_wr32_l(gm, gsz, 0x004dd6ecu, 0);
done:
    cg->r[EBP] = ebp_in;
    cg->r[ESP] = esp0 + 4u;
}

int g_native_off;   /* 3DS: sdmc:/3ds/tpm/nonative.txt present -> lifted bodies (A/B timing) */
void wmain_00402155_ref(cpu_t *c);
void wmain_00402155(cpu_t *cg)
{
#ifndef __3DS__
    static int off = -1;
    if (off < 0) off = getenv("TPM_EM_OFF") != NULL;
    if (off) { wmain_00402155_ref(cg); return; }
#else
    if (g_native_off) { wmain_00402155_ref(cg); return; }
#endif
    { uint32_t prev_ = g_prof_sub; prof_sub(PROF_SUB_ACT_EMIT); na_emit(cg); prof_sub(prev_); }
}

/* ------------------------------------------------------------------ 0x0040f1e0
 * Actor transform: void __cdecl f(actor *obj, float *M)  (~50 calls per frame).
 * Transcribed from the x86 listing. Ordinary polygons (flags & 0x88 == 0) run natively; an actor
 * with an environment-mapped (0x8) or 0x80 polygon is handed to the lifted body (a pre-scan
 * decides before anything is touched). Callees (matrix helpers, allocators, 0x40efd0, 0x40fc10, ...)
 * are the usual guest functions. x87 sequences keep the listing's operation order in doubles. */
void wmain_0040f1e0_ref(cpu_t *c);

typedef struct { uint8_t *gm; uint32_t gsz; cpu_t *cg; gptr S; } xf_ctx;
#define XR32(a) gmem_rd32_l(X->gm, X->gsz, (a))
#define XW32(a, v) gmem_wr32_l(X->gm, X->gsz, (a), (v))
#define XR8(a) gmem_rd8_l(X->gm, X->gsz, (a))
#define XF(a) ldf(X->gm, X->gsz, (a))

static void xf_call(xf_ctx *X, uint32_t target, uint32_t ret, int n, const uint32_t *a)
{
    gptr esp = X->S - 4u * (uint32_t)n;
    for (int i = 0; i < n; i++) XW32(esp + 4u * (uint32_t)i, a[i]);
    esp -= 4u;
    XW32(esp, ret);
    X->cg->r[ESP] = esp;
    { uint32_t prev_ = g_prof_sub; prof_sub(PROF_SUB_ACT_CALLEE); cpu_call(X->cg, target); prof_sub(prev_); }
}

static uint32_t xf_ftol(xf_ctx *X, double v, uint32_t ret)
{
    cpu_t *cg = X->cg;
    uint32_t T = cg->ftop;
    cg->ftop = (T + 7u) & 7u;
    cg->st[cg->ftop] = v;
    xf_call(X, 0x0049a44cu, ret, 0, NULL);
    cg->ftop = T;
    return cg->r[EAX];
}

static void na_xform(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    xf_ctx xc = {gm, gsz, cg, E - 0x144u}, *X = &xc;
    const gptr S = xc.S;
    const gptr obj = XR32(E + 4u), M = XR32(E + 8u);
    uint32_t np, nv, cnt = 0, v108;
    int main_path;
    gptr recbase, vtx, g, view, alloc = 0;
    uint32_t a[8];

    if (!obj) goto done;
    XW32(0x005b8650u, obj);
    XW32(0x005b8e6cu, XR32(obj + 0x28u) != 0xffffffffu ? 3u : 0xffffffffu);
    if (!XR32(obj + 0x24u)) goto done;
    view = XR32(0x006f83e4u);
    a[0] = S + 0xb4u; a[1] = view + 8u; a[2] = M;
    xf_call(X, 0x0047dad6u, 0x0040f23au, 3, a);
    if (XR32(0x008a0200u) == 0) XW32(0x005b643cu, 0);
    else if (!(XR8(0x008686a4u) & 1u)) XW32(0x005b643cu, 1);
    else {
        a[0] = S + 0xa8u; a[1] = obj + 0x58u; a[2] = S + 0xb4u;
        xf_call(X, 0x0047e34du, 0x0040f268u, 3, a);
        g = XR32(0x005b8e68u);
        view = XR32(0x006f83e4u);
        a[0] = XR32(view + 0x48u); a[1] = S + 0xa8u;
        a[2] = gmem_u32_f((float)((double)XF(g + 0x154u) * (double)XF(obj + 0x54u)));
        xf_call(X, 0x0047840eu, 0x0040f294u, 3, a);
        XW32(0x005b643cu, cg->r[EAX]);
        if (cg->r[EAX] == 2u) goto done;
    }
    a[0] = S + 0xe4u; a[1] = M;
    xf_call(X, 0x0047ceb9u, 0x0040f2cbu, 2, a);
    XW32(S + 0x9cu, XR32(0x00868704u)); XW32(S + 0xa0u, XR32(0x00868708u)); XW32(S + 0xa4u, XR32(0x0086870cu));
    a[0] = 0x005b5ff0u; a[1] = S + 0x9cu; a[2] = S + 0xe4u;
    xf_call(X, 0x0047e34du, 0x0040f30eu, 3, a);

    np = XR32(obj + 0x50u);
    recbase = XR32(obj + 0x40u);
    nv = XR32(obj + 0x48u);
    vtx = XR32(obj + 0x30u);
    {
        uint8_t *p = gmem_span(0x005b8450u, np ? np : 1u, 1);
        uint8_t *q = gmem_span(0x005b8658u, nv ? nv * 4u : 1u, 1);
        if (p && np) memset(p, 0, np);
        if (q && nv) memset(q, 0, nv * 4u);
    }
    g = XR32(0x005b8e68u);
    v108 = nv;
    {
        double fv = (double)XF(g + 0x158u), zero = (double)XF(0x004a8100u);
        main_path = !(fv < zero || fv > zero);         /* == 0.0, or unordered */
        if (main_path) {
            uint32_t idx;
            for (idx = 0; idx < np; idx++) {
                gptr rec = recbase + 0x4cu * idx;
                uint32_t code;
                if (XR8(rec + 4u) & 1u) code = 1u;
                else {
                    gptr lst = XR32(rec + 0x18u);
                    uint32_t i0 = XR32(lst);
                    gptr vp = vtx + i0 * 12u;
                    double dy = (double)XF(0x005b5ff4u) - (double)XF(vp + 4u);
                    double dx = (double)XF(0x005b5ff0u) - (double)XF(vp);
                    double dz = (double)XF(0x005b5ff8u) - (double)XF(vp + 8u);
                    double sum = (dy * (double)XF(rec + 0x3cu) + dx * (double)XF(rec + 0x38u)) + dz * (double)XF(rec + 0x40u);
                    if (sum > zero) code = 1u;                          /* TEST AH,0x41 clear: sum > 0 */
                    else if (XR8(0x008686a0u) & 1u) continue;           /* culled */
                    else code = 2u;
                }
                gmem_wr8_l(gm, gsz, 0x005b8450u + idx, (uint8_t)code);
                {
                    uint32_t k, n = XR32(rec + 0x14u);
                    gptr lst = XR32(rec + 0x18u);
                    for (k = 0; k < n; k++) {
                        uint32_t vi = XR32(lst + 4u * k);
                        XW32(0x005b8658u + vi * 4u, vi + 1u);
                    }
                }
                cnt++;
            }
        } else {
            uint32_t idx;
            XW32(0x005b643cu, 0);
            XW32(0x005b5fe4u, 0);
            v108 = 0;
            for (idx = 0; idx < np; idx++) {
                gptr rec = recbase + 0x4cu * idx;
                double s = (double)XF(g + 0x158u);
                float ax = (float)((double)XF(rec + 0x38u) * s), by = (float)((double)XF(rec + 0x3cu) * s), cz = (float)((double)XF(rec + 0x40u) * s);
                uint32_t k, n = XR32(rec + 0x14u);
                gptr lst = XR32(rec + 0x18u);
                gmem_wr8_l(gm, gsz, 0x005b8450u + idx, 1u);
                for (k = 0; k < n; k++) {
                    uint32_t vi = XR32(lst + 4u * k);
                    gptr o = 0x005b9080u + v108 * 12u, src = vtx + vi * 12u;
                    XW32(o, gmem_u32_f((float)((double)XF(src) + (double)ax)));
                    XW32(o + 4u, gmem_u32_f((float)((double)XF(src + 4u) + (double)by)));
                    XW32(o + 8u, gmem_u32_f((float)((double)XF(src + 8u) + (double)cz)));
                    v108++;
                    XW32(0x005b8658u + (v108 - 1u) * 4u, v108);
                }
                cnt++;
                g = XR32(0x005b8e68u);
            }
            vtx = 0x005b9080u;
        }
    }
    if (cnt == 0) goto done;
    nv = main_path ? nv : v108;
    a[0] = nv;
    xf_call(X, 0x00402130u, 0x0040f58du, 1, a);
    alloc = cg->r[EAX];
    if (!alloc) goto done;
    a[0] = vtx; a[1] = alloc; a[2] = 0x005b8658u; a[3] = XR32(0x005b643cu) ? 0x005b6038u : 0u; a[4] = nv; a[5] = S + 0xb4u;
    xf_call(X, 0x0040efd0u, 0x0040f5c2u, 6, a);
    if (XR32(0x005b643cu)) {
        uint32_t i;
        gptr r = XR32(XR32(0x005b8650u) + 0x40u);
        for (i = 0; i < XR32(obj + 0x50u); i++, r += 0x4cu) {
            if (XR8(0x005b8450u + i)) {
                a[0] = r; a[1] = 0x005b6038u;
                xf_call(X, 0x0040f170u, 0x0040f5f4u, 2, a);
                if ((cg->r[EAX] & 0xff80u) == 0u) gmem_wr8_l(gm, gsz, 0x005b6238u + i, (uint8_t)cg->r[EAX]);
                else gmem_wr8_l(gm, gsz, 0x005b8450u + i, 0u);
            }
        }
    }
    if (XR32(0x005b8e6cu) == 3u) {
        gptr gg = XR32(0x005b8e68u), ob = XR32(0x005b8650u);
        uint32_t k;
        view = XR32(0x006f83e4u);
        if (XR32(gg + XR32(ob + 0x20u) * 4u + 0x30u) == 0u) XW32(0x005b8e6cu, 2u);
        for (k = 0; k < XR32(XR32(0x006f83e4u) + 0x58u); k++) {
            view = XR32(0x006f83e4u);
            a[0] = 0x005ba880u + 12u * k; a[1] = view + 0x15cu + 12u * k; a[2] = S + 0xe4u;
            xf_call(X, 0x0047e34du, 0x0040f672u, 3, a);
        }
        view = XR32(0x006f83e4u);
        if (XR32(0x005b8e6cu) == 3u) {
            gg = XR32(0x005b8e68u); ob = XR32(0x005b8650u);
            a[0] = view + 0x5cu; a[1] = 0x005ba880u; a[2] = XR32(view + 0x58u); a[3] = XR32(ob + 0x44u);
            a[4] = XR32(ob + 0x30u); a[5] = XR32(ob + 0x38u); a[6] = XR32(gg + XR32(ob + 0x20u) * 4u + 0x30u); a[7] = XR32(ob + 0x48u);
            xf_call(X, 0x0040fc10u, 0x0040f6c2u, 8, a);
        }
    }
    {
        uint32_t idx, running = 0;
        gptr base = XR32(obj + 0x40u);
        const double zero = (double)XF(0x004a8100u);
        for (idx = 0; idx < XR32(obj + 0x50u); idx++, base += 0x4cu) {
            gptr rec = base;
            gptr node;
            uint32_t k, n;
            if (!XR8(0x005b8450u + idx)) continue;
            xf_call(X, 0x00402021u, 0x0040f6fbu, 0, a);
            node = cg->r[EAX];
            if (!node) goto done;
            XW32(node + 4u, base);
            XW32(node + 8u, alloc);
            XW32(node + 0xcu, XR32(XR32(0x005b8650u) + 0x34u));
            XW32(node + 0x10u, XR32(XR32(0x005b8e68u) + XR32(XR32(0x005b8650u) + 0x20u) * 4u + 0x30u));
            n = XR32(rec + 0x14u);
            if (XR32(rec + 0xcu) == 0u)
                for (k = 0; k < n; k++) XW32(XR32(node + 0x10u) + 4u * k, 0x00ffffffu);
            {
                double fz = (double)XF(XR32(0x005b8e68u) + 0x158u), zero = (double)XF(0x004a8100u);
                n = XR32(rec + 0x14u);
                if (!(fz < zero || fz > zero)) {      /* == 0.0 or unordered: lists copied */
                    gptr l1 = XR32(rec + 0x18u), l2 = XR32(rec + 0x1cu);
                    for (k = 0; k < n; k++) {
                        XW32(node + 0x18u + 4u * k, XR32(l1 + 4u * k));
                        XW32(node + 0x68u + 4u * k, XR32(l2 + 4u * k));
                    }
                } else {                              /* renumbered */
                    gptr l2 = XR32(rec + 0x1cu);
                    for (k = 0; k < n; k++) {
                        XW32(node + 0x18u + 4u * k, running++);
                        XW32(node + 0x68u + 4u * k, XR32(l2 + 4u * k));
                    }
                }
            }
            /* flags 0x8 / 0x80 were excluded by the pre-scan */
            gmem_wr8_l(gm, gsz, node + 0x14u, (uint8_t)XR32(rec + 0x14u));
            gmem_wr8_l(gm, gsz, node + 0x16u, (uint8_t)xf_ftol(X, (double)XF(0x005b8e64u), 0x0040fafcu));
            if (XR32(0x005b643cu)) gmem_wr8_l(gm, gsz, node + 0x15u, XR8(0x005b6238u + idx));
            else gmem_wr8_l(gm, gsz, node + 0x15u, 0u);
            {
                double v = (double)XF(rec + 0x34u);
                if (v > zero && v < (double)XF(0x004a80f8u))
                    gmem_wr8_l(gm, gsz, node + 0x16u, (uint8_t)xf_ftol(X, (double)(float)(v * (double)XF(0x005b8e64u)), 0x0040fb49u));
            }
            a[0] = node;
            xf_call(X, 0x00401eb8u, 0x0040fb52u, 1, a);
            if (XR32(0x005b5fe4u) != 0u && XR32(rec + 0x48u) != 0u) {
                int32_t m = (int32_t)XR8(node + 0x14u);
                int32_t j;
                if (m > 4) m = 4;
                for (j = 0; j < m; j++) {
                    a[0] = S + 0x114u + 12u * (uint32_t)j;
                    a[1] = XR32(XR32(0x005b8650u) + 0x30u) + XR32(node + 0x18u + 4u * (uint32_t)j) * 12u;
                    a[2] = M;
                    xf_call(X, 0x0047e34du, 0x0040fba4u, 3, a);
                }
                a[0] = XR32(0x005b8e68u); a[1] = base; a[2] = S + 0x114u;
                xf_call(X, XR32(rec + 0x48u), 0x0040fbc6u, 3, a);
            }
        }
    }
    a[0] = nv;
    xf_call(X, 0x00402046u, 0x0040fbfcu, 1, a);
done:
    cg->r[ESP] = E + 4u;
}


static uint64_t g_xf_calls, g_xf_fallbacks;
#ifndef __3DS__
static void xf_at_exit(void) { fprintf(stderr, "actor_xform: %llu calls, %llu fell back to the lifted body\n", (unsigned long long)g_xf_calls, (unsigned long long)g_xf_fallbacks); }
#endif

void wmain_0040f1e0(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    const gptr obj = gmem_rd32_l(gm, gsz, E + 4u);
    int rare = 0;
#ifndef __3DS__
    if (!g_xf_calls) atexit(xf_at_exit);
#endif
    g_xf_calls++;
#ifndef __3DS__
    if (g_xf_calls % 2000 == 0) fprintf(stderr, "actor_xform: %llu calls, %llu fallbacks\n", (unsigned long long)g_xf_calls, (unsigned long long)g_xf_fallbacks);
#endif
    if (obj && gmem_rd32_l(gm, gsz, obj + 0x24u)) {
        uint32_t np = gmem_rd32_l(gm, gsz, obj + 0x50u), i;
        gptr rec = gmem_rd32_l(gm, gsz, obj + 0x40u);
        if (np > 2048u) rare = 1;
        for (i = 0; i < np && !rare; i++, rec += 0x4cu)
            if (gmem_rd8_l(gm, gsz, rec + 4u) & 0x88u) rare = 1;
    }
#ifndef __3DS__
    { static int off = -1; if (off < 0) off = getenv("TPM_XF_OFF") != NULL; if (off) rare = 1; }
#endif
#ifdef __3DS__
    if (g_native_off) rare = 1;
#endif
    if (rare) { g_xf_fallbacks++; wmain_0040f1e0_ref(cg); return; }
    { uint32_t prev_ = g_prof_sub; prof_sub(PROF_SUB_ACT_XFORM); na_xform(cg); prof_sub(prev_); }
}

/* ------------------------------------------------------------------ 0x00406300
 * Scene object -> depth-bucket list insert (void __cdecl f(obj *o)), called per visible object.
 * Transcribed from the x86 listing. ECX/EBX/ESI/EDI are preserved by the original, EAX/EDX are scratch.
 * The branch with the global at 0x59df60 != 0 (x87 fade via __ftol) is left to the lifted body. */
void wmain_00406300_ref(cpu_t *c);
void wmain_00406300(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    const gptr o = gmem_rd32_l(gm, gsz, E + 4u);
    uint32_t al, cl, edx, edi;

#ifdef __3DS__
    if (g_native_off) { wmain_00406300_ref(cg); return; }
#else
    { static int off = -1; if (off < 0) off = getenv("TPM_EM_OFF") != NULL; if (off) { wmain_00406300_ref(cg); return; } }
#endif
    if (gmem_rd32_l(gm, gsz, 0x0059df60u) != 0u) { wmain_00406300_ref(cg); return; }
    if (gmem_rd8_l(gm, gsz, o + 0xcu) & gmem_rd8_l(gm, gsz, 0x0059df54u)) goto done;
    al = gmem_rd8_l(gm, gsz, o + 0x28u);
    if (al & 0x10u) goto done;
    if ((al & 4u) && gmem_rd8_l(gm, gsz, 0x008bfaa0u + gmem_rd8_l(gm, gsz, o + 0x2au)) == 2u) goto done;
    if ((gmem_rd8_l(gm, gsz, o + 0x2du) & 1u) && gmem_rd8_l(gm, gsz, 0x008bc920u + gmem_rd16_l(gm, gsz, o + 0x30u)) != 0u) goto done;
    if (gmem_rd8_l(gm, gsz, o + 0x38u) & gmem_rd8_l(gm, gsz, 0x0059df44u)) {
        gmem_wr32_l(gm, gsz, 0x0059df98u, gmem_rd32_l(gm, gsz, 0x0059df98u) + 1u);
        goto done;
    }
    cl = gmem_rd8_l(gm, gsz, o + 0x2bu);
    al = 0;
    if (cl) { uint32_t t = cl & 0x1fu; al = ((t >> 2) + t * 8u) & 0xffu; }
    gmem_wr8_l(gm, gsz, o + 0xeu, (uint8_t)al);
    edi = gmem_rd32_l(gm, gsz, 0x0059df88u);
    edx = gmem_rd8_l(gm, gsz, o + 0x24u);
    if ((int32_t)edx >= (int32_t)edi) goto done;
    if (cl || al) {
        edx = edi;
        if (!al) { uint32_t t = cl & 0x1fu; gmem_wr8_l(gm, gsz, o + 0xeu, (uint8_t)(((t << 3) & 0xffu) + (t >> 2))); }
    }
    {
        uint32_t cnt = gmem_rd32_l(gm, gsz, 0x0059debcu);
        uint32_t head = gmem_rd32_l(gm, gsz, 0x008bc880u + edx * 4u);
        uint32_t rec = cnt * 12u + 0x008a4880u;
        gmem_wr32_l(gm, gsz, rec + 4u, head);
        gmem_wr32_l(gm, gsz, 0x008bc880u + edx * 4u, rec);
        gmem_wr32_l(gm, gsz, 0x0059debcu, cnt + 1u);
        gmem_wr32_l(gm, gsz, rec, o);
    }
    if (gmem_rd8_l(gm, gsz, o + 0x2du) & 1u)
        gmem_wr8_l(gm, gsz, 0x008bc920u + gmem_rd16_l(gm, gsz, o + 0x30u), 1u);
done:
    cg->r[ESP] = E + 4u;
}

/* ------------------------------------------------------------------ 0x0049c740 (_CIcos) / 0x0049c7f0 (_CIsin)
 * CRT trig wrappers: argument and result in ST0. The lifted body runs 6 nested calls (classify, core, error
 * handler stubs) per call, ~800 sin + ~800 cos per frame in gameplay. Fast path: control word 0x27f, a finite
 * argument below 2^63 (FSIN/FCOS leave C2 clear) -> ST0 = cos/sin(ST0), exactly what the lifted core computes.
 * Everything else (inf/NaN, huge arguments, other control words) runs the lifted body. */
void wmain_0049c740_ref(cpu_t *c);
void wmain_0049c7f0_ref(cpu_t *c);
static int trig_fast(cpu_t *cg, double *x)
{
#ifdef __3DS__
    if (g_native_off) return 0;
#else
    { static int off = -1; if (off < 0) off = getenv("TPM_EM_OFF") != NULL; if (off) return 0; }
#endif
    /* What matters for the fast path is the precision control (double), the rounding
     * mode (nearest) and that the exceptions are masked. Bit 6 of the control word is
     * reserved and the game also runs with it clear (0x023f, seen in the level-1
     * golden run, instead of the CRT's 0x027f). The lifted cores do their arithmetic
     * in doubles whatever those words say, so both are equivalent here. */
    if ((cg->fcw & 0x0f3fu) != 0x023fu) return 0;
    *x = cg->st[cg->ftop & 7u];
    return isfinite(*x) && fabs(*x) < 9.2e18;
}
void wmain_0049c740(cpu_t *cg)
{
    double x;
    if (!trig_fast(cg, &x)) { wmain_0049c740_ref(cg); return; }
    cg->st[cg->ftop & 7u] = cos(x);
    cg->fsw &= (uint16_t)~FSW_C2;
    cg->r[ESP] += 4u;
}
void wmain_0049c7f0(cpu_t *cg)
{
    double x;
    if (!trig_fast(cg, &x)) { wmain_0049c7f0_ref(cg); return; }
    cg->st[cg->ftop & 7u] = sin(x);
    cg->fsw &= (uint16_t)~FSW_C2;
    cg->r[ESP] += 4u;
}

/* ------------------------------------------------------------------ 0x0049a44c (__ftol)
 * ST0 truncated to a 64-bit integer in EDX:EAX, ST0 popped (control word saved/restored around FISTP).
 * ~600 calls per frame. Out of range / NaN -> the x87 "integer indefinite" 0x8000000000000000. */
void wmain_0049a44c_ref(cpu_t *c);
void wmain_0049a44c(cpu_t *cg)
{
    double v;
#ifdef __3DS__
    if (g_native_off) { wmain_0049a44c_ref(cg); return; }
#else
    { static int off = -1; if (off < 0) off = getenv("TPM_EM_OFF") != NULL; if (off) { wmain_0049a44c_ref(cg); return; } }
#endif
    v = cg->st[cg->ftop & 7u];
    {
        uint64_t r = (uint64_t)cpu_fist64((uint16_t)(cg->fcw | 0x0c00u), v);   /* RC = truncate, as the original's FLDCW */
        cg->r[EAX] = (uint32_t)r;
        cg->r[EDX] = (uint32_t)(r >> 32);
    }
    cg->ftop = (cg->ftop + 1u) & 7u;
    cg->r[ESP] += 4u;
}

/* ------------------------------------------------------------------ 0x00494238
 * void __cdecl sincos(float angle, float *sin_out, float *cos_out): x = angle * K (float); outputs sin/cos(x) as
 * floats (either pointer may be NULL). ~770 calls per frame. Uses the _CIsin/_CIcos fast path conditions. */
void wmain_00494238_ref(cpu_t *c);
void wmain_00494238(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    double d;
    float x;
    gptr ps, pc;
#ifdef __3DS__
    if (g_native_off) { wmain_00494238_ref(cg); return; }
#else
    { static int off = -1; if (off < 0) off = getenv("TPM_EM_OFF") != NULL; if (off) { wmain_00494238_ref(cg); return; } }
#endif
    if ((cg->fcw & 0x0f3fu) != 0x023fu) { wmain_00494238_ref(cg); return; }   /* see trig_fast: the game also runs with 0x023f */
    x = (float)((double)ldf(gm, gsz, E + 4u) * (double)ldf(gm, gsz, 0x004a8e90u));
    d = (double)x;
    if (!(isfinite(d) && fabs(d) < 9.2e18)) { wmain_00494238_ref(cg); return; }
    ps = gmem_rd32_l(gm, gsz, E + 8u);
    pc = gmem_rd32_l(gm, gsz, E + 12u);
    if (ps) gmem_wr32_l(gm, gsz, ps, gmem_u32_f((float)sin(d)));
    if (pc) gmem_wr32_l(gm, gsz, pc, gmem_u32_f((float)cos(d)));
    cg->fsw &= (uint16_t)~FSW_C2;
    cg->r[ESP] = E + 4u;
}

/* 0x00494272: float __cdecl sin_deg(float degrees) - ST0 = sin(degrees * pi/180).
 * A 4-instruction wrapper around the CIsin intrinsic (0x49c7f0, already native):
 * `fld dword [ebp+8]; fmul dword [0x4a8e90]; call 0x49c7f0; fst dword [ebp-8]; ret`.
 * The product is NOT rounded to float - the callee reads it back from the x87 stack
 * as a double - so the native keeps it in double too. ~20 instructions saved per
 * call, and world_cull (native_world.c, wc_sin) calls it per darkened vertex. */
void wmain_00494272_ref(cpu_t *c);
void wmain_0049c7f0(cpu_t *c);
void wmain_00494272(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    gptr E;
    float x, k;
    {
        uint32_t u;
#ifdef __3DS__
        if (g_native_off) { wmain_00494272_ref(cg); return; }
#else
        { static int off = -1; if (off < 0) off = getenv("TPM_EM_OFF") != NULL; if (off) { wmain_00494272_ref(cg); return; } }
#endif
        E = cg->r[ESP];
        u = gmem_rd32_l(gm, gsz, E + 4u); memcpy(&x, &u, 4);
        u = gmem_rd32_l(gm, gsz, 0x004a8e90u); memcpy(&k, &u, 4);
    }
    cg->ftop = (cg->ftop - 1u) & 7u;
    cg->st[cg->ftop] = (double)x * (double)k;           /* 00494278: fld [ebp+8]; fmul [0x4a8e90] */
    {
        gptr s = E - 4u;
        gmem_wr32_l(gm, gsz, s, 0x00494286u);
        cg->r[ESP] = s;
        wmain_0049c7f0(cg);                             /* 00494281: call 0x49c7f0 */
        cg->r[ESP] = E;
    }
    {   /* 00494286: fst dword [ebp-8] - a dead copy that leaves ST0 alone */
        uint32_t u = gmem_u32_f((float)cg->st[cg->ftop & 7u]);
        gmem_wr32_l(gm, gsz, E - 4u - 0x5cu + 0x54u, u);
    }
    cg->r[ESP] = E + 4u;
}
