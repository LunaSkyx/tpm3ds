/* Native 0x00407d26: gather the nearby scene objects for one actor (0.89 ms/frame at 3
 * calls/frame, docs/PLAN.md session 16 section 6; the x_7d26 timer entry).
 *
 * A transcription of the x86 listing docs/asm/00407d26_listing.txt, which is the authority;
 * the lifted body (wmain_00407d26_ref) is the executable reference.
 *
 * What it does:
 *  - with ctx->0x58 > 0 it turns the actor's world position into the "moved by direction"
 *    position at ctx+0x15c..0x164 (sin/cos of the angle argument, scaled by [0x4aa180]),
 *    publishing and optionally clearing the direction vector at node->0x2c;
 *  - converts the position to two ints (CRT __ftol) and asks 0x40d8cb for the spatial table;
 *  - walks that table (byte count at +0, 0x40-byte cells from +0x10, a byte count at +0x25,
 *    uint16 indices from +0), resolves each index through world->0x9c (0x20-byte records) and
 *    offers up to 20 candidates to 0x408078;
 *  - the same again for the two object lists at world+0x1cc (count +0x1c4, stride 0x80) and
 *    world+0x1d8 (count +0x1d0), again while the count is below 20.
 * Returns the number of accepted candidates in EAX.
 *
 * Contract: only EAX and the guest memory the listing writes change; EBX/ESI/EDI are not used
 * (the original pushes none of them) and EBP/ESP are restored by the epilogue. The x87 stack is
 * used for the __ftol argument exactly as the original does (push, call, the callee pops it).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_00407d26_ref(cpu_t *c);
void wmain_00494238(cpu_t *c);      /* sincos(angle, *sin, *cos) */
void wmain_0049a44c(cpu_t *c);      /* CRT __ftol: st0 -> eax, truncating */
void wmain_0040d8cb(cpu_t *c);      /* spatial table lookup */
void wmain_00408078(cpu_t *c);      /* offer one candidate */

#define G_WORLD   0x008a0060u
#define G_DIRSRC  0x008a4864u
#define G_DIRCLR  0x008a4868u
#define K_SCALE_A 0x004aa180u
#define K_SCALE_B 0x004aa184u

/* A/B switch, same policy as the other natives. */
static int zx_off(void)
{
    extern int g_native_off;
#ifdef __3DS__
    return g_native_off;
#else
    static int off = -1;
    if (off < 0) off = (g_native_off != 0) || getenv("TPM_AX_OFF") != NULL;
    return off;
#endif
}

static uint64_t zx_calls;
static void zx_at_exit(void) { fprintf(stderr, "gather native: %llu calls\n", (unsigned long long)zx_calls); }

static inline uint32_t zrd(uint8_t *gm, uint32_t gsz, gptr a) { return gmem_rd32_l(gm, gsz, a); }
static inline void zwr(uint8_t *gm, uint32_t gsz, gptr a, uint32_t v) { gmem_wr32_l(gm, gsz, a, v); }
static inline float zldf(uint8_t *gm, uint32_t gsz, gptr a) { return gmem_f32_l(zrd(gm, gsz, a)); }
static inline void zstf(uint8_t *gm, uint32_t gsz, gptr a, float f) { zwr(gm, gsz, a, gmem_u32_f(f)); }

/* cdecl guest call with the arguments on the guest stack below sp (the body's ESP). */
static void zcall(cpu_t *cg, void (*fn)(cpu_t *), gptr sp, uint32_t ret, uint32_t n, const uint32_t *a)
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

void wmain_00407d26(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    const gptr sp = E - 0x40u;              /* push ebp; sub esp, 0x3c */
    const gptr ctx = zrd(gm, gsz, E + 4u);  /* [ebp+8]  */
    const gptr world = zrd(gm, gsz, G_WORLD);   /* [0x8a0060] is the struct pointer, not a literal */
    const gptr vec = zrd(gm, gsz, E + 8u);  /* [ebp+0xc] position */
    const gptr arg3 = zrd(gm, gsz, E + 0xcu);/* [ebp+0x10] */
    const gptr psin = E - 0x28u;            /* [ebp-0x24] */
    const gptr pcos = E - 0x30u;            /* [ebp-0x2c] */
    uint32_t count = 0;                     /* [ebp-8]  */
    uint32_t i, k;
    uint32_t a[5];

    if (zx_off()) { wmain_00407d26_ref(cg); return; }
    { static int once; if (!once) { once = 1; atexit(zx_at_exit); } }
    zx_calls++;

    if (zrd(gm, gsz, ctx + 0x58u) > 0u) {   /* 00407d3d: jbe 0x407dd3 */
        gptr node;
        float s, c;
        a[0] = zrd(gm, gsz, E + 0x10u);                /* angle */
        a[1] = psin; a[2] = pcos;
        zcall(cg, wmain_00494238, sp, 0x00407d58u, 3, a);
        s = zldf(gm, gsz, psin) * zldf(gm, gsz, K_SCALE_A);
        c = zldf(gm, gsz, pcos) * zldf(gm, gsz, K_SCALE_A);
        zstf(gm, gsz, psin, s);
        zstf(gm, gsz, pcos, c);
        node = zrd(gm, gsz, ctx + 0x5cu);
        zwr(gm, gsz, G_DIRSRC, zrd(gm, gsz, node + 0x2cu));
        if (zrd(gm, gsz, G_DIRCLR) != 0u) zwr(gm, gsz, node + 0x2cu, 0u);
        zstf(gm, gsz, ctx + 0x15cu, zldf(gm, gsz, vec) - s);
        zstf(gm, gsz, ctx + 0x160u, zldf(gm, gsz, vec + 4u) + c);
        zstf(gm, gsz, ctx + 0x164u, zldf(gm, gsz, vec + 8u) + zldf(gm, gsz, K_SCALE_B));
    }

    /* 00407dd3: two CRT __ftol calls on the position. fld <float>; call __ftol; the callee pops. */
    cg->ftop = (cg->ftop - 1u) & 7u;
    cg->st[cg->ftop] = (double)zldf(gm, gsz, vec);
    zcall(cg, wmain_0049a44c, sp, 0x00407dddu, 0, a);
    {
        const uint32_t i0 = cg->r[EAX];
        cg->ftop = (cg->ftop - 1u) & 7u;
        cg->st[cg->ftop] = (double)zldf(gm, gsz, vec + 4u);
        zcall(cg, wmain_0049a44c, sp, 0x00407debu, 0, a);
        {
            const uint32_t i1 = cg->r[EAX];
            a[0] = world; a[1] = i0; a[2] = i1;
            zcall(cg, wmain_0040d8cb, sp, 0x00407e02u, 3, a);
        }
    }
    {
        const gptr tab = cg->r[EAX];            /* [ebp-0x18] */
        if (tab != 0u) {
            for (i = 0; i < (uint32_t)gmem_rd8_l(gm, gsz, tab); i++) {   /* 00407e2b */
                const gptr cell = tab + (i << 6) + 0x10u;                /* [ebp-0x34] */
                if ((uint16_t)gmem_rd16_l(gm, gsz, cell) == 0xffffu) continue;
                for (k = 0; k < (uint32_t)gmem_rd8_l(gm, gsz, cell + 0x25u); k++) {
                    const uint32_t idx = (uint16_t)gmem_rd16_l(gm, gsz, cell + 2u * k);
                    const gptr p = zrd(gm, gsz, world + 0x9cu) + (idx << 5);   /* [ebp-0x3c] */
                    const uint32_t c = gmem_rd8_l(gm, gsz, p + 0x1cu);           /* [ebp-0x30] */
                    if ((int32_t)c <= 0) continue;
                    if (zrd(gm, gsz, (c << 7) + 0x0059e034u) == 0u) continue;
                    if (zrd(gm, gsz, (c << 7) + 0x0059e02cu) != (uint32_t)gmem_rd8_l(gm, gsz, p + 0x1eu)) continue;
                    a[0] = ctx; a[1] = (c << 7) + 0x0059dfc0u; a[2] = vec; a[3] = count; a[4] = arg3;
                    zcall(cg, wmain_00408078, sp, 0x00407f00u, 5, a);
                    if (cg->r[EAX] != 0u) { count++; goto lists; }   /* 00407f10: jmp 0x407f1c */
                }
            }
        }
    }
lists:
    /* 00407f1c: the two object lists. */
    for (int pass = 0; pass < 2; pass++) {
        gptr ent = zrd(gm, gsz, world + (pass ? 0x1d8u : 0x1ccu));
        const uint32_t n = zrd(gm, gsz, world + (pass ? 0x1d0u : 0x1c4u));
        for (i = 0; i < n && count < 0x14u; i++, ent += 0x80u) {
            if (zrd(gm, gsz, ent + 0x74u) == 0u) continue;
            a[0] = ctx; a[1] = ent; a[2] = vec; a[3] = count; a[4] = arg3;
            zcall(cg, wmain_00408078, sp, pass ? 0x00407ff8u : 0x00407f7bu, 5, a);
            if (cg->r[EAX] != 0u) count++;
        }
    }
done:
    cg->r[ESP] = E + 4u;
    cg->r[EAX] = count;
}
