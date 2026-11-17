/* Native animated-texture frame selector, guest 0x0041ae50 (perf: ~6.9% of the
 * samples in the steady level-1 window, sampler build 2026-11-14).
 *
 * void __cdecl animtex(world *w, poly *p)
 *   w+0x50  u32  world time (ms)          p+0x08 i16 current texture index
 *   w+0xb0  i32  texture count            p+0x2c u8  flags (bit 0x10: no ping-pong)
 *   w+0xb8  ptr  texture table (0x28 B)   p+0x32 i16 frames per texture (period)
 *                                         p+0x34 u8  texture frame count
 *                                         p+0x35 u8  current frame (result)
 *
 * The original divides a 32-bit value by 16/32-bit values with `div` and then
 * walks the texture table counting entries with the same id. Transcribed from
 * source/gen/wmain_gen_004.c (wmain_0041ae50_ref) so that the partial-register
 * results (EAX/ECX/EDX are caller-saved, but the differential test compares
 * them) and the early-return paths match the lifted body exactly.
 *
 * The lazy-flag state is deliberately NOT written on the normal path: the lifted
 * body's lean call ABI drops it (no caller reads flags after this call), so the
 * native must leave it alone to stay observably identical.
 *
 * Verification: tests/test_override.c ("0041ae50") runs native and lifted
 * reference on identical randomized input and compares all registers, the flag
 * fields, memory and the stack window.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_0041ae50_ref(cpu_t *c);          /* lifted body (ALWAYS_REF: A/B + fallback) */

/* A/B switch: the 3DS build falls back to the lifted body when nonative.txt exists,
 * the host build when TPM_AX_OFF is in the environment (golden A/B runs). */
static int na_ax_off(void)
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

/* x86 `div` raises #DE on a zero divisor. It cannot happen with this function's
 * data (the period is forced non-zero first), but the lifted body traps, so the
 * native keeps the same observable behaviour (including the lazy flag state the
 * trap path saves). */
static void na_div_trap(cpu_t *cg, uint32_t ax, uint32_t eax, uint32_t edx, uint32_t ebx,
                        uint32_t ebp, uint32_t esi, uint32_t edi, uint32_t ecx, uint32_t pc)
{
    cg->fop = FL_LOGIC; cg->fbits = 16;
    cg->fa = ax; cg->fb = ax; cg->fr = ax & 0xffffu;
    cg->r[EAX] = eax; cg->r[ECX] = ecx; cg->r[EDX] = edx; cg->r[EBX] = ebx;
    cg->r[EBP] = ebp; cg->r[ESI] = esi; cg->r[EDI] = edi;
    cpu_trap(cg, pc, "divide error");
}

void wmain_0041ae50(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const uint32_t E = cg->r[ESP];
    uint32_t eax = cg->r[EAX], ecx = cg->r[ECX], edx = cg->r[EDX];
    uint32_t ebx = cg->r[EBX], ebp = cg->r[EBP], esi = cg->r[ESI], edi = cg->r[EDI];
    /* EBX/EBP/ESI/EDI are pushed on entry and popped again: they read back as the
     * caller's values whatever the body did. */
    const uint32_t r_ebx = ebx, r_ebp = ebp, r_esi = esi, r_edi = edi;
    uint32_t ax16;

    if (na_ax_off()) { wmain_0041ae50_ref(cg); return; }
    /* 0041ae50: push ebx/ebp/esi/edi (restored below; dead stack, not modelled) */
    /* 0041ae54: mov edi,[esp+0x18] -> poly */
    edi = gmem_rd32_l(gm, gsz, E + 8u);
    /* 0041ae58: mov cl,byte [edi+0x34] */
    ecx = (ecx & 0xffffff00u) | (uint32_t)gmem_rd8_l(gm, gsz, edi + 0x34u);
    /* 0041ae5b: cmp cl,1 / jbe 0x41af1c */
    if ((uint8_t)ecx <= 1u) goto L_done;
    /* 0041ae64: mov ax,word [edi+8] */
    eax = (eax & 0xffff0000u) | (uint32_t)gmem_rd16_l(gm, gsz, edi + 8u);
    /* 0041ae68: test ax,ax / jl 0x41af1c */
    if ((int16_t)(uint16_t)eax < 0) goto L_done;
    /* 0041ae71: mov ebx,[esp+0x14] -> world */
    ebx = gmem_rd32_l(gm, gsz, E + 4u);
    /* 0041ae75: movsx ebp,ax */
    ebp = (uint32_t)(int32_t)(int16_t)(uint16_t)eax;
    /* 0041ae78: cmp ebp,[ebx+0xb0] / jg 0x41af1c */
    if ((int32_t)ebp > (int32_t)gmem_rd32_l(gm, gsz, ebx + 0xb0u)) goto L_done;
    /* 0041ae84: cmp word [edi+0x32],0 / jne ; else word [edi+0x32]=1 */
    if (gmem_rd16_l(gm, gsz, edi + 0x32u) == 0u) gmem_wr16_l(gm, gsz, edi + 0x32u, 1u);
    /* 0041ae91: mov ax,word [edi+0x32] */
    eax = (eax & 0xffff0000u) | (uint32_t)gmem_rd16_l(gm, gsz, edi + 0x32u);
    ax16 = eax & 0xffffu;
    /* 0041ae95: test ax,ax ; 41ae98 movsx esi,ax ; 41ae9b jge ; 41ae9d neg esi */
    esi = (uint32_t)(int32_t)(int16_t)(uint16_t)ax16;
    if ((int16_t)(uint16_t)ax16 < 0) esi = 0u - esi;
    /* 0041ae9f: mov eax,[ebx+0x50] ; xor edx,edx ; div esi */
    eax = gmem_rd32_l(gm, gsz, ebx + 0x50u);
    edx = 0u;
    if (esi == 0u) { na_div_trap(cg, ax16, eax, edx, ebx, ebp, esi, edi, ecx, 0x0041aea4u); return; }
    {   /* EDX is zero, so the quotient always fits in 32 bits */
        uint32_t n = eax, d = esi;
        eax = n / d;
        edx = n % d;
    }
    /* 0041aea6: test byte [edi+0x2c],0x10 */
    if (((uint8_t)gmem_rd8_l(gm, gsz, edi + 0x2cu) & 0x10u) == 0u) goto L_pingpong;
    /* 0041aeac: and ecx,0xff ; xor edx,edx ; div ecx */
    ecx &= 0xffu;
    edx = 0u;
    if (ecx == 0u) { na_div_trap(cg, ax16, eax, edx, ebx, ebp, esi, edi, ecx, 0x0041aeb4u); return; }
    {
        uint32_t n = eax, d = ecx;
        eax = n / d;
        edx = n % d;
    }
    /* 0041aeb6: mov [edi+0x35],dl */
    gmem_wr8_l(gm, gsz, edi + 0x35u, (uint8_t)(edx & 0xffu));
    goto L_aed7;

L_pingpong:
    /* 0041aebb: mov esi,ecx ; and esi,0xff ; lea ecx,[esi+esi-2] */
    esi = ecx & 0xffu;
    ecx = (esi + esi - 2u) & 0xffffffffu;
    edx = 0u;
    /* 0041aec9: div ecx */
    if (ecx == 0u) { na_div_trap(cg, ax16, eax, edx, ebx, ebp, esi, edi, ecx, 0x0041aec9u); return; }
    {
        uint32_t n = eax, d = ecx;
        eax = n / d;
        edx = n % d;
    }
    /* 0041aecb: cmp edx,esi ; mov [edi+0x35],dl ; jb */
    gmem_wr8_l(gm, gsz, edi + 0x35u, (uint8_t)(edx & 0xffu));
    if (edx < esi) goto L_aed7;
    /* 0041aed2: sub cl,dl (8-bit) ; mov [edi+0x35],cl */
    {
        uint32_t r = ((ecx & 0xffu) - (edx & 0xffu)) & 0xffu;
        ecx = (ecx & 0xffffff00u) | r;
        gmem_wr8_l(gm, gsz, edi + 0x35u, (uint8_t)r);
    }

L_aed7:
    /* 0041aed7: mov edx,[ebx+0xb8] ; mov eax,ebp ; xor ebp,ebp ; lea ecx,[eax+eax*4]
     *           lea ecx,[edx+ecx*8] ; xor edx,edx ; mov dl,[edi+0x35] ; add ecx,0x20 */
    edx = gmem_rd32_l(gm, gsz, ebx + 0xb8u);
    eax = ebp;
    ebp = 0u;
    ecx = (eax + eax * 4u) & 0xffffffffu;
    ecx = (edx + ecx * 8u) & 0xffffffffu;
    edx = (uint32_t)gmem_rd8_l(gm, gsz, edi + 0x35u);
    ecx = (ecx + 0x20u) & 0xffffffffu;
    /* 0041aeef: test edx,edx ; mov esi,[ecx] ; jbe 0x41af10 */
    esi = gmem_rd32_l(gm, gsz, ecx);
    if (edx == 0u) goto L_af10;
    /* 0041aef5: mov ebx,[ebx+0xb0] */
    ebx = gmem_rd32_l(gm, gsz, ebx + 0xb0u);
L_aefb:
    /* 0041aefb: cmp eax,ebx / jge 0x41af0c */
    if ((int32_t)eax >= (int32_t)ebx) goto L_af0c;
    /* 0041aeff: add ecx,0x28 ; inc eax ; cmp [ecx],esi ; jne */
    ecx = (ecx + 0x28u) & 0xffffffffu;
    eax = (eax + 1u) & 0xffffffffu;
    if (gmem_rd32_l(gm, gsz, ecx) == esi) ebp = (ebp + 1u) & 0xffffffffu;   /* 0041af07: inc ebp */
    /* 0041af08: cmp ebp,edx / jb 0x41aefb */
    if (ebp < edx) goto L_aefb;
L_af0c:
    ebx = gmem_rd32_l(gm, gsz, E + 4u);           /* 0041af0c: mov ebx,[esp+0x14] */
L_af10:
    /* 0041af10: cmp eax,[ebx+0xb0] / jge 0x41af1c */
    if ((int32_t)eax >= (int32_t)gmem_rd32_l(gm, gsz, ebx + 0xb0u)) goto L_done;
    /* 0041af18: mov word [edi+8],ax */
    gmem_wr16_l(gm, gsz, edi + 8u, (uint16_t)(eax & 0xffffu));

L_done:
    cg->r[EAX] = eax; cg->r[ECX] = ecx; cg->r[EDX] = edx;
    cg->r[EBX] = r_ebx; cg->r[EBP] = r_ebp; cg->r[ESI] = r_esi; cg->r[EDI] = r_edi;
    cg->r[ESP] = E + 4u;                              /* 0041af20: ret */
}

/* ---------------------------------------------------------------------------
 * Native object-key list insert, guest 0x00401eb8 (4.9% of the lifted
 * instructions in the steady window; icount build 2026-11-14).
 *
 * int __cdecl insert(obj *o):
 *   0x4dd6c0  ptr    last used bucket (cache)
 *   0x4dd6e0  int    number of inserted objects
 *   0x5956f8  bucket array, 0xc bytes each ({ key, head, tail })
 *   0x5959f8  int    bucket count (<= 0x40)
 *   0x8bfba0  u8     byte stored at o+0x17
 * The original scans the bucket array linearly for a matching key; the native
 * keeps the same scan, order and side effects. Flags are not written back (the
 * lifted body's lean ABI drops them); EAX/ECX/EDX/ESP/EBP are reproduced.
 * ------------------------------------------------------------------------- */
void wmain_00401eb8_ref(cpu_t *c);

void wmain_00401eb8(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const uint32_t E = cg->r[ESP];
    const uint32_t r_ebp = cg->r[EBP];
    uint32_t eax = cg->r[EAX], ecx = cg->r[ECX], edx = cg->r[EDX], ebp = E - 4u;
    uint32_t l4 = 0, l8 = 0, lc = 0, l10 = 0;    /* [ebp-4], [ebp-8], [ebp-0xc], [ebp-0x10] */
    uint32_t arg;

    if (na_ax_off()) { wmain_00401eb8_ref(cg); return; }

    arg = gmem_rd32_l(gm, gsz, E + 4u);                  /* 00401ec5: [ebp+8] */
    if (arg == 0u) { eax = 0u; goto L_end; }             /* 00401ecb */
    {                                                    /* 00401ed2: o+0x17 = [0x8bfba0] */
        uint32_t cl = (uint32_t)gmem_rd8_l(gm, gsz, 0x008bfba0u);
        ecx = (ecx & 0xffffff00u) | cl;
        gmem_wr8_l(gm, gsz, arg + 0x17u, (uint8_t)cl);
    }
    edx = arg;                                           /* 00401ede */
    eax = gmem_rd32_l(gm, gsz, edx + 4u);                /* 00401ee1 */
    l10 = eax;
    if (eax == 0u) { eax = 0u; goto L_end; }             /* 00401eed */
    ecx = l10;                                           /* 00401ef4 */
    edx = gmem_rd32_l(gm, gsz, ecx + 0x20u);             /* 00401ef7 */
    lc = edx;
    if (edx == 0u) { eax = 0u; goto L_end; }             /* 00401f03 */
    l4 = 0u;                                             /* 00401f0a */
    if (gmem_rd32_l(gm, gsz, 0x004dd6c0u) == 0u) goto L_1f31;   /* 00401f11 */
    eax = gmem_rd32_l(gm, gsz, 0x004dd6c0u);             /* 00401f1a */
    ecx = gmem_rd32_l(gm, gsz, eax);
    if (ecx != lc) goto L_1f31;                          /* 00401f21/24 */
    edx = gmem_rd32_l(gm, gsz, 0x004dd6c0u);             /* 00401f26 */
    l4 = edx;
    goto L_1f7d;                                         /* 00401f2f */
L_1f31:
    l8 = 0u;                                             /* 00401f31 */
L_1f43:
    ecx = l8;                                            /* 00401f43 */
    if ((int32_t)ecx >= (int32_t)gmem_rd32_l(gm, gsz, 0x005959f8u)) goto L_1f6b;   /* 00401f46 */
    edx = l8 * 12u;                                      /* 00401f51: imul */
    edx = (edx + 0x005956f8u) & 0xffffffffu;             /* 00401f54 */
    l4 = edx;
    eax = l4;                                            /* 00401f5d */
    ecx = gmem_rd32_l(gm, gsz, eax);
    if (ecx != lc) goto L_1f69;                          /* 00401f62/65 */
    goto L_1f6b;                                         /* 00401f67 */
L_1f69:
    l8 = (l8 + 1u) & 0xffffffffu;                        /* 00401f3a */
    goto L_1f43;
L_1f6b:
    edx = l8;                                            /* 00401f6b */
    if ((int32_t)edx < (int32_t)gmem_rd32_l(gm, gsz, 0x005959f8u)) goto L_1f7d;    /* 00401f6e */
    l4 = 0u;                                             /* 00401f76 */
L_1f7d:
    if (l4 != 0u) goto L_1fbe;                           /* 00401f7d */
    eax = gmem_rd32_l(gm, gsz, 0x005959f8u);             /* 00401f83 */
    eax = (eax * 12u + 0x005956f8u) & 0xffffffffu;       /* 00401f88/8b */
    l4 = eax;
    ecx = gmem_rd32_l(gm, gsz, 0x005959f8u);             /* 00401f93 */
    ecx = (ecx + 1u) & 0xffffffffu;                      /* 00401f99 */
    gmem_wr32_l(gm, gsz, 0x005959f8u, ecx);              /* 00401f9c */
    if ((int32_t)gmem_rd32_l(gm, gsz, 0x005959f8u) <= 0x40) goto L_1fbe;   /* 00401fa2 */
    edx = (gmem_rd32_l(gm, gsz, 0x005959f8u) - 1u) & 0xffffffffu;          /* 00401fab */
    gmem_wr32_l(gm, gsz, 0x005959f8u, edx);              /* 00401fb4 */
    eax = 0u;                                            /* 00401fba */
    goto L_end;
L_1fbe:
    eax = arg;                                           /* 00401fbe */
    gmem_wr32_l(gm, gsz, eax, 0u);                       /* 00401fc1: *o = 0 */
    ecx = l4;                                            /* 00401fc7 */
    if (gmem_rd32_l(gm, gsz, ecx + 4u) != 0u) goto L_1fec;   /* 00401fca */
    edx = l4;                                            /* 00401fd0 */
    eax = lc;
    gmem_wr32_l(gm, gsz, edx, eax);                      /* 00401fd6: bucket.key = key */
    ecx = l4;                                            /* 00401fd8 */
    edx = arg;
    gmem_wr32_l(gm, gsz, ecx + 4u, edx);                 /* 00401fde: bucket.head = o */
    eax = l4;                                            /* 00401fe1 */
    ecx = arg;
    gmem_wr32_l(gm, gsz, eax + 8u, ecx);                 /* 00401fe7: bucket.tail = o */
    goto L_2000;
L_1fec:
    edx = l4;                                            /* 00401fec */
    eax = gmem_rd32_l(gm, gsz, edx + 8u);
    ecx = arg;
    gmem_wr32_l(gm, gsz, eax, ecx);                      /* 00401ff5: *tail = o */
    edx = l4;                                            /* 00401ff7 */
    eax = arg;
    gmem_wr32_l(gm, gsz, edx + 8u, eax);                 /* 00401ffd: bucket.tail = o */
L_2000:
    ecx = l4;                                            /* 00402000 */
    gmem_wr32_l(gm, gsz, 0x004dd6c0u, ecx);
    edx = gmem_rd32_l(gm, gsz, 0x004dd6e0u);             /* 00402009 */
    edx = (edx + 1u) & 0xffffffffu;                      /* 0040200f */
    gmem_wr32_l(gm, gsz, 0x004dd6e0u, edx);              /* 00402012 */
    eax = 1u;                                            /* 00402018 */
L_end:
    (void)ebp; (void)l4; (void)l8;
    cg->r[EAX] = eax; cg->r[ECX] = ecx; cg->r[EDX] = edx;
    cg->r[EBP] = r_ebp;                                  /* 0040201f: pop ebp */
    cg->r[ESP] = E + 4u;                                 /* 00402020: ret */
}
