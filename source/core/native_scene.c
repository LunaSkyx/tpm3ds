/* Native scene helpers: 0x0040f170 (aggregate a byte over an object's parts) and
 * 0x0047cac0 (3x3 rotation matrix from Euler angles).
 *
 * 0x40f170 runs ~520 times per frame and 0x47cac0 ~215 (host fnhist, frames
 * 1800-2400); together they execute 4.7% of all lifted instructions
 * (tools/icount: 1.88% + 2.53%), nearly all of it call-site state sync.
 *
 * 0x40f170: uint __cdecl agg(obj *o, int off):
 *   n = o[0x14]; v = o[0x18]; or = 0; and = 0xff;
 *   for (i = 0; i < n; i++) { b = *(u8*)(v[i] + off); or |= b; and &= b; }
 *   return (and << 8) | or;
 * Straightforward; the original leaves EAX/ECX/EDX/EBX as the loop left them, so
 * those are reproduced too.
 *
 * 0x47cac0: void __cdecl rot(float out[12], const float angles[3], const float pos[3]):
 * three sincos() calls (guest 0x494238, already native) and nine products/sums in
 * the listing's exact order (docs/asm/0047cac0_listing.txt), each intermediate
 * rounded to float where the original stores it.
 *
 * Verification: tests/test_override.c, 5000 randomized cases each; registers, lazy
 * flags, fsw/fcw, x87 stack, guest memory and the stack window are compared.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_0040f170_ref(cpu_t *c);
void wmain_0047cac0_ref(cpu_t *c);
void wmain_00494238(cpu_t *c);            /* sincos(angle, *sin, *cos), native */

static int na_sc_off(void)
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

/* 0x0040f170 */
void wmain_0040f170(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    uint32_t E, eax, ecx, edx, ebx, esi, edi;

    if (na_sc_off()) { wmain_0040f170_ref(cg); return; }
    E = cg->r[ESP];
    ecx = gmem_rd32_l(gm, gsz, E + 4u);                 /* 0040f170: mov ecx,[esp+4] */
    eax = cg->r[EAX]; edx = cg->r[EDX]; ebx = cg->r[EBX];
    esi = 0u;                                           /* 0040f176: xor esi,esi */
    edi = gmem_rd32_l(gm, gsz, ecx + 0x14u);            /* 0040f178: count */
    edx = gmem_rd32_l(gm, gsz, ecx + 0x18u);            /* 0040f17b: pointer array */
    eax = 0xffu;                                        /* 0040f180 */
    if (edi != 0u) {
        uint32_t ebp = gmem_rd32_l(gm, gsz, E + 8u);    /* 0040f188: [esp+0x14] after 2 pushes */
        do {
            ecx = gmem_rd32_l(gm, gsz, edx);            /* 0040f18d */
            ebx = 0u;                                    /* 0040f18f: xor ebx,ebx */
            edx = (edx + 4u) & 0xffffffffu;             /* 0040f191 */
            ebx = (ebx & 0xffffff00u) | (uint32_t)gmem_rd8_l(gm, gsz, (ecx + ebp) & 0xffffffffu);  /* 0040f194: mov bl */
            ecx = ebx;                                  /* 0040f197 */
            esi |= ecx;                                 /* 0040f199 */
            eax &= ecx;                                 /* 0040f19b */
            edi = (edi - 1u) & 0xffffffffu;             /* 0040f19d */
        } while (edi != 0u);
    }
    eax = ((eax << 8) | esi) & 0xffffffffu;             /* 0040f1a2 */
    cg->r[EAX] = eax; cg->r[ECX] = ecx; cg->r[EDX] = edx;
    /* EBX/EBP/ESI/EDI are pushed and popped, i.e. they read back as the caller's. */
    cg->r[ESP] = E + 4u;
}

/* 0x0047cac0 */
void wmain_0047cac0(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    uint32_t E, eax, ecx, edx, out, ang, pos;
    gptr F;                                             /* scratch frame below E */
    float sin0, cos0, sin1, cos1, sin2, cos2;
    float t1, t2, t3, t4;                               /* [ebp-0x10], [ebp-0x1c], [ebp-0x18], [ebp-0x14] */

    if (na_sc_off()) { wmain_0047cac0_ref(cg); return; }
    E = cg->r[ESP];
    {
        uint32_t a0 = gmem_rd32_l(gm, gsz, E + 4u);     /* out */
        uint32_t a1 = gmem_rd32_l(gm, gsz, E + 8u);     /* angles */
        uint32_t a2 = gmem_rd32_l(gm, gsz, E + 12u);    /* pos */
        out = a0; ang = a1; pos = a2;
    }
    F = E - 0x40u;                                      /* private scratch, dead stack */
    /* three sincos(angle, &sin, &cos) calls, in the listing's order */
    {
        uint32_t args[3];
        args[0] = gmem_rd32_l(gm, gsz, ang);            /* angles[0] */
        args[1] = F + 0x00u;                            /* &sin0 */
        args[2] = F + 0x04u;                            /* &cos0 */
        eax = args[0];
        {   /* guest call: sincos */
            gptr s = F - 0x20u;
            uint32_t i;
            for (i = 0; i < 3u; i++) gmem_wr32_l(gm, gsz, s + 4u + 4u * i, args[i]);
            gmem_wr32_l(gm, gsz, s, 0x0047cad9u);
            cg->r[ESP] = s;
            wmain_00494238(cg);
            cg->r[ESP] = E;
        }
        args[0] = gmem_rd32_l(gm, gsz, ang + 4u);
        args[1] = F + 0x08u;                            /* &sin1 */
        args[2] = F + 0x0cu;                            /* &cos1 */
        {
            gptr s = F - 0x20u;
            uint32_t i;
            for (i = 0; i < 3u; i++) gmem_wr32_l(gm, gsz, s + 4u + 4u * i, args[i]);
            gmem_wr32_l(gm, gsz, s, 0x0047caf0u);
            cg->r[ESP] = s;
            wmain_00494238(cg);
            cg->r[ESP] = E;
        }
        args[0] = gmem_rd32_l(gm, gsz, ang + 8u);
        args[1] = F + 0x10u;                            /* &sin2 */
        args[2] = F + 0x14u;                            /* &cos2 */
        {
            gptr s = F - 0x20u;
            uint32_t i;
            for (i = 0; i < 3u; i++) gmem_wr32_l(gm, gsz, s + 4u + 4u * i, args[i]);
            gmem_wr32_l(gm, gsz, s, 0x0047cb07u);
            cg->r[ESP] = s;
            wmain_00494238(cg);
            cg->r[ESP] = E;
        }
    }
    { uint32_t v = gmem_rd32_l(gm, gsz, F + 0x00u); memcpy(&sin0, &v, 4); }
    { uint32_t v = gmem_rd32_l(gm, gsz, F + 0x04u); memcpy(&cos0, &v, 4); }
    { uint32_t v = gmem_rd32_l(gm, gsz, F + 0x08u); memcpy(&sin1, &v, 4); }
    { uint32_t v = gmem_rd32_l(gm, gsz, F + 0x0cu); memcpy(&cos1, &v, 4); }
    { uint32_t v = gmem_rd32_l(gm, gsz, F + 0x10u); memcpy(&sin2, &v, 4); }
    { uint32_t v = gmem_rd32_l(gm, gsz, F + 0x14u); memcpy(&cos2, &v, 4); }
    /* 0047cb0a: each product is rounded to float where the original stores it */
    t1 = (float)((double)cos1 * (double)cos2);          /* [ebp-0x10] */
    t2 = (float)((double)sin1 * (double)sin2);          /* [ebp-0x1c] */
    t3 = (float)((double)cos1 * (double)sin2);          /* [ebp-0x18] */
    t4 = (float)((double)sin1 * (double)cos2);          /* [ebp-0x14] */
    {
        float o0 = (float)(-(double)t2 * (double)sin0 + (double)t1);
        float o1 = (float)((double)t3 * (double)sin0 + (double)t4);
        float o2 = (float)(-(double)sin2 * (double)cos0);
        float o3 = (float)(-(double)sin1 * (double)cos0);
        float o4 = (float)((double)cos1 * (double)cos0);
        float o5 = sin0;
        float o6 = (float)((double)t4 * (double)sin0 + (double)t3);
        float o7 = (float)(-(double)sin0 * (double)t1 + (double)t2);   /* 0047cb8d: -(sin0*t1) + t2 */
        float o8 = (float)((double)cos2 * (double)cos0);              /* 0047cb9e */
        gmem_wr32_l(gm, gsz, out, gmem_u32_f(o0));
        gmem_wr32_l(gm, gsz, out + 4u, gmem_u32_f(o1));
        gmem_wr32_l(gm, gsz, out + 8u, gmem_u32_f(o2));
        gmem_wr32_l(gm, gsz, out + 12u, gmem_u32_f(o3));
        gmem_wr32_l(gm, gsz, out + 16u, gmem_u32_f(o4));
        gmem_wr32_l(gm, gsz, out + 20u, gmem_u32_f(o5));
        gmem_wr32_l(gm, gsz, out + 24u, gmem_u32_f(o6));
        gmem_wr32_l(gm, gsz, out + 28u, gmem_u32_f(o7));
        gmem_wr32_l(gm, gsz, out + 32u, gmem_u32_f(o8));
    }
    /* 0047cb8e onward: translation */
    gmem_wr32_l(gm, gsz, out + 36u, gmem_rd32_l(gm, gsz, pos));
    gmem_wr32_l(gm, gsz, out + 40u, gmem_rd32_l(gm, gsz, pos + 4u));
    gmem_wr32_l(gm, gsz, out + 44u, gmem_rd32_l(gm, gsz, pos + 8u));
    /* The reference's last x87 value (fld cos2; fmul cos0; fstp dword) is still in the
     * slot it used, and its epilogue writes that slot back: reproduce the leak. */
    cg->st[(cg->ftop + 7u) & 7u] = (double)cos2 * (double)cos0;
    eax = gmem_rd32_l(gm, gsz, pos + 4u);              /* 0047cbb7: eax = pos[1] */
    ecx = (out + 0x24u) & 0xffffffffu;                 /* 0047cbad */
    edx = gmem_rd32_l(gm, gsz, pos + 8u);              /* 0047cbbd: edx = pos[2] */
    cg->r[EAX] = eax; cg->r[ECX] = ecx; cg->r[EDX] = edx;
    cg->r[ESP] = E + 4u;
}


