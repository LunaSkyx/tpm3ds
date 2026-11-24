/* Native pre-filter for 0x00408078: "offer one candidate light/object to the actor" (called up to ~20x
 * per actor from the gather 0x407d26, ~7 % of the busy host time in a big fight).
 *
 * int __cdecl f(ctx, cand, pos, count, float radius_extra)
 *
 * Most candidates are rejected: flag 0x100, the per-axis direction masks, or the distance test
 * (distance^2 < (scaled radius + extra)^2 fails). This file transcribes exactly that rejecting prefix
 * (listing docs/asm/00408078_listing.txt, 0x408078..0x408252) and returns 0 for it; every accepted
 * candidate (the rare case that allocates a light record and calls 0x47a7be/0x40705a/0x476792) runs the
 * lifted body wmain_00408078_ref unchanged. The rejecting path only writes locals of its own frame, so
 * skipping the body is invisible; the float arithmetic mirrors the lifted code (double intermediates,
 * rounding to float at every store; the x87 compare semantics of "test ah, 1 / 0x41").
 *
 * Contract: EAX = 0, ESP = E + 4 (cdecl, caller pops the arguments); the lifted callers do not read
 * the other scratch registers after the call (the previous gather native relies on the same).
 * Verify (host): TPM_VERIFY_NA=1 runs the lifted body on every rejection and compares EAX.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_00408078_ref(cpu_t *c);
extern int g_native_off;

#define K_ZERO 0x004a8084u
#define K_SCLA 0x004a8094u

static inline float ldf(uint8_t *gm, uint32_t gsz, gptr a) { return gmem_f32_l(gmem_rd32_l(gm, gsz, a)); }

/* 1: rejected by the prefix, 0: must run the lifted body */
static int cand_rejected(cpu_t *cg, gptr E)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr p2 = gmem_rd32_l(gm, gsz, E + 8u), p3 = gmem_rd32_l(gm, gsz, E + 12u);
    const uint32_t fl = gmem_rd32_l(gm, gsz, p2);
    const uint32_t kind = gmem_rd32_l(gm, gsz, p2 + 12u);
    const double K = (double)ldf(gm, gsz, K_ZERO);
    float dx, dy, dz, r, c, l10, l4;
    (void)cg;

    if (fl & 0x100u) return 1;
    dx = (float)((double)ldf(gm, gsz, p3)       - (double)ldf(gm, gsz, p2 + 0x10u));
    dy = (float)((double)ldf(gm, gsz, p3 + 4u)  - (double)ldf(gm, gsz, p2 + 0x14u));
    dz = (float)((double)ldf(gm, gsz, p3 + 8u)  - (double)ldf(gm, gsz, p2 + 0x18u));
    if (kind != 0u) {
        /* fail when "dx < K or unordered" (test ah,1 clear) resp. "dx > K" (test ah,0x41 clear) */
        if (!(fl & 1u)    && !((double)dx >= K)) return 1;
        if (!(fl & 2u)    &&  ((double)dx >  K)) return 1;
        if (!(fl & 4u)    &&  ((double)dy >  K)) return 1;
        if (!(fl & 8u)    && !((double)dy >= K)) return 1;
        if (!(fl & 0x10u) &&  ((double)dz >  K)) return 1;
        if (!(fl & 0x20u) && !((double)dz >= K)) return 1;
    }
    r = (float)((double)ldf(gm, gsz, p2 + 0x24u) * (double)ldf(gm, gsz, K_SCLA));
    if (kind == 2u) {
        float t = ((double)dz >= K) ? dz : -dz;
        double tmp = (double)r * (double)t + (double)r;
        r = (float)tmp;
        c = (float)((double)dx * (double)dx + (double)dy * (double)dy);
    } else {
        double a = (double)dx * (double)dx, b = (double)dy * (double)dy, d = (double)dz * (double)dz;
        c = (float)((a + b) + d);
    }
    {
        double s = (double)r + (double)ldf(gm, gsz, E + 0x14u);   /* fld r; fadd extra; fst l10 (ST keeps the double) */
        l10 = (float)s;
        l4 = (float)(s * (double)l10);
    }
    return !((double)c < (double)l4);
}

void wmain_00408078(cpu_t *cg)
{
    const gptr E = cg->r[ESP];
    if (g_native_off) { wmain_00408078_ref(cg); return; }
#ifndef __3DS__
    { static int off = -1; if (off < 0) off = getenv("TPM_CAND_OFF") != NULL; if (off) { wmain_00408078_ref(cg); return; } }
#endif
#ifndef __3DS__
    {
        static int verify = -1;
        if (verify < 0) { const char *e = getenv("TPM_VERIFY_NA"); verify = (e && *e == '1'); }
        if (verify) {
            static unsigned long long n, bad;
            int rej = cand_rejected(cg, E);
            cpu_t c0 = *cg;
            wmain_00408078_ref(cg);
            n++;
            if (rej && cg->r[EAX] != 0u) {
                if (bad++ < 8) fprintf(stderr, "cand408078 verify: rejected natively but ref accepted (call %llu)\n", n);
            }
            { static int reg; if (!reg) { reg = 1; } }
            (void)c0;
            if ((n & 0x3fffu) == 0) fprintf(stderr, "cand408078 verify: %llu calls, %llu mismatches\n", n, bad);
            return;
        }
    }
#endif
    if (cand_rejected(cg, E)) {
        cg->r[EAX] = 0;
        cg->r[ESP] = E + 4u;
        return;
    }
    wmain_00408078_ref(cg);
}
