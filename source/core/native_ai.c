/* Native 0x00422130: the per-frame actor AI sweep.
 *
 * Walks 64 actor slots at 0x6bfa50 (stride 0x124), skips inactive ones, and for each
 * live actor runs the distance test against the world's reference point and then
 * either the "out of range" bookkeeping (0x8a0200 = 2) or the transform (0x422427 /
 * 0x420db2). 224 lifted instructions; in the actor-heavy scenes it is worth ~19 ms
 * per frame (docs/PLAN.md), which makes it the single biggest win of the project.
 *
 * THE REGISTER CONTRACT (why this function needs a differential test)
 * ------------------------------------------------------------------
 * The lifter reduces the state sync around a call to a register set derived from the
 * callee's lifted body. For an override that body no longer runs, so lift.py now
 * treats every override as "clobbers every register and uses x87" and the callers do a
 * full sync. 0x422130 has exactly one call site (0x0041fcbe in FUN_0041fb..); the
 * generated caller loads EAX, ECX, EDX, ESP and EBP back out of cg->r after the call,
 * so this native must leave every one of them exactly as the lifted reference body
 * does. It also means EBP: the reference restores the caller's EBP with its
 * `mov esp,ebp; pop ebp` epilogue, so a callee that scribbles EBP must not leak out.
 * The transcription below therefore mirrors every integer register write of the
 * listing (docs/asm/00422130_listing.txt) and the epilogue writes EAX/ECX/EDX/EBP/ESP
 * like the reference: EAX = 1, ECX/EDX whatever the loop left, EBP the caller's.
 *
 * Transcription notes:
 *  - `ebp` is the original's frame pointer (E - 4); locals are addressed exactly like
 *    the original so anything a callee leaves in the frame comes out the same. The
 *    frame itself is below ESP and is never observed, so it lives in C locals here.
 *  - x87: `ft0` is the lifter's static slot base and `st[(ft0 + k) & 7]` the slots;
 *    at a call site the reference spills the slots and sets TOP = ft0 - depth, and
 *    rebases ft0 = TOP + depth afterwards (fpu_static.py). The only call site with a
 *    non-zero depth is the sqrt at 0x4222d7 (depth 1), which is why ai_sync/ai_reload
 *    take the depth: with it, the callee sees its argument in ST(0) and the status
 *    word that `fnstsw ax` stores (bits 11-13 = TOP) matches the reference too.
 *  - The epilogue writes back st[(ft0+6)], st[(ft0+7)] and TOP = ft0 only; slot 5 is
 *    dead at the return, so the reference leaves st[(ft0+5)] alone and so does this.
 *  - Comparisons use FCOM() so the status-word bits the code tests (C0/C2/C3) are the
 *    emulated ones; 0x49a390 is the x87 sqrt on ST(0).
 *
 * Listing: docs/asm/00422130_listing.txt. Verification: tests/test_override.c
 * ("00422130" in two shapes) plus the 2400-frame golden A/B.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_00422130_ref(cpu_t *c);
void wmain_004214bf(cpu_t *c);
void wmain_0040e42a(cpu_t *c);
void wmain_0041f910(cpu_t *c);
void wmain_0041f700(cpu_t *c);
void wmain_0041f80b(cpu_t *c);
void wmain_0041fa37(cpu_t *c);
void wmain_00447d18(cpu_t *c);
void wmain_0049a390(cpu_t *c);
void wmain_0041f929(cpu_t *c);
void wmain_0047e34d(cpu_t *c);
void wmain_0047840e(cpu_t *c);
void wmain_00422427(cpu_t *c);
void wmain_00420db2(cpu_t *c);

/* A/B switch, same policy as the other natives: nonative.txt turns every native off,
 * noai.txt only this one (it is the only switch that isolates the AI sweep on the
 * device), and TPM_AX_OFF does the same in the host environment. The golden A/B runs
 * need it. */
int g_ai_native_off;
static int na_ai_off(void)
{
    extern int g_native_off;
#ifdef __3DS__
    return g_native_off || g_ai_native_off;
#else
    static int off = -1;
    if (off < 0) off = (g_native_off != 0) || (g_ai_native_off != 0) || getenv("TPM_AX_OFF") != NULL;
    return off;
#endif
}

typedef struct {
    cpu_t *cg;
    uint8_t *gm;
    uint32_t gsz, E, F;
    uint32_t ebp;            /* the caller's EBP, restored by the epilogue */
    double fs5, fs6, fs7;    /* the lifted slots st[(ft0+5..7)] */
    uint32_t ft0;            /* the lifter's slot base (TOP at depth 0) */
} ai_t;

static float ai_f(uint8_t *gm, uint32_t gsz, uint32_t a)
{
    uint32_t u = gmem_rd32_l(gm, gsz, a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* Spill the tracked x87 slots and set TOP = ft0 - depth, as the lifter emits at a
 * call site (tools/lifter/fpu_static.py). */
static void ai_sync(ai_t *A, uint32_t depth)
{
    cpu_t *c = A->cg;
    const uint32_t t = A->ft0;
    c->st[(t + 5u) & 7u] = A->fs5;
    c->st[(t + 6u) & 7u] = A->fs6;
    c->st[(t + 7u) & 7u] = A->fs7;
    c->ftop = (t - depth) & 7u;
}

/* After the call: rebase ft0 on the TOP the callee left and reload the slots. */
static void ai_reload(ai_t *A, uint32_t depth)
{
    cpu_t *c = A->cg;
    const uint32_t t = (c->ftop + depth) & 7u;
    A->ft0 = t;
    A->fs5 = c->st[(t + 5u) & 7u];
    A->fs6 = c->st[(t + 6u) & 7u];
    A->fs7 = c->st[(t + 7u) & 7u];
}

/* One cdecl guest call: push the args, call. `depth`/`fpu` are the lifter's x87 facts
 * for the call site: `fpu` says the callee may read or write the x87 stack, in which
 * case the reference spills the tracked slots and rebases them afterwards (with
 * TOP = ft0 - depth before the call). Six of the seven x87 call sites have depth 0;
 * the sqrt at 0x4222d7 has depth 1. The other call sites have fpu == 0: the reference
 * does not touch cg->ftop or the slots there, and neither does this. */
static void ai_call(ai_t *A, void (*fn)(cpu_t *), uint32_t ret, uint32_t n,
                    const uint32_t *a, uint32_t depth, int fpu)
{
    cpu_t *c = A->cg;
    gptr s = A->F - 0x88u - 4u * (n + 1u);
    uint32_t i;
    if (fpu) ai_sync(A, depth);
    for (i = 0; i < n; i++) gmem_wr32_l(A->gm, A->gsz, s + 4u + 4u * i, a[i]);
    gmem_wr32_l(A->gm, A->gsz, s, ret);
    c->r[ESP] = s;
    fn(c);
    c->r[ESP] = A->E;
    if (fpu) ai_reload(A, depth);
}

/* fnstsw ax: low half = status word with TOP = ft0 (depth 0 at every such point). */
#define AI_FSW_AX() (R[EAX] = (R[EAX] & 0xffff0000u) | (cpu_fsw_at(cg, a.ft0) & 0xffffu))

/* Call counter (one increment per call): with the sweep called at most once or twice
 * per frame, the number says whether the native is on the frame's hot path at all.
 * Reported on stderr at exit, like the actor natives' verify counters. */
static uint64_t g_ai_calls;
static void ai_at_exit(void) { fprintf(stderr, "ai native: %llu calls\n", (unsigned long long)g_ai_calls); }
static void ai_count(void)
{
    static int init;
    if (!init) { init = 1; atexit(ai_at_exit); }
    g_ai_calls++;
}

void wmain_00422130(cpu_t *cg)
{
    ai_t a;
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    uint32_t *const R = cg->r;
    uint32_t E, i, n8, n3c, slot, args[4];

    if (na_ai_off()) { wmain_00422130_ref(cg); return; }
    ai_count();

    E = cg->r[ESP];
    a.cg = cg; a.gm = gm; a.gsz = gsz; a.E = E; a.F = E - 4u;
    a.ebp = cg->r[EBP];
    a.ft0 = cg->ftop;
    a.fs5 = cg->st[(a.ft0 + 5u) & 7u];
    a.fs6 = cg->st[(a.ft0 + 6u) & 7u];
    a.fs7 = cg->st[(a.ft0 + 7u) & 7u];

    /* 00422139/40/47/4e: [ebp-8] = 0, [ebp-0x3c] = 0, [ebp-0x44] = 0, [ebp-0x40] = 0x6bfa50 */
    n8 = 0;
    n3c = 0;
    i = 0;
    slot = 0x006bfa50u;
    goto L_42216c;                                          /* 00422155: jmp */

L_422157:                                                   /* 00422157: the loop increment */
    R[EAX] = i;                                             /* mov eax,[ebp-0x44] */
    R[EAX] = R[EAX] + 1u;                                   /* add eax,1 */
    i = R[EAX];                                             /* mov [ebp-0x44],eax */
    R[ECX] = slot;                                          /* mov ecx,[ebp-0x40] */
    R[ECX] = R[ECX] + 0x124u;                               /* add ecx,0x124 */
    slot = R[ECX];                                          /* mov [ebp-0x40],ecx */
L_42216c:
    if ((int32_t)i >= 0x40) goto L_42241e;                  /* cmp [ebp-0x44],0x40 / jge */
    R[EDX] = slot;                                          /* 00422176: mov edx,[ebp-0x40] */
    if (gmem_rd32_l(gm, gsz, slot + 0x10u) == 0u)           /* cmp [edx+0x10],0 / jne */
        goto L_422157;                                      /* 0042217f: jmp */
    /* 00422181 */
    R[EAX] = slot;                                          /* mov eax,[ebp-0x40] */
    R[ECX] = gmem_rd32_l(gm, gsz, R[EAX] + 0x14u);          /* mov ecx,[eax+0x14] */
    R[ECX] &= 0x10u;                                        /* and ecx,0x10 */
    if (R[ECX] == 0u) goto L_4221c8;                        /* test/je */
    R[EDX] = slot;                                          /* 0042218e: mov edx,[ebp-0x40] */
    R[EAX] = gmem_rd32_l(gm, gsz, R[EDX] + 0x14u);          /* mov eax,[edx+0x14] */
    R[EAX] &= 0x10000u;                                     /* and eax,0x10000 */
    if (R[EAX] == 0u) goto L_4221c8;
    R[ECX] = slot;                                          /* 0042219d: mov ecx,[ebp-0x40] */
    if (gmem_rd32_l(gm, gsz, R[ECX] + 0xd0u) != 0u) goto L_4221c8;   /* cmp/jne */
    /* 004221a9: clear bits 0x10/0x10000 and hand the slot to 0x4214bf */
    R[EDX] = slot;                                          /* mov edx,[ebp-0x40] */
    R[EAX] = gmem_rd32_l(gm, gsz, R[EDX] + 0x14u);          /* mov eax,[edx+0x14] */
    R[EAX] &= 0xfffeffefu;                                  /* and eax,0xfffeffef */
    R[ECX] = slot;                                          /* mov ecx,[ebp-0x40] */
    gmem_wr32_l(gm, gsz, R[ECX] + 0x14u, R[EAX]);           /* mov [ecx+0x14],eax */
    R[EDX] = i;                                             /* mov edx,[ebp-0x44] */
    args[0] = R[EDX];                                       /* push edx */
    ai_call(&a, wmain_004214bf, 0x004221c3u, 1, args, 0u, 1);
    goto L_422157;                                          /* 004221c6: jmp */

L_4221c8:                                                   /* 004221c8 */
    R[EAX] = slot;                                          /* mov eax,[ebp-0x40] */
    R[ECX] = gmem_rd32_l(gm, gsz, R[EAX] + 0x14u);          /* mov ecx,[eax+0x14] */
    R[ECX] &= 0x10u;                                        /* and ecx,0x10 */
    if (R[ECX] != 0u) goto L_42234d;                        /* test/jne */
    /* 004221d9: the in-range / move-toward-target path */
    R[EDX] = slot;                                          /* mov edx,[ebp-0x40] */
    FCOM(cg, (double)ai_f(gm, gsz, R[EDX] + 0x120u),        /* fld [edx+0x120]; fcomp [0x4a823c] */
              (double)ai_f(gm, gsz, 0x004a823cu));
    AI_FSW_AX();                                            /* 004221e8: fnstsw ax */
    if (!(R[EAX] & 0x4000u)) goto L_42220d;                 /* 004221ea: test ah,0x40 / je */
    /* 004221ef: ask the engine for the limit */
    R[EAX] = a.F - 0x54u;                                   /* lea eax,[ebp-0x54] */
    args[0] = gmem_rd32_l(gm, gsz, 0x008a0060u);            /* mov ecx,[0x8a0060] */
    args[1] = R[EAX];                                       /* push eax */
    ai_call(&a, wmain_0040e42a, 0x004221ffu, 2, args, 0u, 0);
    gmem_wr32_l(gm, gsz, a.F - 0x7cu, R[EAX]);              /* 00422202: mov [ebp-0x7c],eax */
    gmem_wr32_l(gm, gsz, a.F - 4u,                          /* 00422205: fild [ebp-0x7c]; fstp [ebp-4] */
                gmem_u32_f((float)(int32_t)gmem_rd32_l(gm, gsz, a.F - 0x7cu)));
    goto L_422219;                                          /* 0042220b: jmp */
L_42220d:                                                   /* 0042220d */
    R[EDX] = slot;                                          /* mov edx,[ebp-0x40] */
    R[EAX] = gmem_rd32_l(gm, gsz, R[EDX] + 0x120u);         /* 00422210: mov eax,[edx+0x120] */
    gmem_wr32_l(gm, gsz, a.F - 4u, R[EAX]);                 /* 00422216: mov [ebp-4],eax */
L_422219:
    FCOM(cg, (double)ai_f(gm, gsz, a.F - 4u),               /* fld [ebp-4]; fcomp [0x4a8260] */
              (double)ai_f(gm, gsz, 0x004a8260u));
    AI_FSW_AX();                                            /* 00422222: fnstsw ax */
    if (R[EAX] & 0x0100u)                                   /* 00422224: test ah,1 / je */
        gmem_wr32_l(gm, gsz, a.F - 4u, 0x40800000u);        /* 00422229: mov [ebp-4],0x40800000 */
    /* 00422230 */
    R[ECX] = slot;                                          /* mov ecx,[ebp-0x40] */
    if (gmem_rd32_l(gm, gsz, R[ECX] + 0xccu) != 0u) {       /* cmp [ecx+0xcc],0 / je */
        R[EDX] = slot;                                      /* 0042223c: mov edx,[ebp-0x40] */
        R[EAX] = gmem_rd32_l(gm, gsz, R[EDX] + 0xccu);      /* mov eax,[edx+0xcc] */
        R[EAX] = R[EAX] + 0x6cu;                            /* add eax,0x6c */
        args[0] = R[EAX];                                   /* push eax */
        ai_call(&a, wmain_0041f910, 0x0042224eu, 1, args, 0u, 0);
        goto L_422258;                                      /* 00422251: jmp */
    } else {                                                /* 00422253 */
        ai_call(&a, wmain_0041f700, 0x00422253u, 0, args, 0u, 0);
    }
L_422258:                                                   /* 00422258 */
    R[ECX] = slot;                                          /* mov ecx,[ebp-0x40] */
    R[ECX] = R[ECX] + 0xc0u;                                /* add ecx,0xc0 */
    args[0] = R[ECX];                                       /* push ecx */
    ai_call(&a, wmain_0041f80b, 0x00422267u, 1, args, 0u, 1);
    R[EDX] = a.F - 0x64u;                                   /* 0042226a: lea edx,[ebp-0x64] */
    args[0] = R[EDX];                                       /* push edx */
    ai_call(&a, wmain_0041fa37, 0x00422273u, 1, args, 0u, 0);
    ai_call(&a, wmain_00447d18, 0x0042227bu, 0, args, 0u, 0);
    gmem_wr32_l(gm, gsz, a.F - 0x58u, R[EAX]);              /* 0042227b: mov [ebp-0x58],eax */
    if (gmem_rd32_l(gm, gsz, a.F - 0x58u) != 0u)            /* 0042227e: cmp [ebp-0x58],0 / jne */
        goto L_42229f;
    R[EAX] = gmem_rd32_l(gm, gsz, 0x008a011cu);             /* 00422284: mov eax,[0x8a011c] */
    R[EAX] = R[EAX] + 0x24u;                                /* add eax,0x24 */
    R[ECX] = gmem_rd32_l(gm, gsz, R[EAX]);                  /* mov ecx,[eax] */
    gmem_wr32_l(gm, gsz, a.F - 0x70u, R[ECX]);              /* mov [ebp-0x70],ecx */
    R[EDX] = gmem_rd32_l(gm, gsz, R[EAX] + 4u);             /* mov edx,[eax+4] */
    gmem_wr32_l(gm, gsz, a.F - 0x6cu, R[EDX]);              /* mov [ebp-0x6c],edx */
    R[EAX] = gmem_rd32_l(gm, gsz, R[EAX] + 8u);             /* mov eax,[eax+8] */
    gmem_wr32_l(gm, gsz, a.F - 0x68u, R[EAX]);              /* mov [ebp-0x68],eax */
    goto L_4222b6;                                          /* 0042229d: jmp */
L_42229f:
    R[ECX] = gmem_rd32_l(gm, gsz, a.F - 0x58u);             /* 0042229f: mov ecx,[ebp-0x58] */
    R[ECX] = R[ECX] + 0x18u;                                /* add ecx,0x18 */
    R[EDX] = gmem_rd32_l(gm, gsz, R[ECX]);                  /* mov edx,[ecx] */
    gmem_wr32_l(gm, gsz, a.F - 0x70u, R[EDX]);              /* mov [ebp-0x70],edx */
    R[EAX] = gmem_rd32_l(gm, gsz, R[ECX] + 4u);             /* mov eax,[ecx+4] */
    gmem_wr32_l(gm, gsz, a.F - 0x6cu, R[EAX]);              /* mov [ebp-0x6c],eax */
    R[ECX] = gmem_rd32_l(gm, gsz, R[ECX] + 8u);             /* mov ecx,[ecx+8] */
    gmem_wr32_l(gm, gsz, a.F - 0x68u, R[ECX]);              /* mov [ebp-0x68],ecx */
L_4222b6: {
    /* fld [ebp-0x64]; fsub [ebp-0x70]; fld [ebp-0x64]; fsub [ebp-0x70]; fmulp;
       fld [ebp-0x60]; fsub [ebp-0x6c]; fld [ebp-0x60]; fsub [ebp-0x6c]; fmulp;
       faddp; fst [ebp-0x78]  - two x87 products and their sum, each rounded to
       double, then stored as a float (the lifter's model of the x87 in doubles). */
    float dx = ai_f(gm, gsz, a.F - 0x64u) - ai_f(gm, gsz, a.F - 0x70u);
    float dy = ai_f(gm, gsz, a.F - 0x60u) - ai_f(gm, gsz, a.F - 0x6cu);
    double d2 = (double)dx * (double)dx + (double)dy * (double)dy;

    gmem_wr32_l(gm, gsz, a.F - 0x78u, gmem_u32_f((float)d2));   /* 004222d4: fst [ebp-0x78] */
    a.fs7 = d2;                                             /* 004222d7: ST(0) = d2, depth 1 */
    ai_call(&a, wmain_0049a390, 0x004222dcu, 0, args, 1u, 1);  /* sqrt */
    /* 004222dc: fst dword [ebp-0x80] - the double on the stack, rounded to float */
    gmem_wr32_l(gm, gsz, a.F - 0x80u, gmem_u32_f((float)a.fs7));
    FCOM(cg, a.fs7, (double)ai_f(gm, gsz, a.F - 4u));       /* 004222df: fcomp [ebp-4] */
    AI_FSW_AX();                                            /* 004222e2: fnstsw ax */
    if (!(R[EAX] & 0x4100u)) {                              /* 004222e4: test ah,0x41 / jne */
        gmem_wr32_l(gm, gsz, 0x008a0200u, 2u);              /* 004222e9: out of range */
        goto L_42234b;                                      /* 004222f3: jmp */
    }
}
    /* 004222f5: transform the point into the slot. cdecl: the last push is arg 0,
     * so 0x47e34d(out=(ebp-0x50), r=(ebp-0x14), M=[0x8a011c]+0x44+8) - the
     * reference pushes M, r, out in that order. */
    R[EDX] = a.F - 0x38u;                                   /* 004222f5: lea edx,[ebp-0x38] */
    args[0] = R[EDX];                                       /* push edx */
    ai_call(&a, wmain_0041f929, 0x004222feu, 1, args, 0u, 0);
    R[EAX] = gmem_rd32_l(gm, gsz, 0x008a011cu);             /* 00422301: mov eax,[0x8a011c] */
    R[ECX] = gmem_rd32_l(gm, gsz, R[EAX] + 0x44u);          /* mov ecx,[eax+0x44] */
    R[ECX] = R[ECX] + 8u;                                   /* add ecx,8 */
    args[2] = R[ECX];                                       /* push ecx */
    R[EDX] = a.F - 0x14u;                                   /* lea edx,[ebp-0x14] */
    args[1] = R[EDX];                                       /* push edx */
    R[EAX] = a.F - 0x50u;                                   /* lea eax,[ebp-0x50] */
    args[0] = R[EAX];                                       /* push eax */
    ai_call(&a, wmain_0047e34d, 0x0042231au, 3, args, 0u, 1);
    R[ECX] = slot;                                          /* 0042231d: mov ecx,[ebp-0x40] */
    R[EDX] = gmem_rd32_l(gm, gsz, R[ECX] + 0x11cu);         /* mov edx,[ecx+0x11c] */
    gmem_wr32_l(gm, gsz, a.F - 0x74u, R[EDX]);              /* mov [ebp-0x74],edx */
    R[EAX] = gmem_rd32_l(gm, gsz, a.F - 0x74u);             /* mov eax,[ebp-0x74] */
    args[2] = R[EAX];                                       /* push eax */
    R[ECX] = a.F - 0x50u;                                   /* lea ecx,[ebp-0x50] */
    args[1] = R[ECX];                                       /* push ecx */
    R[EDX] = gmem_rd32_l(gm, gsz, 0x008a011cu);             /* mov edx,[0x8a011c] */
    R[EAX] = gmem_rd32_l(gm, gsz, R[EDX] + 0x44u);          /* mov eax,[edx+0x44] */
    R[ECX] = gmem_rd32_l(gm, gsz, R[EAX] + 0x48u);          /* mov ecx,[eax+0x48] */
    args[0] = R[ECX];                                       /* push ecx -> 0x47840e(arg0, &out, slot+0x11c) */
    ai_call(&a, wmain_0047840e, 0x00422343u, 3, args, 0u, 1);
    gmem_wr32_l(gm, gsz, 0x008a0200u, R[EAX]);              /* 00422346: mov [0x8a0200],eax */
L_42234b:                                                   /* 0042234b */
    goto L_422363;                                          /* 0042234b: jmp */
L_42234d:                                                   /* 0042234d: fl & 0x10 */
    ai_call(&a, wmain_0041f700, 0x00422352u, 0, args, 0u, 0);
    gmem_wr32_l(gm, gsz, 0x008a0200u, 1u);                  /* 00422352: mov [0x8a0200],1 */
    gmem_wr32_l(gm, gsz, a.F - 4u, 0u);                     /* 0042235c: mov [ebp-4],0 */
L_422363:                                                   /* 00422363 */
    if (gmem_rd32_l(gm, gsz, 0x008a0200u) == 2u) {          /* cmp [0x8a0200],2 / jne */
        /* 0042236c: the "arrived" counter */
        R[EDX] = n8;                                        /* mov edx,[ebp-8] */
        R[EDX] = R[EDX] + 1u;                               /* add edx,1 */
        n8 = R[EDX];                                        /* mov [ebp-8],edx */
        R[EAX] = slot;                                      /* 00422375: mov eax,[ebp-0x40] */
        R[ECX] = gmem_rd32_l(gm, gsz, R[EAX] + 0x114u);     /* mov ecx,[eax+0x114] */
        R[ECX] = R[ECX] + 1u;                               /* add ecx,1 */
        R[EDX] = slot;                                      /* 00422381: mov edx,[ebp-0x40] */
        gmem_wr32_l(gm, gsz, R[EDX] + 0x114u, R[ECX]);      /* mov [edx+0x114],ecx */
        R[EAX] = slot;                                      /* 0042238a: mov eax,[ebp-0x40] */
        if ((int32_t)gmem_rd32_l(gm, gsz, R[EAX] + 0x114u) >= 0x1e) {   /* cmp 0x1e / jl */
            R[ECX] = slot;                                  /* 00422396: mov ecx,[ebp-0x40] */
            FCOM(cg, (double)ai_f(gm, gsz, R[ECX] + 0xbcu),  /* fld [ecx+0xbc]; fcomp [0x4a823c] */
                      (double)ai_f(gm, gsz, 0x004a823cu));
            AI_FSW_AX();                                    /* 004223a5: fnstsw ax */
            if ((R[EAX] & 0x4000u) &&                       /* test ah,0x40 / je */
                !(gmem_rd32_l(gm, gsz, slot + 0x14u) & 0x20000u)) {
                R[ECX] = slot;                              /* 004223bb: mov ecx,[ebp-0x40] */
                R[EDX] = gmem_rd32_l(gm, gsz, R[ECX] + 0x14u);   /* mov edx,[ecx+0x14] */
                R[EDX] |= 0x20000u;                         /* or edx,0x20000 */
                R[EAX] = slot;                              /* 004223c7: mov eax,[ebp-0x40] */
                gmem_wr32_l(gm, gsz, R[EAX] + 0x14u, R[EDX]);    /* mov [eax+0x14],edx */
                R[ECX] = slot;                              /* 004223cd: mov ecx,[ebp-0x40] */
                R[EDX] = gmem_rd32_l(gm, gsz, 0x006bfa48u); /* mov edx,[0x6bfa48] */
                gmem_wr32_l(gm, gsz, R[ECX] + 0x118u, R[EDX]);   /* mov [ecx+0x118],edx */
            }
        }
        goto L_422157;                                      /* 004223dc: jmp */
    }
    /* 004223e1: the "still moving" path. cdecl: 0x422427(slot, i) - i is pushed
     * first, so the slot is arg 0. */
    R[EAX] = i;                                             /* mov eax,[ebp-0x44] */
    args[1] = R[EAX];                                       /* push eax */
    R[ECX] = slot;                                          /* mov ecx,[ebp-0x40] */
    args[0] = R[ECX];                                       /* push ecx */
    ai_call(&a, wmain_00422427, 0x004223eeu, 2, args, 0u, 1);
    R[EDX] = slot;                                          /* 004223f1: mov edx,[ebp-0x40] */
    R[EAX] = gmem_rd32_l(gm, gsz, R[EDX] + 0x14u);          /* mov eax,[edx+0x14] */
    R[EAX] &= 0x40000u;                                     /* and eax,0x40000 */
    if (R[EAX] == 0u) {                                     /* test/jne */
        R[ECX] = a.F - 0x38u;                               /* 00422400: lea ecx,[ebp-0x38] */
        args[1] = R[ECX];                                   /* push ecx */
        R[EDX] = i;                                         /* mov edx,[ebp-0x44] */
        args[0] = R[EDX];                                   /* push edx -> 0x420db2(i, &(ebp-0x38)) */
        ai_call(&a, wmain_00420db2, 0x0042240du, 2, args, 0u, 1);
    }
    R[EAX] = n3c;                                           /* 00422410: mov eax,[ebp-0x3c] */
    R[EAX] = R[EAX] + 1u;                                   /* add eax,1 */
    n3c = R[EAX];                                           /* mov [ebp-0x3c],eax */
    goto L_422157;                                          /* 00422419: jmp */

L_42241e:                                                   /* 0042241e: mov eax,1 */
    R[EAX] = 1u;
    /* 00422423: mov esp,ebp; 00422425: pop ebp - the caller's EBP comes back */
    R[EBP] = a.ebp;
    cg->r[ESP] = E + 4u;
    /* 00422426: ret - spill st[(ft0+6)], st[(ft0+7)] and TOP = ft0; slot 5 is dead. */
    cg->st[(a.ft0 + 6u) & 7u] = a.fs6;
    cg->st[(a.ft0 + 7u) & 7u] = a.fs7;
    cg->ftop = a.ft0;
}
