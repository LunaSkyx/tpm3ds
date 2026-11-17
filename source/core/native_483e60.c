/* Native 0x00483e60: the skinned-vertex transform.
 *
 * 663 instructions, 730 stream lines, 9 call sites, 3 calls/frame, 2.10 ms/frame (docs/PLAN.md
 * section 12 hands this one over from the skeleton native).  A semantic transcription of the
 * instruction stream in build-host/skin_stream.txt (the authority; there is no docs/asm listing
 * for this function), with the lifted body (wmain_00483e60_ref) as the executable reference.
 *
 * What it does: the argument (arg0, the pose/skin context) carries four sub-object records
 * (stride 0x14c from arg0+8, count 4 at [esp+0x14]) and a list of 0xb4-byte vertex records
 * (count at arg0+0x54, base at arg0+0x58).
 *  1. 0x483e98..0x483f74 - the keyframe search: for each of the four sub-objects, if its
 *     animation record [sub+0x128] is present and its count [rec+0x38] is non-zero, walk the
 *     0x2c-stride entries at [rec+0x3c]+0x24 and advance the key index [sub+0x20+i*4] to the
 *     first keyframe whose time (0x38-stride records, float at +0) is above the current time
 *     [sub+0x120], stopping at count-1 (an index already there is left alone): it re-brackets
 *     the pair of keys the interpolation uses.
 *  2. 0x483f7a..0x48463e - the main pass: for each of the arg0+0x54 vertex records (0xb4 stride,
 *     [edi]) and each of the four sub-objects, resolve the animation entry ([rec+0x3c] +
 *     [record]*0x2c) and, if its +0x24 pointer and the record's flag byte [esi] bit 1 allow it,
 *     the key sample at [entry+0x28] + key*0x38 (key = [sub+0x20+i*4]): the sample's +0 time
 *     gives the weight `[sub+0x120] - time`, +4 is a flags word, +8..0x10 and +0x14..0x1c are
 *     two origins and +0x20..0x28 / +0x2c..0x34 two 3-vectors (flag bit 0 uses the first pair,
 *     bit 1 the second).  The record's vertex range ([sub+8] or [sub+0xc], picked by
 *     [entry+0x2c] & [edi+4]) is resolved against the frame's accumulated range
 *     ([esp+0x18]/[esp+0x24]) to decide "blend into the accumulator" vs "start a new sample";
 *     the sample is moved against [edi+0x28..0x3c], its six components are wrapped by 0x49418b
 *     (angle wrap), and the blend of the previous sample ([esp+0x68..0x7c]) with the current
 *     one and the two weights ([esp+0x14], [esp+0x10]) happens at
 *     0x484200/0x48426e/0x4842ee/0x484331 into the accumulators [esp+0x38..0x40] (angles) and
 *     [esp+0x44..0x4c] (position).
 *  3. 0x4843af..0x4845c3 - the two- or one-sided extrapolation of those accumulators (with the
 *     six values at [edi+0x28..0x3c] folded in), six more 0x49418b wraps, then
 *     0x47cac0(out_matrix, angles, position) into the vertex record: out = [arg0+0x20] +
 *     arg0+8 + i*0x30, angles = S+0x38, position = S+0x44.  Note that the third 0x49418b
 *     argument is pushed at 0x484541 and only dropped at 0x4845a9, so every [esp+N] in between
 *     is 4 below the frame's steady value (0x484589 therefore reads arg0 from [esp+0x88]).
 *  4. 0x4845c8..0x4846b0 - if the transformed vertex is not the origin, 0x47e0cd(vertex, orig)
 *     fixes it up; then the record is submitted with 0x4852b3(arg0, arg0+0x58, arg1) and
 *     [record+0x1c] gets the global [0x4b8860].  The 0x484646 path is the fallback when
 *     [arg0+0x18] is null or its first dword is set: it copies 0x30 bytes (a 4x3 matrix) per
 *     record from [[arg0+4]+0x58]+0x84 + i*0xb4 (rep movsd, count [[arg0+4]+0x54]) to
 *     [arg0+0x20] + i*0x30, and submits the same way.
 *
 * Contract (why this is a transcription, not a lifted-body copy)
 * ---------------------------------------------------------------
 * The original is `sub esp,0x70; push ebx/ebp/esi/edi` with no frame pointer, so the whole
 * function runs at one guest stack frame.  This file keeps ESP in a C local and mirrors every
 * push/sub/add literally, so every [esp+N] slot is the reference's address - including the
 * places where an outstanding argument push shifts them by 4 (0x484525, 0x484536 and the whole
 * 0x484547..0x4845a9 block run with the third 0x49418b argument still pushed, which is why
 * 0x484589 reads arg0 from [esp+0x88] and 0x484680 reads it from [esp+0x84]).
 *
 * The x87 stack is an explicit physical eight-slot model in doubles (xs[] + xt, st(k) =
 * xs[(xt+k)&7]), initialised from cg->st[]/cg->ftop and written back at the epilogue, so the
 * arithmetic is the reference's own (values stay double until an fstp narrows them) and the call
 * sites see and leave the same x87 state.  At every call all integer registers and all eight
 * slots are published to cg and read back afterwards: that is the hardware state, which is also
 * what the reference body's per-call facts reproduce.  Every fcom/fcomp here needs its FPOP()
 * (there are 11 of them, all `fcomp`), every `fstp st(n)` needs its FPOP(), and `fsub dword
 * ptr [mem]` is st(0) = st(0) - mem while `fsubr` (not used here) would be the other way round.
 *
 * The original pushes ebx/ebp/esi/edi and restores them in the epilogue, so the native restores
 * all four.  EAX/ECX/EDX are the body's final values and ESP = entry ESP + 4.  Flags: this native
 * does not publish cg->eflags; every path ends in the 0x4852b3 call and only movs follow it, so
 * cg->eflags is that callee's final state, exactly as the reference leaves it.  cg->fsw is
 * maintained through FCOM() so the C0/C3 tests are the emulated ones.
 *
 * A/B: TPM_AX_OFF (host) / nonative.txt (3DS) falls back to wmain_00483e60_ref; the call counter
 * below prints on exit, so a passing A/B cannot be a never-called one.  Verified:
 * tests/scenarios/level1_bench.txt 0 diff, tests/scenarios/golden_long.txt 0 diff, test_override
 * and test_imuse green.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_00483e60_ref(cpu_t *c);
void wmain_0047cac0(cpu_t *c);      /* 3x3(+pos) from Euler angles (source/core/native_scene.c) */
void wmain_0047e0cd(cpu_t *c);
void wmain_004852b3(cpu_t *c);
void wmain_0049418b(cpu_t *c);      /* angle wrap to (-180,180] (source/core/native_angle.c) */

/* A/B switch, same policy as the other natives (TPM_AX_OFF on the host, nonative.txt /
 * g_native_off on the device). */
static int sv_off(void)
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

static uint64_t sv_calls;
static void sv_at_exit(void) { fprintf(stderr, "skinned-vertex native: %llu calls\n", (unsigned long long)sv_calls); }

#define RD(a)       gmem_rd32_l(gm, gsz, (a))
#define WR(a, v)    gmem_wr32_l(gm, gsz, (a), (uint32_t)(v))
#define RD8(a)      gmem_rd8_l(gm, gsz, (a))
#define WR8(a, v)   gmem_wr8_l(gm, gsz, (a), (uint8_t)(v))
#define RDF(a)      ((double)gmem_f32_l(RD(a)))
#define WRF(a, v)   WR((a), gmem_u32_f((float)(v)))
#define PUSH(v)     do { esp -= 4u; WR(esp, (v)); } while (0)

/* x87: physical stack, xs[(xt + k) & 7] == st(k).  Values stay double until an fstp/fst narrows
 * them, exactly like the reference's static slots. */
#define XS(k)       xs[(xt + (uint32_t)(k)) & 7u]
#define FPUSH(v)    do { double v_ = (v); xt = (xt - 1u) & 7u; xs[xt] = v_; } while (0)
#define FPOP()      do { xt = (xt + 1u) & 7u; } while (0)
#define FXCH(k)     do { double t_ = XS(0); XS(0) = XS(k); XS(k) = t_; } while (0)
#define FST32(a)    WRF((a), XS(0))
#define FSTP32(a)   do { WRF((a), XS(0)); FPOP(); } while (0)
#define FSTP_ST(k)  do { XS(k) = XS(0); FPOP(); } while (0)   /* fstp st(k) */
#define FLD32(a)    FPUSH(RDF(a))
#define FCOM_M(a)   FCOM(cg, XS(0), RDF(a))
#define FNSTSW_AX() do { eax = (eax & 0xffff0000u) | (cpu_fsw_at(cg, xt) & 0xffffu); } while (0)

/* One cdecl guest call: the argument pushes are the literal PUSH() lines above, this only pushes
 * the return address, publishes the register/x87 state, calls and reloads.  ESP after the call is
 * the caller's ESP again (the callee's `ret` popped the address). */
#define ZCALL(fn_, ret_)                                                        \
    do {                                                                        \
        uint32_t zs_ = esp - 4u;                                                \
        WR(zs_, (ret_));                                                        \
        esp = zs_;                                                              \
        cg->r[EAX] = eax; cg->r[ECX] = ecx; cg->r[EDX] = edx;                   \
        cg->r[EBX] = ebx; cg->r[EBP] = ebp; cg->r[ESI] = esi;                   \
        cg->r[EDI] = edi; cg->r[ESP] = esp;                                     \
        memcpy(cg->st, xs, sizeof xs);                                          \
        cg->ftop = xt;                                                          \
        (fn_)(cg);                                                              \
        esp += 4u;                                                              \
        eax = cg->r[EAX]; ecx = cg->r[ECX]; edx = cg->r[EDX];                   \
        ebx = cg->r[EBX]; ebp = cg->r[EBP]; esi = cg->r[ESI]; edi = cg->r[EDI]; \
        memcpy(xs, cg->st, sizeof xs);                                          \
        xt = cg->ftop;                                                          \
    } while (0)

static void sv_body(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi, esp;
    double xs[8];
    uint32_t xt;
    int32_t df;                       /* rep movsd direction step; the function never changes DF */

    eax = cg->r[EAX]; ecx = cg->r[ECX]; edx = cg->r[EDX];
    ebx = cg->r[EBX]; ebp = cg->r[EBP]; esi = cg->r[ESI]; edi = cg->r[EDI];
    esp = cg->r[ESP];
    memcpy(xs, cg->st, sizeof xs);
    xt = cg->ftop;
    df = (cg->eflags & F_DF) ? -4 : 4;

    /* 00483e60: sub esp, 0x70 */
    esp -= 0x70u;
    /* 00483e63: mov ecx, dword ptr [esp + 0x74]   (arg0: the skin/pose context) */
    ecx = RD(esp + 0x74u);
    /* 00483e67: push ebx */
    PUSH(ebx);
    /* 00483e68: push ebp */
    PUSH(ebp);
    /* 00483e69: xor edx, edx */
    edx = 0;
    /* 00483e6b: mov eax, dword ptr [ecx + 0x18] */
    eax = RD(ecx + 0x18u);
    /* 00483e6e: mov ebp, dword ptr [ecx + 4] */
    ebp = RD(ecx + 4u);
    /* 00483e71: push esi */
    PUSH(esi);
    /* 00483e72: cmp eax, edx / 00483e79: je 0x484646 */
    /* 00483e74: push edi */
    PUSH(edi);
    /* 00483e75: mov dword ptr [esp + 0x20], ebp */
    WR(esp + 0x20u, ebp);
    if (eax == edx) goto L_00484646;

    /* 00483e7f: cmp dword ptr [eax], edx / 00483e81: jne 0x484646 */
    if (RD(eax) != edx) goto L_00484646;
    /* 00483e87: add eax, 8 */
    eax += 8u;
    /* 00483e8a: mov dword ptr [esp + 0x14], 4 */
    WR(esp + 0x14u, 4u);
    /* 00483e92: mov dword ptr [esp + 0x34], eax */
    WR(esp + 0x34u, eax);
    /* 00483e96: mov ebx, eax */
    ebx = eax;

L_00483e98:
    /* 00483e98: cmp dword ptr [ebx], edx / 00483e9a: je 0x483f65 */
    if (RD(ebx) == edx) goto L_00483f65;
    /* 00483ea0: mov ebp, dword ptr [ebx + 0x128] */
    ebp = RD(ebx + 0x128u);
    /* 00483ea6: lea eax, [ebx + 0x20] */
    eax = ebx + 0x20u;
    /* 00483ea9: mov dword ptr [esp + 0x2c], ebp */
    WR(esp + 0x2cu, ebp);
    /* 00483ead: mov dword ptr [esp + 0x1c], edx */
    WR(esp + 0x1cu, edx);
    /* 00483eb1: mov esi, dword ptr [ebp + 0x38] */
    esi = RD(ebp + 0x38u);
    /* 00483eb4: mov ecx, dword ptr [ebp + 0x3c] */
    ecx = RD(ebp + 0x3cu);
    /* 00483eb7: cmp esi, edx / 00483eb9: jbe 0x483f65 */
    if (esi <= edx) goto L_00483f65;

    /* 00483ebf: lea esi, [ecx + 0x24] */
    esi = ecx + 0x24u;
    /* 00483ec2: mov dword ptr [esp + 0x18], eax */
    WR(esp + 0x18u, eax);

L_00483ec6:
    /* 00483ec6: mov ecx, dword ptr [esi] */
    ecx = RD(esi);
    /* 00483ec8: xor edi, edi */
    edi = 0;
    /* 00483eca: test ecx, ecx / 00483ecc: je 0x483f41 */
    if (ecx == 0u) goto L_00483f41;

    /* 00483ece: mov eax, dword ptr [esi - 4] */
    eax = RD(esi - 4u);
    /* 00483ed1: dec ecx */
    ecx--;
    /* 00483ed2: mov dword ptr [esp + 0x30], ecx */
    WR(esp + 0x30u, ecx);
    /* 00483ed6: mov eax, dword ptr [ebx + eax*4 + 0x20] */
    eax = RD(ebx + eax * 4u + 0x20u);
    /* 00483eda: cmp eax, ecx / 00483edc: je 0x483f41 */
    if (eax == ecx) goto L_00483f41;

    /* 00483ede: mov edx, dword ptr [esi + 4] */
    edx = RD(esi + 4u);
    /* 00483ee1: lea ecx, [eax + 1] */
    ecx = eax + 1u;
    /* 00483ee4: fld dword ptr [ebx + 0x120] */
    FLD32(ebx + 0x120u);
    /* 00483eea: lea eax, [ecx*8] */
    eax = ecx * 8u;
    /* 00483ef1: sub eax, ecx */
    eax -= ecx;
    /* 00483ef3: fcomp dword ptr [edx + eax*8] */
    FCOM(cg, XS(0), RDF(edx + eax * 8u));
    FPOP();
    /* 00483ef6: fnstsw ax / 00483ef8: test ah, 1 / 00483efb: jne 0x483f41 */
    FNSTSW_AX();
    if (((eax >> 8) & 1u) != 0u) goto L_00483f41;

    /* 00483efd: lea eax, [ecx + 1] */
    eax = ecx + 1u;
    /* 00483f00: mov ebp, eax */
    ebp = eax;
    /* 00483f02: shl ebp, 3 */
    ebp <<= 3;
    /* 00483f05: sub ebp, eax */
    ebp -= eax;
    /* 00483f07: lea edx, [edx + ebp*8] */
    edx = edx + ebp * 8u;
    /* 00483f0a: mov ebp, dword ptr [esp + 0x30] */
    ebp = RD(esp + 0x30u);

L_00483f0e:
    /* 00483f0e: cmp ecx, ebp / 00483f10: jne 0x483f19 */
    if (ecx != ebp) goto L_00483f19;
    /* 00483f12: mov edi, 1 */
    edi = 1u;
    /* 00483f17: jmp 0x483f33 */
    goto L_00483f33;

L_00483f19:
    /* 00483f19: fld dword ptr [ebx + 0x120] */
    FLD32(ebx + 0x120u);
    /* 00483f1f: fcomp dword ptr [edx] */
    FCOM(cg, XS(0), RDF(edx));
    FPOP();
    /* 00483f21: fnstsw ax / 00483f23: test ah, 1 / 00483f26: je 0x483f2f */
    FNSTSW_AX();
    if (((eax >> 8) & 1u) == 0u) goto L_00483f2f;
    /* 00483f28: mov edi, 1 */
    edi = 1u;
    /* 00483f2d: jmp 0x483f33 */
    goto L_00483f33;

L_00483f2f:
    /* 00483f2f: inc ecx */
    ecx++;
    /* 00483f30: add edx, 0x38 */
    edx += 0x38u;

L_00483f33:
    /* 00483f33: test edi, edi / 00483f35: je 0x483f0e */
    if (edi == 0u) goto L_00483f0e;
    /* 00483f37: mov edx, dword ptr [esp + 0x18] */
    edx = RD(esp + 0x18u);
    /* 00483f3b: mov ebp, dword ptr [esp + 0x2c] */
    ebp = RD(esp + 0x2cu);
    /* 00483f3f: mov dword ptr [edx], ecx */
    WR(edx, ecx);

L_00483f41:
    /* 00483f41: mov eax, dword ptr [esp + 0x1c] */
    eax = RD(esp + 0x1cu);
    /* 00483f45: mov edi, dword ptr [esp + 0x18] */
    edi = RD(esp + 0x18u);
    /* 00483f49: mov ecx, dword ptr [ebp + 0x38] */
    ecx = RD(ebp + 0x38u);
    /* 00483f4c: inc eax */
    eax++;
    /* 00483f4d: add edi, 4 */
    edi += 4u;
    /* 00483f50: add esi, 0x2c */
    esi += 0x2cu;
    /* 00483f53: cmp eax, ecx */
    /* 00483f55: mov dword ptr [esp + 0x1c], eax */
    WR(esp + 0x1cu, eax);
    /* 00483f59: mov dword ptr [esp + 0x18], edi */
    WR(esp + 0x18u, edi);
    /* 00483f5d: jb 0x483ec6 */
    if (eax < ecx) goto L_00483ec6;

    /* 00483f63: xor edx, edx */
    edx = 0;

L_00483f65:
    /* 00483f65: mov eax, dword ptr [esp + 0x14] */
    eax = RD(esp + 0x14u);
    /* 00483f69: add ebx, 0x14c */
    ebx += 0x14cu;
    /* 00483f6f: dec eax */
    eax--;
    /* 00483f70: mov dword ptr [esp + 0x14], eax */
    WR(esp + 0x14u, eax);
    /* 00483f74: jne 0x483e98 */
    if (eax != 0u) goto L_00483e98;

    /* 00483f7a: mov eax, dword ptr [esp + 0x20]   (arg0 again) */
    eax = RD(esp + 0x20u);
    /* 00483f7e: mov dword ptr [esp + 0x2c], edx */
    WR(esp + 0x2cu, edx);
    /* 00483f82: mov ecx, dword ptr [eax + 0x54] */
    ecx = RD(eax + 0x54u);
    /* 00483f85: mov edi, dword ptr [eax + 0x58] */
    edi = RD(eax + 0x58u);
    /* 00483f88: cmp ecx, edx / 00483f8a: jbe 0x484680 */
    if (ecx <= edx) goto L_00484680;

    /* 00483f90: mov dword ptr [esp + 0x30], edx */
    WR(esp + 0x30u, edx);
    /* 00483f94: mov dword ptr [esp + 0x1c], edx */
    WR(esp + 0x1cu, edx);
    /* 00483f98: add edi, 0x44 */
    edi += 0x44u;
    /* 00483f9b: jmp 0x483f9f */
    goto L_00483f9f;

L_00483f9d:
    /* 00483f9d: xor edx, edx */
    edx = 0;

L_00483f9f:
    /* 00483f9f: xor eax, eax */
    eax = 0;
    /* 00483fa1: mov esi, dword ptr [esp + 0x34] */
    esi = RD(esp + 0x34u);
    /* 00483fa5: mov dword ptr [esp + 0x44], eax */
    WR(esp + 0x44u, eax);
    /* 00483fa9: xor ebp, ebp */
    ebp = 0;
    /* 00483fab: mov dword ptr [esp + 0x18], edx */
    WR(esp + 0x18u, edx);
    /* 00483faf: mov dword ptr [esp + 0x48], eax */
    WR(esp + 0x48u, eax);
    /* 00483fb3: xor ecx, ecx */
    ecx = 0;
    /* 00483fb5: xor edx, edx */
    edx = 0;
    /* 00483fb7: mov dword ptr [esp + 0x4c], eax */
    WR(esp + 0x4cu, eax);
    /* 00483fbb: mov dword ptr [esp + 0x38], ecx */
    WR(esp + 0x38u, ecx);
    /* 00483fbf: mov dword ptr [esp + 0x68], edx */
    WR(esp + 0x68u, edx);
    /* 00483fc3: mov dword ptr [esp + 0x74], eax */
    WR(esp + 0x74u, eax);
    /* 00483fc7: mov dword ptr [esp + 0x3c], ecx */
    WR(esp + 0x3cu, ecx);
    /* 00483fcb: mov dword ptr [esp + 0x6c], edx */
    WR(esp + 0x6cu, edx);
    /* 00483fcf: mov dword ptr [esp + 0x78], eax */
    WR(esp + 0x78u, eax);
    /* 00483fd3: mov dword ptr [esp + 0x24], ebp */
    WR(esp + 0x24u, ebp);
    /* 00483fd7: mov dword ptr [esp + 0x10], ebp */
    WR(esp + 0x10u, ebp);
    /* 00483fdb: mov dword ptr [esp + 0x14], ebp */
    WR(esp + 0x14u, ebp);
    /* 00483fdf: mov dword ptr [esp + 0x40], ecx */
    WR(esp + 0x40u, ecx);
    /* 00483fe3: mov dword ptr [esp + 0x70], edx */
    WR(esp + 0x70u, edx);
    /* 00483fe7: mov dword ptr [esp + 0x7c], eax */
    WR(esp + 0x7cu, eax);
    /* 00483feb: mov dword ptr [esp + 0x28], 4 */
    WR(esp + 0x28u, 4u);

L_00483ff3:
    /* 00483ff3: mov eax, dword ptr [esi + 0x128] */
    eax = RD(esi + 0x128u);
    /* 00483ff9: test eax, eax / 00483ffb: je 0x48439a */
    if (eax == 0u) goto L_0048439a;
    /* 00484001: mov ecx, dword ptr [eax + 0x2c] */
    ecx = RD(eax + 0x2cu);
    /* 00484004: mov edx, dword ptr [edi + 4] */
    edx = RD(edi + 4u);
    /* 00484007: test edx, ecx / 00484009: je 0x484010 */
    if ((edx & ecx) == 0u) goto L_00484010;
    /* 0048400b: mov ebx, dword ptr [esi + 0xc] */
    ebx = RD(esi + 0xcu);
    /* 0048400e: jmp 0x484013 */
    goto L_00484013;
L_00484010:
    /* 00484010: mov ebx, dword ptr [esi + 8] */
    ebx = RD(esi + 8u);

L_00484013:
    /* 00484013: test byte ptr [esi], 2 / 00484016: je 0x48439a */
    if ((RD8(esi) & 2u) == 0u) goto L_0048439a;
    /* 0048401c: mov ecx, dword ptr [edi] */
    ecx = RD(edi);
    /* 0048401e: mov eax, dword ptr [eax + 0x3c] */
    eax = RD(eax + 0x3cu);
    /* 00484021: lea edx, [ecx + ecx*4] */
    edx = ecx + ecx * 4u;
    /* 00484024: lea edx, [ecx + edx*2] */
    edx = ecx + edx * 2u;
    /* 00484027: lea edx, [eax + edx*4] */
    edx = eax + edx * 4u;
    /* 0048402a: mov eax, dword ptr [edx + 0x24] */
    eax = RD(edx + 0x24u);
    /* 0048402d: test eax, eax / 0048402f: je 0x48439a */
    if (eax == 0u) goto L_0048439a;
    /* 00484035: cmp ebx, dword ptr [esp + 0x18] / 00484039: jl 0x48439a */
    if ((int32_t)ebx < (int32_t)RD(esp + 0x18u)) goto L_0048439a;
    /* 0048403f: cmp ebx, ebp / 00484041: jge 0x484058 */
    if ((int32_t)ebx >= (int32_t)ebp) goto L_00484058;
    /* 00484043: fld dword ptr [esp + 0x10] */
    FLD32(esp + 0x10u);
    /* 00484047: fcomp dword ptr [0x4a8c3c] */
    FCOM_M(0x004a8c3cu);
    FPOP();
    /* 0048404d: fnstsw ax / 0048404f: test ah, 1 / 00484052: je 0x48439a */
    FNSTSW_AX();
    if (((eax >> 8) & 1u) == 0u) goto L_0048439a;

L_00484058:
    /* 00484058: mov ecx, dword ptr [esi + ecx*4 + 0x20] */
    ecx = RD(esi + ecx * 4u + 0x20u);
    /* 0048405c: mov eax, ecx */
    eax = ecx;
    /* 0048405e: shl eax, 3 */
    eax <<= 3;
    /* 00484061: sub eax, ecx */
    eax -= ecx;
    /* 00484063: mov ecx, dword ptr [edx + 0x28] */
    ecx = RD(edx + 0x28u);
    /* 00484066: fld dword ptr [esi + 0x120] */
    FLD32(esi + 0x120u);
    /* 0048406c: fsub dword ptr [ecx + eax*8] */
    XS(0) -= RDF(ecx + eax * 8u);
    /* 0048406f: lea eax, [ecx + eax*8] */
    eax = ecx + eax * 8u;
    /* 00484072: mov ecx, dword ptr [eax + 4] */
    ecx = RD(eax + 4u);
    /* 00484075: test cl, 1 / 00484078: je 0x4840ae */
    if ((ecx & 1u) == 0u) goto L_004840ae;

    /* 0048407a: fld dword ptr [eax + 0x20] */
    FLD32(eax + 0x20u);
    /* 0048407d: fld dword ptr [eax + 0x24] */
    FLD32(eax + 0x24u);
    /* 00484080: fld dword ptr [eax + 0x28] */
    FLD32(eax + 0x28u);
    /* 00484083: fxch st(2) */
    FXCH(2);
    /* 00484085: fmul st(3) */
    XS(0) *= XS(3);
    /* 00484087: fxch st(1) */
    FXCH(1);
    /* 00484089: fmul st(3) */
    XS(0) *= XS(3);
    /* 0048408b: fxch st(2) */
    FXCH(2);
    /* 0048408d: fmul st(3) */
    XS(0) *= XS(3);
    /* 0048408f: fxch st(1) */
    FXCH(1);
    /* 00484091: fadd dword ptr [eax + 8] */
    XS(0) += RDF(eax + 8u);
    /* 00484094: fxch st(2) */
    FXCH(2);
    /* 00484096: fadd dword ptr [eax + 0xc] */
    XS(0) += RDF(eax + 0xcu);
    /* 00484099: fxch st(1) */
    FXCH(1);
    /* 0048409b: fadd dword ptr [eax + 0x10] */
    XS(0) += RDF(eax + 0x10u);
    /* 0048409e: fxch st(2) */
    FXCH(2);
    /* 004840a0: fstp dword ptr [esp + 0x5c] */
    FSTP32(esp + 0x5cu);
    /* 004840a4: fstp dword ptr [esp + 0x60] */
    FSTP32(esp + 0x60u);
    /* 004840a8: fstp dword ptr [esp + 0x64] */
    FSTP32(esp + 0x64u);
    /* 004840ac: jmp 0x4840ca */
    goto L_004840ca;

L_004840ae:
    /* 004840ae: lea edx, [eax + 8] */
    edx = eax + 8u;
    /* 004840b1: mov ebp, dword ptr [eax + 8] */
    ebp = RD(eax + 8u);
    /* 004840b4: mov dword ptr [esp + 0x5c], ebp */
    WR(esp + 0x5cu, ebp);
    /* 004840b8: mov ebp, dword ptr [edx + 4] */
    ebp = RD(edx + 4u);
    /* 004840bb: mov dword ptr [esp + 0x60], ebp */
    WR(esp + 0x60u, ebp);
    /* 004840bf: mov ebp, dword ptr [esp + 0x24] */
    ebp = RD(esp + 0x24u);
    /* 004840c3: mov edx, dword ptr [edx + 8] */
    edx = RD(edx + 8u);
    /* 004840c6: mov dword ptr [esp + 0x64], edx */
    WR(esp + 0x64u, edx);

L_004840ca:
    /* 004840ca: test cl, 2 / 004840cd: je 0x4840fd */
    if ((ecx & 2u) == 0u) goto L_004840fd;
    /* 004840cf: fld dword ptr [eax + 0x2c] */
    FLD32(eax + 0x2cu);
    /* 004840d2: fld dword ptr [eax + 0x30] */
    FLD32(eax + 0x30u);
    /* 004840d5: fld dword ptr [eax + 0x34] */
    FLD32(eax + 0x34u);
    /* 004840d8: fxch st(2) */
    FXCH(2);
    /* 004840da: fmul st(3) */
    XS(0) *= XS(3);
    /* 004840dc: fxch st(1) */
    FXCH(1);
    /* 004840de: fmul st(3) */
    XS(0) *= XS(3);
    /* 004840e0: fxch st(2) */
    FXCH(2);
    /* 004840e2: fmul st(3) */
    XS(0) *= XS(3);
    /* 004840e4: fxch st(1) */
    FXCH(1);
    /* 004840e6: fadd dword ptr [eax + 0x14] */
    XS(0) += RDF(eax + 0x14u);
    /* 004840e9: fxch st(2) */
    FXCH(2);
    /* 004840eb: fadd dword ptr [eax + 0x18] */
    XS(0) += RDF(eax + 0x18u);
    /* 004840ee: fxch st(1) */
    FXCH(1);
    /* 004840f0: fadd dword ptr [eax + 0x1c] */
    XS(0) += RDF(eax + 0x1cu);
    /* 004840f3: fxch st(2) */
    FXCH(2);
    /* 004840f5: fstp dword ptr [esp + 0x50] */
    FSTP32(esp + 0x50u);
    /* 004840f9: fstp st(2) */
    FSTP_ST(2);
    /* 004840fb: jmp 0x48411e */
    goto L_0048411e;

L_004840fd:
    /* 004840fd: add eax, 0x14 */
    eax += 0x14u;
    /* 00484100: fstp st(0) */
    FPOP();
    /* 00484102: mov ecx, dword ptr [eax] */
    ecx = RD(eax);
    /* 00484104: mov dword ptr [esp + 0x50], ecx */
    WR(esp + 0x50u, ecx);
    /* 00484108: mov edx, dword ptr [eax + 4] */
    edx = RD(eax + 4u);
    /* 0048410b: mov dword ptr [esp + 0x54], edx */
    WR(esp + 0x54u, edx);
    /* 0048410f: mov eax, dword ptr [eax + 8] */
    eax = RD(eax + 8u);
    /* 00484112: fld dword ptr [esp + 0x54] */
    FLD32(esp + 0x54u);
    /* 00484116: mov dword ptr [esp + 0x58], eax */
    WR(esp + 0x58u, eax);
    /* 0048411a: fld dword ptr [esp + 0x58] */
    FLD32(esp + 0x58u);

L_0048411e:
    /* 0048411e: fld dword ptr [esp + 0x5c] */
    FLD32(esp + 0x5cu);
    /* 00484122: fld dword ptr [esp + 0x60] */
    FLD32(esp + 0x60u);
    /* 00484126: fld dword ptr [esp + 0x64] */
    FLD32(esp + 0x64u);
    /* 0048412a: fld dword ptr [esp + 0x50] */
    FLD32(esp + 0x50u);
    /* 0048412e: fxch st(3) */
    FXCH(3);
    /* 00484130: fsub dword ptr [edi + 0x28] */
    XS(0) -= RDF(edi + 0x28u);
    /* 00484133: fxch st(2) */
    FXCH(2);
    /* 00484135: fsub dword ptr [edi + 0x2c] */
    XS(0) -= RDF(edi + 0x2cu);
    /* 00484138: fxch st(1) */
    FXCH(1);
    /* 0048413a: fsub dword ptr [edi + 0x30] */
    XS(0) -= RDF(edi + 0x30u);
    /* 0048413d: fxch st(5) */
    FXCH(5);
    /* 0048413f: fsub dword ptr [edi + 0x38] */
    XS(0) -= RDF(edi + 0x38u);
    /* 00484142: fxch st(4) */
    FXCH(4);
    /* 00484144: fsub dword ptr [edi + 0x3c] */
    XS(0) -= RDF(edi + 0x3cu);
    /* 00484147: fxch st(3) */
    FXCH(3);
    /* 00484149: fsub dword ptr [edi + 0x34] */
    XS(0) -= RDF(edi + 0x34u);
    /* 0048414c: fxch st(2) */
    FXCH(2);
    /* 0048414e: fstp dword ptr [esp + 0x5c] */
    FSTP32(esp + 0x5cu);
    /* 00484152: push ecx   (stack space for the first 0x49418b argument) */
    PUSH(ecx);
    /* 00484153: fstp dword ptr [esp + 0x64] */
    FSTP32(esp + 0x64u);
    /* 00484157: fxch st(3) */
    FXCH(3);
    /* 00484159: fstp dword ptr [esp + 0x68] */
    FSTP32(esp + 0x68u);
    /* 0048415d: fxch st(1) */
    FXCH(1);
    /* 0048415f: fstp dword ptr [esp + 0x58] */
    FSTP32(esp + 0x58u);
    /* 00484163: fstp dword ptr [esp + 0x5c] */
    FSTP32(esp + 0x5cu);
    /* 00484167: fstp dword ptr [esp]   (the argument) */
    FSTP32(esp);
    /* 0048416a: call 0x49418b (angle wrap) */
    ZCALL(wmain_0049418b, 0x0048416fu);
    /* 0048416f: mov ecx, dword ptr [esp + 0x58] */
    ecx = RD(esp + 0x58u);
    /* 00484173: add esp, 4 */
    esp += 4u;
    /* 00484176: fstp dword ptr [esp + 0x50] */
    FSTP32(esp + 0x50u);
    /* 0048417a: push ecx */
    PUSH(ecx);
    /* 0048417b: call 0x49418b */
    ZCALL(wmain_0049418b, 0x00484180u);
    /* 00484180: mov edx, dword ptr [esp + 0x5c] */
    edx = RD(esp + 0x5cu);
    /* 00484184: add esp, 4 */
    esp += 4u;
    /* 00484187: fstp dword ptr [esp + 0x54] */
    FSTP32(esp + 0x54u);
    /* 0048418b: push edx */
    PUSH(edx);
    /* 0048418c: call 0x49418b */
    ZCALL(wmain_0049418b, 0x00484191u);
    /* 00484191: fld dword ptr [esi + 0x18] */
    FLD32(esi + 0x18u);
    /* 00484194: fcomp dword ptr [0x4a8c3c] */
    FCOM_M(0x004a8c3cu);
    FPOP();
    /* 0048419a: add esp, 4 */
    esp += 4u;
    /* 0048419d: fstp dword ptr [esp + 0x58] */
    FSTP32(esp + 0x58u);
    /* 004841a1: fnstsw ax / 004841a3: test ah, 1 / 004841a6: je 0x4841fc */
    FNSTSW_AX();
    if (((eax >> 8) & 1u) == 0u) goto L_004841fc;

    /* 004841a8..0x4841b7: six fld [esi + 0x18] */
    FLD32(esi + 0x18u);
    FLD32(esi + 0x18u);
    FLD32(esi + 0x18u);
    FLD32(esi + 0x18u);
    FLD32(esi + 0x18u);
    FLD32(esi + 0x18u);
    /* 004841ba: fxch st(5) */
    FXCH(5);
    /* 004841bc: fmul dword ptr [esp + 0x5c] */
    XS(0) *= RDF(esp + 0x5cu);
    /* 004841c0: fxch st(4) */
    FXCH(4);
    /* 004841c2: fmul dword ptr [esp + 0x60] */
    XS(0) *= RDF(esp + 0x60u);
    /* 004841c6: fxch st(3) */
    FXCH(3);
    /* 004841c8: fmul dword ptr [esp + 0x64] */
    XS(0) *= RDF(esp + 0x64u);
    /* 004841cc: fxch st(2) */
    FXCH(2);
    /* 004841ce: fmul dword ptr [esp + 0x50] */
    XS(0) *= RDF(esp + 0x50u);
    /* 004841d2: fxch st(1) */
    FXCH(1);
    /* 004841d4: fmul dword ptr [esp + 0x54] */
    XS(0) *= RDF(esp + 0x54u);
    /* 004841d8: fxch st(5) */
    FXCH(5);
    /* 004841da: fmul dword ptr [esp + 0x58] */
    XS(0) *= RDF(esp + 0x58u);
    /* 004841de: fxch st(4) */
    FXCH(4);
    /* 004841e0: fstp dword ptr [esp + 0x5c] */
    FSTP32(esp + 0x5cu);
    /* 004841e4: fxch st(2) */
    FXCH(2);
    /* 004841e6: fstp dword ptr [esp + 0x60] */
    FSTP32(esp + 0x60u);
    /* 004841ea: fstp dword ptr [esp + 0x64] */
    FSTP32(esp + 0x64u);
    /* 004841ee: fstp dword ptr [esp + 0x50] */
    FSTP32(esp + 0x50u);
    /* 004841f2: fxch st(1) */
    FXCH(1);
    /* 004841f4: fstp dword ptr [esp + 0x54] */
    FSTP32(esp + 0x54u);
    /* 004841f8: fstp dword ptr [esp + 0x58] */
    FSTP32(esp + 0x58u);

L_004841fc:
    /* 004841fc: cmp ebx, ebp / 004841fe: jne 0x48426e */
    if (ebx != ebp) goto L_0048426e;
    /* 00484200..0x484218: seven fld (the six accumulators and the blend weight) */
    FLD32(esp + 0x44u);
    FLD32(esp + 0x48u);
    FLD32(esp + 0x4cu);
    FLD32(esp + 0x38u);
    FLD32(esp + 0x3cu);
    FLD32(esp + 0x40u);
    FLD32(esp + 0x10u);
    /* 0048421c: fxch st(6) */
    FXCH(6);
    /* 0048421e: fadd dword ptr [esp + 0x5c] */
    XS(0) += RDF(esp + 0x5cu);
    /* 00484222: fxch st(5) */
    FXCH(5);
    /* 00484224: fadd dword ptr [esp + 0x60] */
    XS(0) += RDF(esp + 0x60u);
    /* 00484228: fxch st(4) */
    FXCH(4);
    /* 0048422a: fadd dword ptr [esp + 0x64] */
    XS(0) += RDF(esp + 0x64u);
    /* 0048422e: fxch st(3) */
    FXCH(3);
    /* 00484230: fadd dword ptr [esp + 0x50] */
    XS(0) += RDF(esp + 0x50u);
    /* 00484234: fxch st(2) */
    FXCH(2);
    /* 00484236: fadd dword ptr [esp + 0x54] */
    XS(0) += RDF(esp + 0x54u);
    /* 0048423a: fxch st(1) */
    FXCH(1);
    /* 0048423c: fadd dword ptr [esp + 0x58] */
    XS(0) += RDF(esp + 0x58u);
    /* 00484240: fxch st(6) */
    FXCH(6);
    /* 00484242: fadd dword ptr [esi + 0x18] */
    XS(0) += RDF(esi + 0x18u);
    /* 00484245: fxch st(5) */
    FXCH(5);
    /* 00484247: fstp dword ptr [esp + 0x44] */
    FSTP32(esp + 0x44u);
    /* 0048424b: fxch st(3) */
    FXCH(3);
    /* 0048424d: fstp dword ptr [esp + 0x48] */
    FSTP32(esp + 0x48u);
    /* 00484251: fxch st(1) */
    FXCH(1);
    /* 00484253: fstp dword ptr [esp + 0x4c] */
    FSTP32(esp + 0x4cu);
    /* 00484257: fstp dword ptr [esp + 0x38] */
    FSTP32(esp + 0x38u);
    /* 0048425b: fstp dword ptr [esp + 0x3c] */
    FSTP32(esp + 0x3cu);
    /* 0048425f: fxch st(1) */
    FXCH(1);
    /* 00484261: fstp dword ptr [esp + 0x40] */
    FSTP32(esp + 0x40u);
    /* 00484265: fstp dword ptr [esp + 0x10] */
    FSTP32(esp + 0x10u);
    /* 00484269: jmp 0x48439a */
    goto L_0048439a;

L_0048426e:
    /* 0048426e: jle 0x4842ee   (uses the cmp ebx,ebp at 0x4841fc) */
    if ((int32_t)ebx <= (int32_t)ebp) goto L_004842ee;
    /* 00484270..0x4842e5: save the current sample as the previous one, start a new accumulator */
    eax = RD(esp + 0x44u);
    ecx = RD(esp + 0x48u);
    edx = RD(esp + 0x4cu);
    WR(esp + 0x68u, eax);
    eax = RD(esp + 0x38u);
    WR(esp + 0x6cu, ecx);
    ecx = RD(esp + 0x3cu);
    WR(esp + 0x70u, edx);
    edx = RD(esp + 0x40u);
    WR(esp + 0x74u, eax);
    eax = RD(esp + 0x10u);
    WR(esp + 0x78u, ecx);
    ecx = RD(esp + 0x5cu);
    WR(esp + 0x7cu, edx);
    edx = RD(esp + 0x60u);
    WR(esp + 0x14u, eax);
    eax = RD(esp + 0x64u);
    WR(esp + 0x44u, ecx);
    ecx = RD(esp + 0x50u);
    WR(esp + 0x18u, ebp);
    WR(esp + 0x48u, edx);
    edx = RD(esp + 0x54u);
    WR(esp + 0x4cu, eax);
    eax = RD(esp + 0x58u);
    WR(esp + 0x38u, ecx);
    ecx = RD(esi + 0x18u);
    ebp = ebx;
    WR(esp + 0x3cu, edx);
    WR(esp + 0x24u, ebp);
    WR(esp + 0x40u, eax);
    WR(esp + 0x10u, ecx);
    /* 004842e9: jmp 0x48439a */
    goto L_0048439a;

L_004842ee:
    /* 004842ee: cmp ebx, dword ptr [esp + 0x18] / 004842f2: jle 0x484331 */
    if ((int32_t)ebx <= (int32_t)RD(esp + 0x18u)) goto L_00484331;
    /* 004842f4..0x48432b: the new sample becomes the current one (no accumulation yet) */
    edx = RD(esp + 0x5cu);
    eax = RD(esp + 0x60u);
    ecx = RD(esp + 0x64u);
    WR(esp + 0x68u, edx);
    edx = RD(esp + 0x50u);
    WR(esp + 0x6cu, eax);
    eax = RD(esp + 0x54u);
    WR(esp + 0x70u, ecx);
    ecx = RD(esp + 0x58u);
    WR(esp + 0x74u, edx);
    edx = RD(esi + 0x18u);
    WR(esp + 0x78u, eax);
    WR(esp + 0x7cu, ecx);
    WR(esp + 0x14u, edx);
    WR(esp + 0x18u, ebx);
    /* 0048432f: jmp 0x48439a */
    goto L_0048439a;

L_00484331:
    /* 00484331..0x484349: seven fld */
    FLD32(esp + 0x68u);
    FLD32(esp + 0x6cu);
    FLD32(esp + 0x70u);
    FLD32(esp + 0x74u);
    FLD32(esp + 0x78u);
    FLD32(esp + 0x7cu);
    FLD32(esp + 0x14u);
    /* 0048434d: fxch st(6) */
    FXCH(6);
    /* 0048434f: fadd dword ptr [esp + 0x5c] */
    XS(0) += RDF(esp + 0x5cu);
    /* 00484353: fxch st(5) */
    FXCH(5);
    /* 00484355: fadd dword ptr [esp + 0x60] */
    XS(0) += RDF(esp + 0x60u);
    /* 00484359: fxch st(4) */
    FXCH(4);
    /* 0048435b: fadd dword ptr [esp + 0x64] */
    XS(0) += RDF(esp + 0x64u);
    /* 0048435f: fxch st(3) */
    FXCH(3);
    /* 00484361: fadd dword ptr [esp + 0x50] */
    XS(0) += RDF(esp + 0x50u);
    /* 00484365: fxch st(2) */
    FXCH(2);
    /* 00484367: fadd dword ptr [esp + 0x54] */
    XS(0) += RDF(esp + 0x54u);
    /* 0048436b: fxch st(1) */
    FXCH(1);
    /* 0048436d: fadd dword ptr [esp + 0x58] */
    XS(0) += RDF(esp + 0x58u);
    /* 00484371: fxch st(6) */
    FXCH(6);
    /* 00484373: fadd dword ptr [esi + 0x18] */
    XS(0) += RDF(esi + 0x18u);
    /* 00484376: fxch st(5) */
    FXCH(5);
    /* 00484378: fstp dword ptr [esp + 0x68] */
    FSTP32(esp + 0x68u);
    /* 0048437c: fxch st(3) */
    FXCH(3);
    /* 0048437e: fstp dword ptr [esp + 0x6c] */
    FSTP32(esp + 0x6cu);
    /* 00484382: fxch st(1) */
    FXCH(1);
    /* 00484384: fstp dword ptr [esp + 0x70] */
    FSTP32(esp + 0x70u);
    /* 00484388: fstp dword ptr [esp + 0x74] */
    FSTP32(esp + 0x74u);
    /* 0048438c: fstp dword ptr [esp + 0x78] */
    FSTP32(esp + 0x78u);
    /* 00484390: fxch st(1) */
    FXCH(1);
    /* 00484392: fstp dword ptr [esp + 0x7c] */
    FSTP32(esp + 0x7cu);
    /* 00484396: fstp dword ptr [esp + 0x14] */
    FSTP32(esp + 0x14u);

L_0048439a:
    /* 0048439a: mov eax, dword ptr [esp + 0x28] */
    eax = RD(esp + 0x28u);
    /* 0048439e: add esi, 0x14c */
    esi += 0x14cu;
    /* 004843a4: dec eax */
    eax--;
    /* 004843a5: mov dword ptr [esp + 0x28], eax */
    WR(esp + 0x28u, eax);
    /* 004843a9: jne 0x483ff3 */
    if (eax != 0u) goto L_00483ff3;

    /* 004843af: fld dword ptr [esp + 0x10] */
    FLD32(esp + 0x10u);
    /* 004843b3: fcomp dword ptr [0x4a8c3c] */
    FCOM_M(0x004a8c3cu);
    FPOP();
    /* 004843b9: fnstsw ax / 004843bb: test ah, 1 / 004843be: je 0x4844b2 */
    FNSTSW_AX();
    if (((eax >> 8) & 1u) == 0u) goto L_004844b2;
    /* 004843c4: fld dword ptr [esp + 0x14] */
    FLD32(esp + 0x14u);
    /* 004843c8: fcomp dword ptr [0x4a8c38] */
    FCOM_M(0x004a8c38u);
    FPOP();
    /* 004843ce: fnstsw ax / 004843d0: test ah, 0x41 / 004843d3: jne 0x4844b2 */
    FNSTSW_AX();
    if (((eax >> 8) & 0x41u) != 0u) goto L_004844b2;
    /* 004843d9: fld dword ptr [esp + 0x14] */
    FLD32(esp + 0x14u);
    /* 004843dd: fcomp dword ptr [0x4a8c3c] */
    FCOM_M(0x004a8c3cu);
    FPOP();
    /* 004843e3: fnstsw ax / 004843e5: test ah, 0x41 / 004843e8: jne 0x484442 */
    FNSTSW_AX();
    if (((eax >> 8) & 0x41u) != 0u) goto L_00484442;
    /* 004843ea: fld dword ptr [0x4a8c3c]; fdiv dword ptr [esp + 0x14] */
    FLD32(0x004a8c3cu);
    XS(0) /= RDF(esp + 0x14u);
    /* 004843f4..0x4843fc: fld st(0); fld st(1); fld st(2); fld st(3); fld st(4) */
    FPUSH(XS(0));
    FPUSH(XS(1));
    FPUSH(XS(2));
    FPUSH(XS(3));
    FPUSH(XS(4));
    /* 004843fe: fxch st(4) */
    FXCH(4);
    /* 00484400: fmul dword ptr [esp + 0x68] */
    XS(0) *= RDF(esp + 0x68u);
    /* 00484404: fxch st(3) */
    FXCH(3);
    /* 00484406: fmul dword ptr [esp + 0x6c] */
    XS(0) *= RDF(esp + 0x6cu);
    /* 0048440a: fxch st(2) */
    FXCH(2);
    /* 0048440c: fmul dword ptr [esp + 0x70] */
    XS(0) *= RDF(esp + 0x70u);
    /* 00484410: fxch st(1) */
    FXCH(1);
    /* 00484412: fmul dword ptr [esp + 0x74] */
    XS(0) *= RDF(esp + 0x74u);
    /* 00484416: fxch st(4) */
    FXCH(4);
    /* 00484418: fmul dword ptr [esp + 0x78] */
    XS(0) *= RDF(esp + 0x78u);
    /* 0048441c: fxch st(5) */
    FXCH(5);
    /* 0048441e: fmul dword ptr [esp + 0x7c] */
    XS(0) *= RDF(esp + 0x7cu);
    /* 00484422: fxch st(3) */
    FXCH(3);
    /* 00484424: fstp dword ptr [esp + 0x68] */
    FSTP32(esp + 0x68u);
    /* 00484428: fxch st(1) */
    FXCH(1);
    /* 0048442a: fstp dword ptr [esp + 0x6c] */
    FSTP32(esp + 0x6cu);
    /* 0048442e: fstp dword ptr [esp + 0x70] */
    FSTP32(esp + 0x70u);
    /* 00484432: fxch st(1) */
    FXCH(1);
    /* 00484434: fstp dword ptr [esp + 0x74] */
    FSTP32(esp + 0x74u);
    /* 00484438: fxch st(1) */
    FXCH(1);
    /* 0048443a: fstp dword ptr [esp + 0x78] */
    FSTP32(esp + 0x78u);
    /* 0048443e: fstp dword ptr [esp + 0x7c] */
    FSTP32(esp + 0x7cu);

L_00484442:
    /* 00484442: fld dword ptr [0x4a8c3c]; fsub dword ptr [esp + 0x10] */
    FLD32(0x004a8c3cu);
    XS(0) -= RDF(esp + 0x10u);
    /* 0048444c..0x484454: fld st(0); fld st(1); fld st(2); fld st(3); fld st(4) */
    FPUSH(XS(0));
    FPUSH(XS(1));
    FPUSH(XS(2));
    FPUSH(XS(3));
    FPUSH(XS(4));
    /* 00484456: fxch st(4) */
    FXCH(4);
    /* 00484458: fmul dword ptr [esp + 0x68] */
    XS(0) *= RDF(esp + 0x68u);
    /* 0048445c: fxch st(3) */
    FXCH(3);
    /* 0048445e: fmul dword ptr [esp + 0x6c] */
    XS(0) *= RDF(esp + 0x6cu);
    /* 00484462: fxch st(2) */
    FXCH(2);
    /* 00484464: fmul dword ptr [esp + 0x70] */
    XS(0) *= RDF(esp + 0x70u);
    /* 00484468: fxch st(1) */
    FXCH(1);
    /* 0048446a: fmul dword ptr [esp + 0x74] */
    XS(0) *= RDF(esp + 0x74u);
    /* 0048446e: fxch st(4) */
    FXCH(4);
    /* 00484470: fmul dword ptr [esp + 0x78] */
    XS(0) *= RDF(esp + 0x78u);
    /* 00484474: fxch st(5) */
    FXCH(5);
    /* 00484476: fmul dword ptr [esp + 0x7c] */
    XS(0) *= RDF(esp + 0x7cu);
    /* 0048447a: fxch st(3) */
    FXCH(3);
    /* 0048447c: fadd dword ptr [esp + 0x44] */
    XS(0) += RDF(esp + 0x44u);
    /* 00484480: fxch st(2) */
    FXCH(2);
    /* 00484482: fadd dword ptr [esp + 0x48] */
    XS(0) += RDF(esp + 0x48u);
    /* 00484486: fxch st(1) */
    FXCH(1);
    /* 00484488: fadd dword ptr [esp + 0x4c] */
    XS(0) += RDF(esp + 0x4cu);
    /* 0048448c: fxch st(4) */
    FXCH(4);
    /* 0048448e: fadd dword ptr [esp + 0x38] */
    XS(0) += RDF(esp + 0x38u);
    /* 00484492: fxch st(5) */
    FXCH(5);
    /* 00484494: fadd dword ptr [esp + 0x3c] */
    XS(0) += RDF(esp + 0x3cu);
    /* 00484498: fxch st(3) */
    FXCH(3);
    /* 0048449a: fadd dword ptr [esp + 0x40] */
    XS(0) += RDF(esp + 0x40u);
    /* 0048449e: fxch st(2) */
    FXCH(2);
    /* 004844a0: fstp dword ptr [esp + 0x44] */
    FSTP32(esp + 0x44u);
    /* 004844a4: fstp dword ptr [esp + 0x48] */
    FSTP32(esp + 0x48u);
    /* 004844a8: fxch st(2) */
    FXCH(2);
    /* 004844aa: fstp dword ptr [esp + 0x4c] */
    FSTP32(esp + 0x4cu);
    /* 004844ae: fxch st(2) */
    FXCH(2);
    /* 004844b0: jmp 0x48450d */
    goto L_0048450d;

L_004844b2:
    /* 004844b2: fld dword ptr [esp + 0x10] */
    FLD32(esp + 0x10u);
    /* 004844b6: fcomp dword ptr [0x4a8c3c] */
    FCOM_M(0x004a8c3cu);
    FPOP();
    /* 004844bc: fnstsw ax / 004844be: test ah, 0x41 / 004844c1: jne 0x48451b */
    FNSTSW_AX();
    if (((eax >> 8) & 0x41u) != 0u) goto L_0048451b;
    /* 004844c3: fld dword ptr [0x4a8c3c]; fdiv dword ptr [esp + 0x10] */
    FLD32(0x004a8c3cu);
    XS(0) /= RDF(esp + 0x10u);
    /* 004844cd..0x4844d5: fld st(0); fld st(1); fld st(2); fld st(3); fld st(4) */
    FPUSH(XS(0));
    FPUSH(XS(1));
    FPUSH(XS(2));
    FPUSH(XS(3));
    FPUSH(XS(4));
    /* 004844d7: fxch st(4) */
    FXCH(4);
    /* 004844d9: fmul dword ptr [esp + 0x44] */
    XS(0) *= RDF(esp + 0x44u);
    /* 004844dd: fxch st(3) */
    FXCH(3);
    /* 004844df: fmul dword ptr [esp + 0x48] */
    XS(0) *= RDF(esp + 0x48u);
    /* 004844e3: fxch st(2) */
    FXCH(2);
    /* 004844e5: fmul dword ptr [esp + 0x4c] */
    XS(0) *= RDF(esp + 0x4cu);
    /* 004844e9: fxch st(1) */
    FXCH(1);
    /* 004844eb: fmul dword ptr [esp + 0x38] */
    XS(0) *= RDF(esp + 0x38u);
    /* 004844ef: fxch st(4) */
    FXCH(4);
    /* 004844f1: fmul dword ptr [esp + 0x3c] */
    XS(0) *= RDF(esp + 0x3cu);
    /* 004844f5: fxch st(5) */
    FXCH(5);
    /* 004844f7: fmul dword ptr [esp + 0x40] */
    XS(0) *= RDF(esp + 0x40u);
    /* 004844fb: fxch st(3) */
    FXCH(3);
    /* 004844fd: fstp dword ptr [esp + 0x44] */
    FSTP32(esp + 0x44u);
    /* 00484501: fxch st(1) */
    FXCH(1);
    /* 00484503: fstp dword ptr [esp + 0x48] */
    FSTP32(esp + 0x48u);
    /* 00484507: fstp dword ptr [esp + 0x4c] */
    FSTP32(esp + 0x4cu);
    /* 0048450b: fxch st(1) */
    FXCH(1);

L_0048450d:
    /* 0048450d: fstp dword ptr [esp + 0x38] */
    FSTP32(esp + 0x38u);
    /* 00484511: fxch st(1) */
    FXCH(1);
    /* 00484513: fstp dword ptr [esp + 0x3c] */
    FSTP32(esp + 0x3cu);
    /* 00484517: fstp dword ptr [esp + 0x40] */
    FSTP32(esp + 0x40u);

L_0048451b:
    /* 0048451b: mov eax, dword ptr [esp + 0x38] */
    eax = RD(esp + 0x38u);
    /* 0048451f: push eax */
    PUSH(eax);
    /* 00484520: call 0x49418b */
    ZCALL(wmain_0049418b, 0x00484525u);
    /* 00484525: mov ecx, dword ptr [esp + 0x40] */
    ecx = RD(esp + 0x40u);
    /* 00484529: add esp, 4 */
    esp += 4u;
    /* 0048452c: fstp dword ptr [esp + 0x38] */
    FSTP32(esp + 0x38u);
    /* 00484530: push ecx */
    PUSH(ecx);
    /* 00484531: call 0x49418b */
    ZCALL(wmain_0049418b, 0x00484536u);
    /* 00484536: mov edx, dword ptr [esp + 0x44] */
    edx = RD(esp + 0x44u);
    /* 0048453a: add esp, 4 */
    esp += 4u;
    /* 0048453d: fstp dword ptr [esp + 0x3c] */
    FSTP32(esp + 0x3cu);
    /* 00484541: push edx   (the third argument stays pushed until 0x4845a9) */
    PUSH(edx);
    /* 00484542: call 0x49418b */
    ZCALL(wmain_0049418b, 0x00484547u);

    /* 00484547..0x484556: six fld [edi + 0x28..0x3c] */
    FLD32(edi + 0x28u);
    FLD32(edi + 0x2cu);
    FLD32(edi + 0x30u);
    FLD32(edi + 0x34u);
    FLD32(edi + 0x38u);
    FLD32(edi + 0x3cu);
    /* 00484559: fxch st(5) */
    FXCH(5);
    /* 0048455b: fadd dword ptr [esp + 0x48] */
    XS(0) += RDF(esp + 0x48u);
    /* 0048455f: fxch st(4) */
    FXCH(4);
    /* 00484561: fadd dword ptr [esp + 0x4c] */
    XS(0) += RDF(esp + 0x4cu);
    /* 00484565: fxch st(3) */
    FXCH(3);
    /* 00484567: fadd dword ptr [esp + 0x50] */
    XS(0) += RDF(esp + 0x50u);
    /* 0048456b: fxch st(2) */
    FXCH(2);
    /* 0048456d: fadd dword ptr [esp + 0x3c] */
    XS(0) += RDF(esp + 0x3cu);
    /* 00484571: fxch st(1) */
    FXCH(1);
    /* 00484573: fadd dword ptr [esp + 0x40] */
    XS(0) += RDF(esp + 0x40u);
    /* 00484577: fxch st(6) */
    FXCH(6);
    /* 00484579: fstp dword ptr [esp + 0x44] */
    FSTP32(esp + 0x44u);
    /* 0048457d: fxch st(4) */
    FXCH(4);
    /* 0048457f: fadd dword ptr [esp + 0x44] */
    XS(0) += RDF(esp + 0x44u);
    /* 00484583: fxch st(3) */
    FXCH(3);
    /* 00484585: fstp dword ptr [esp + 0x48] */
    FSTP32(esp + 0x48u);
    /* 00484589: mov esi, dword ptr [esp + 0x88]   (arg0: the third argument is still pushed) */
    esi = RD(esp + 0x88u);
    /* 00484590: mov ebx, dword ptr [esp + 0x34] */
    ebx = RD(esp + 0x34u);
    /* 00484594: fxch st(1) */
    FXCH(1);
    /* 00484596: fstp dword ptr [esp + 0x4c] */
    FSTP32(esp + 0x4cu);
    /* 0048459a: mov edx, dword ptr [esi + 0x20] */
    edx = RD(esi + 0x20u);
    /* 0048459d: mov eax, ebx */
    eax = ebx;
    /* 0048459f: fstp dword ptr [esp + 0x50] */
    FSTP32(esp + 0x50u);
    /* 004845a3: fxch st(1) */
    FXCH(1);
    /* 004845a5: fstp dword ptr [esp + 0x3c] */
    FSTP32(esp + 0x3cu);
    /* 004845a9: add esp, 4 */
    esp += 4u;
    /* 004845ac: lea ecx, [esp + 0x44] */
    ecx = esp + 0x44u;
    /* 004845b0: add eax, edx */
    eax += edx;
    /* 004845b2: lea edx, [esp + 0x38] */
    edx = esp + 0x38u;
    /* 004845b6: fxch st(1) */
    FXCH(1);
    /* 004845b8: fstp dword ptr [esp + 0x3c] */
    FSTP32(esp + 0x3cu);
    /* 004845bc: push ecx */
    PUSH(ecx);
    /* 004845bd: push edx */
    PUSH(edx);
    /* 004845be: fstp dword ptr [esp + 0x48] */
    FSTP32(esp + 0x48u);
    /* 004845c2: push eax */
    PUSH(eax);
    /* 004845c3: call 0x47cac0  (out = ebx + arg0[0x20], angles = S+0x38, pos = S+0x44) */
    ZCALL(wmain_0047cac0, 0x004845c8u);
    /* 004845c8: mov eax, dword ptr [esi + 0x24] */
    eax = RD(esi + 0x24u);
    /* 004845cb: mov ecx, dword ptr [esp + 0x28] */
    ecx = RD(esp + 0x28u);
    /* 004845cf: add ecx, eax */
    ecx += eax;
    /* 004845d1: add esp, 0xc */
    esp += 0xcu;
    /* 004845d4: fld dword ptr [ecx] */
    FLD32(ecx);
    /* 004845d6: fcomp dword ptr [0x4a8c38] */
    FCOM_M(0x004a8c38u);
    FPOP();
    /* 004845dc: fnstsw ax / 004845de: test ah, 0x40 / 004845e1: je 0x484603 */
    FNSTSW_AX();
    if (((eax >> 8) & 0x40u) == 0u) goto L_00484603;
    /* 004845e3: fld dword ptr [ecx + 4] */
    FLD32(ecx + 4u);
    /* 004845e6: fcomp dword ptr [0x4a8c38] */
    FCOM_M(0x004a8c38u);
    FPOP();
    /* 004845ec: fnstsw ax / 004845ee: test ah, 0x40 / 004845f1: je 0x484603 */
    FNSTSW_AX();
    if (((eax >> 8) & 0x40u) == 0u) goto L_00484603;
    /* 004845f3: fld dword ptr [ecx + 8] */
    FLD32(ecx + 8u);
    /* 004845f6: fcomp dword ptr [0x4a8c38] */
    FCOM_M(0x004a8c38u);
    FPOP();
    /* 004845fc: fnstsw ax / 004845fe: test ah, 0x40 / 00484601: jne 0x484614 */
    FNSTSW_AX();
    if (((eax >> 8) & 0x40u) != 0u) goto L_00484614;

L_00484603:
    /* 00484603: mov ebp, dword ptr [esi + 0x20] */
    ebp = RD(esi + 0x20u);
    /* 00484606: mov edx, ebx */
    edx = ebx;
    /* 00484608: add edx, ebp */
    edx += ebp;
    /* 0048460a: push ecx */
    PUSH(ecx);
    /* 0048460b: push edx */
    PUSH(edx);
    /* 0048460c: call 0x47e0cd */
    ZCALL(wmain_0047e0cd, 0x00484611u);
    /* 00484611: add esp, 8 */
    esp += 8u;

L_00484614:
    /* 00484614: mov ecx, dword ptr [esp + 0x1c] */
    ecx = RD(esp + 0x1cu);
    /* 00484618: mov eax, dword ptr [esp + 0x2c] */
    eax = RD(esp + 0x2cu);
    /* 0048461c: add ecx, 0xc */
    ecx += 0xcu;
    /* 0048461f: inc eax */
    eax++;
    /* 00484620: mov dword ptr [esp + 0x1c], ecx */
    WR(esp + 0x1cu, ecx);
    /* 00484624: mov ecx, dword ptr [esp + 0x20] */
    ecx = RD(esp + 0x20u);
    /* 00484628: add ebx, 0x30 */
    ebx += 0x30u;
    /* 0048462b: add edi, 0xb4 */
    edi += 0xb4u;
    /* 00484631: mov edx, dword ptr [ecx + 0x54] */
    edx = RD(ecx + 0x54u);
    /* 00484634: mov dword ptr [esp + 0x2c], eax */
    WR(esp + 0x2cu, eax);
    /* 00484638: cmp eax, edx */
    /* 0048463a: mov dword ptr [esp + 0x30], ebx */
    WR(esp + 0x30u, ebx);
    /* 0048463e: jb 0x483f9d */
    if (eax < edx) goto L_00483f9d;
    /* 00484644: jmp 0x484680 */
    goto L_00484680;

L_00484646:
    /* 00484646: mov esi, dword ptr [ebp + 0x54] */
    esi = RD(ebp + 0x54u);
    /* 00484649: mov eax, dword ptr [ebp + 0x58] */
    eax = RD(ebp + 0x58u);
    /* 0048464c: xor ebx, ebx */
    ebx = 0;
    /* 0048464e: cmp esi, edx / 00484650: jbe 0x484680 */
    if (esi <= edx) goto L_00484680;
    /* 00484652: add eax, 0x84 */
    eax += 0x84u;
    /* 00484657: jmp 0x484660 */
    goto L_00484660;

L_00484659:
    /* 00484659: mov ecx, dword ptr [esp + 0x84] */
    ecx = RD(esp + 0x84u);

L_00484660:
    /* 00484660: mov esi, dword ptr [ecx + 0x20] */
    esi = RD(ecx + 0x20u);
    /* 00484663: mov edi, edx */
    edi = edx;
    /* 00484665: add edi, esi */
    edi += esi;
    /* 00484667: mov ecx, 0xc */
    ecx = 0xcu;
    /* 0048466c: mov esi, eax */
    esi = eax;
    /* 0048466e: add eax, 0xb4 */
    eax += 0xb4u;
    /* 00484673: rep movsd dword ptr es:[edi], dword ptr [esi] */
    while (ecx) { WR(edi, RD(esi)); esi += (uint32_t)df; edi += (uint32_t)df; ecx--; }
    /* 00484675: mov ecx, dword ptr [ebp + 0x54] */
    ecx = RD(ebp + 0x54u);
    /* 00484678: inc ebx */
    ebx++;
    /* 00484679: add edx, 0x30 */
    edx += 0x30u;
    /* 0048467c: cmp ebx, ecx / 0048467e: jb 0x484659 */
    if (ebx < ecx) goto L_00484659;

L_00484680:
    /* 00484680: mov eax, dword ptr [esp + 0x20] */
    eax = RD(esp + 0x20u);
    /* 00484684: mov edx, dword ptr [esp + 0x88] */
    edx = RD(esp + 0x88u);
    /* 0048468b: mov esi, dword ptr [esp + 0x84] */
    esi = RD(esp + 0x84u);
    /* 00484692: push edx */
    PUSH(edx);
    /* 00484693: mov ecx, dword ptr [eax + 0x58] */
    ecx = RD(eax + 0x58u);
    /* 00484696: push ecx */
    PUSH(ecx);
    /* 00484697: push esi */
    PUSH(esi);
    /* 00484698: call 0x4852b3 */
    ZCALL(wmain_004852b3, 0x0048469du);
    /* 0048469d: mov edx, dword ptr [0x4b8860] */
    edx = RD(0x004b8860u);
    /* 004846a3: add esp, 0xc */
    esp += 0xcu;
    /* 004846a6: mov dword ptr [esi + 0x1c], edx */
    WR(esi + 0x1cu, edx);
    /* 004846a9: pop edi */
    esp += 4u; edi = RD(esp - 4u);
    /* 004846aa: pop esi */
    esp += 4u; esi = RD(esp - 4u);
    /* 004846ab: pop ebp */
    esp += 4u; ebp = RD(esp - 4u);
    /* 004846ac: pop ebx */
    esp += 4u; ebx = RD(esp - 4u);
    /* 004846ad: add esp, 0x70 */
    esp += 0x70u;
    /* 004846b0: ret */
    esp += 4u;

    memcpy(cg->st, xs, sizeof xs);
    cg->ftop = xt;
    cg->r[EAX] = eax; cg->r[ECX] = ecx; cg->r[EDX] = edx;
    cg->r[EBX] = ebx; cg->r[EBP] = ebp; cg->r[ESI] = esi; cg->r[EDI] = edi;
    cg->r[ESP] = esp;
}


void wmain_00483e60(cpu_t *cg)
{
    if (sv_off()) { wmain_00483e60_ref(cg); return; }
    { static int once; if (!once) { once = 1; atexit(sv_at_exit); } }
    sv_calls++;
    sv_body(cg);
}
