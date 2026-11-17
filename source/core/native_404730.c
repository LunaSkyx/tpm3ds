/* Native 0x00404730: the per-frame skeleton / bone-hierarchy update.
 *
 * 1023 instructions, 47 call sites, one call per frame, 5.64 ms/frame of inclusive
 * timer time with no timed children (docs/PLAN.md section 11) - the body is the cost,
 * which is why it is the one large native that pays.  A semantic transcription of the
 * instruction stream in build-host/skel_stream.txt (the authority; there is no
 * docs/asm listing for this function), with the lifted body (wmain_00404730_ref) as
 * the executable reference.
 *
 * What it does (two paths, chosen by the 0x8822a8 flag):
 *  - [0x8822a8] == 0 (0x404e66): the "no world" path, which seeds the camera/screen
 *    state from [0x868718] and 0x6f83e4, builds a projection with 0x486828, converts
 *    the four screen corners with __frnd+__ftol to get a clip rectangle, and then
 *    walks that rectangle's grid cells (ctx+0x70/0x74/0x90), frustum-culling each
 *    cell against the planes in 0x59dec8 and dispatching 0x405cf0 / 0x47e40a /
 *    0x4064b0 per cell.  Ends by zeroing 0x59df98..0x59dfb4.
 *  - otherwise (0x404757): the world path.  Clears the per-frame tables
 *    (0x8bfaa0, 0x8bc920, 0x8bc880), publishes ctx fields into the 0x59dfxx block,
 *    converts the camera position, computes the view rectangle and the 0x8be9xx
 *    bookkeeping, and inside row/column loops scans the cell grid, collecting the
 *    visible objects into 0x8bea1c (0x403ef0 / 0x403fa0) and submitting them to
 *    0x4056c0.  Returns after storing the 8-bit "something changed" flag in 0x8be920.
 *
 * Contract (why this is a transcription, not a lifted-body copy)
 * ---------------------------------------------------------------
 * The original is `sub esp,0x90; push ebx/ebp/esi/edi` with no frame pointer (ebp is
 * the argument, ctx), so the whole function runs at one stack frame.  This file keeps
 * ESP as an explicit C local and mirrors every push/sub/pop/lea literally, so the
 * guest stack frame is byte-identical to the reference: the reference body's [esp+N]
 * slots and the frame a callee may write through a pointer (0x47e34d is handed
 * S+0x54) are the real guest addresses here too.  Registers live in C locals; the
 * x87 stack is an explicit physical 8-slot model in doubles (`xs[]` + `xt`, st(k) =
 * xs[(xt+k)&7]), initialised from cg->st[]/cg->ftop and written back at the epilogue,
 * so the arithmetic is the reference's own (values stay double until an fstp narrows
 * them) and the call sites see/leave the same x87 state.  At every call all integer
 * registers and all eight slots are published to cg and read back afterwards: that is
 * exactly the hardware state (callee-saved registers come back, caller-saved ones are
 * the callee's), which is also what the reference body's per-call facts reproduce.
 *
 * The original pushes ebx/ebp/esi/edi and restores them in the epilogue, so the
 * native restores all four (the caller-visible contract for an override: leave every
 * register as the lifted body would).  EAX/ECX/EDX are the body's final values and
 * ESP = entry ESP + 4.  Flags: this native does not publish cg->eflags; the original
 * body ends with whatever its last cmp/test set, but every branch in the guest is
 * preceded by its own flag producer, and no callee of this function consumes incoming
 * flags (checked: the only eflags read in a callee prologue is DF for rep movs,
 * which this native never changes either).  cg->fsw is maintained by FCOM() exactly
 * like the reference.
 *
 * A/B: TPM_AX_OFF (host) / nonative.txt (3DS) falls back to wmain_00404730_ref; the
 * call counter below prints on exit, so a passing A/B cannot be a never-called one.
 * Verified: level1_bench.txt, 3150 frames, 0 diff on the framebuffer hash with
 * "skeleton native: 2151 calls".  (golden_long.txt does not reach the level.)
 *
 * Two things that bit the transcription and are worth knowing:
 *  - The caller can enter with a NON-EMPTY x87 stack (the level code leaks slots from
 *    earlier float calls: the first frame entered with TOP=2).  The entry top is
 *    preserved and the body only uses the slots below it, so the native must track the
 *    physical stack, not assume depth 0.  The reference's static slots do the same.
 *  - One fcomp lost its pop in the first draft (0x4051b1) and the whole stack stayed
 *    one deep from then on.  The A/B showed it at frame 272; a shadow run (mine and
 *    wmain_00404730_ref on the same entry cpu_t + memory snapshot, comparing registers,
 *    x87 and the arena) pinned it in one run.  Every fcom/fcomp/fcompp in the listing
 *    needs its FPOP()/two FPOPs here.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_00404730_ref(cpu_t *c);
void wmain_00403ef0(cpu_t *c);
void wmain_00403fa0(cpu_t *c);
void wmain_00404180(cpu_t *c);
void wmain_004056c0(cpu_t *c);
void wmain_00405cf0(cpu_t *c);
void wmain_004064b0(cpu_t *c);
void wmain_0040e42a(cpu_t *c);
void wmain_0041f700(cpu_t *c);
void wmain_0041f71b(cpu_t *c);
void wmain_0041f770(cpu_t *c);
void wmain_0041f7df(cpu_t *c);
void wmain_0041f80b(cpu_t *c);
void wmain_0041f837(cpu_t *c);
void wmain_0041fa37(cpu_t *c);
void wmain_0047e34d(cpu_t *c);
void wmain_0047e40a(cpu_t *c);
void wmain_00486828(cpu_t *c);
void wmain_00494238(cpu_t *c);
void wmain_0049428d(cpu_t *c);
void wmain_004942bf(cpu_t *c);
void wmain_0049a44c(cpu_t *c);      /* CRT __ftol: ST(0) -> EAX, pops */
void wmain_0049a480(cpu_t *c);      /* CRT __frnd: double argument on the stack -> ST(0) */

/* A/B switch, same policy as the other natives (TPM_AX_OFF on the host, nonative.txt
 * / g_native_off on the device). */
static int sk_off(void)
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

static uint64_t sk_calls;
static void sk_at_exit(void) { fprintf(stderr, "skeleton native: %llu calls\n", (unsigned long long)sk_calls); }

#define RD(a)       gmem_rd32_l(gm, gsz, (a))
#define WR(a, v)    gmem_wr32_l(gm, gsz, (a), (uint32_t)(v))
#define RD8(a)      gmem_rd8_l(gm, gsz, (a))
#define WR8(a, v)   gmem_wr8_l(gm, gsz, (a), (uint8_t)(v))
#define RDF(a)      ((double)gmem_f32_l(RD(a)))
#define WRF(a, v)   WR((a), gmem_u32_f((float)(v)))
#define PUSH(v)     do { esp -= 4u; WR(esp, (v)); } while (0)

/* x87: physical stack, xs[(xt + k) & 7] == st(k).  Values stay double until an
 * fstp/fst narrows them, exactly like the reference's static slots. */
#define XS(k)       xs[(xt + (uint32_t)(k)) & 7u]
#define FPUSH(v)    do { double v_ = (v); xt = (xt - 1u) & 7u; xs[xt] = v_; } while (0)
#define FPOP()      do { xt = (xt + 1u) & 7u; } while (0)
#define FXCH(k)     do { double t_ = XS(0); XS(0) = XS(k); XS(k) = t_; } while (0)
#define FST32(a)    WRF((a), XS(0))
#define FSTP32(a)   do { WRF((a), XS(0)); FPOP(); } while (0)
#define FLD32(a)    FPUSH(RDF(a))
#define FILD32(a)   FPUSH((double)(int32_t)RD(a))
#define FCOM_M(a)   FCOM(cg, XS(0), RDF(a))
#define FNSTSW_AX() do { eax = (eax & 0xffff0000u) | (cpu_fsw_at(cg, xt) & 0xffffu); } while (0)

/* One cdecl guest call: the argument pushes are the literal PUSH() lines above, this
 * only pushes the return address, publishes the register/x87 state, calls and reloads.
 * ESP after the call is the caller's ESP again (the callee's `ret` popped the address). */
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

static void sk_body(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi, esp;
    double xs[8];
    uint32_t xt, i;
    int32_t df;                       /* rep stos/movs direction step */

    eax = cg->r[EAX]; ecx = cg->r[ECX]; edx = cg->r[EDX];
    ebx = cg->r[EBX]; ebp = cg->r[EBP]; esi = cg->r[ESI]; edi = cg->r[EDI];
    esp = cg->r[ESP];
    memcpy(xs, cg->st, sizeof xs);
    xt = cg->ftop;
    df = (cg->eflags & F_DF) ? -1 : 1;   /* DF is never changed by this function */

    /* 00404730: sub esp, 0x90 */
    esp -= 0x90u;
    /* 00404736: push ebx */
    PUSH(ebx);
    /* 00404737: push ebp */
    PUSH(ebp);
    /* 00404738: mov ebp, dword ptr [esp + 0x9c]   (the argument, ctx) */
    ebp = RD(esp + 0x9cu);
    /* 0040473f: xor ebx, ebx */
    ebx = 0;
    /* 00404741: push esi */
    PUSH(esi);
    /* 00404744: push edi */
    PUSH(edi);
    /* 00404742/45: cmp ebp,ebx / je 0x4056a9 */
    if (ebp == 0u) goto L_004056a9;

    /* 0040474b: cmp dword ptr [0x8822a8], ebx / je 0x404e66 */
    if (RD(0x008822a8u) == 0u) goto L_00404e66;

    /* 00404757: mov eax, dword ptr [0x6f83e4] */
    eax = RD(0x006f83e4u);
    /* 0040475c: mov dword ptr [0x59df4c], ebx */
    WR(0x0059df4cu, ebx);
    /* 00404762: cmp eax, ebx / je 0x4056a9 */
    if (eax == 0u) goto L_004056a9;
    /* 0040476a: cmp dword ptr [ebp + 0x90], ebx / je 0x4056a9 */
    if (RD(ebp + 0x90u) == 0u) goto L_004056a9;
    /* 00404776: lea eax, [ebp + 0xc4] */
    eax = ebp + 0xc4u;
    /* 0040477c: test eax, eax / je 0x4056a9  (never taken: ebp != 0) */
    if (eax == 0u) goto L_004056a9;
    /* 00404784: cmp dword ptr [ebp + 0xb8], ebx / je 0x4056a9 */
    if (RD(ebp + 0xb8u) == 0u) goto L_004056a9;

    /* 00404790: mov edi,0x8bfaa0 / mov ecx,0x3f / rep stosd; stosw; stosb  (255 bytes) */
    ecx = 0x3fu; edi = 0x008bfaa0u;
    while (ecx) { WR(edi, 0u); edi += 4 * df; ecx--; }
    WR(edi, 0u); edi += 2 * df;          /* stosw */
    WR8(edi, 0u); edi += df;             /* stosb */
    /* 004047a7: mov ecx, [ebp + 0xc] / xor eax,eax / inc ecx; rep stosd|stosb at 0x8bc920 */
    ecx = RD(ebp + 0xcu) + 1u; eax = 0;
    {
        uint32_t n = ecx, edi0 = 0x008bc920u;
        edi = edi0;
        for (i = 0; i < n / 4u; i++) { WR(edi, 0u); edi += 4 * df; }
        for (i = 0; i < (n & 3u); i++) { WR8(edi, 0u); edi += df; }
    }
    /* 004047c0: mov ecx,0x21 / rep stosd at 0x8bc880  (0x84 bytes) */
    ecx = 0x21u; edi = 0x008bc880u; eax = 0;
    while (ecx) { WR(edi, 0u); edi += 4 * df; ecx--; }

    /* 004047ce: mov eax,[ebp+0x50] / mov [0x59df38],eax */
    eax = RD(ebp + 0x50u);
    WR(0x0059df38u, eax);
    /* 004047d6: mov ecx,[ebp+0xbc] / mov [0x59df88],ecx */
    ecx = RD(ebp + 0xbcu);
    WR(0x0059df88u, ecx);
    /* 004047e2: mov [0x59df58], ebp */
    WR(0x0059df58u, ebp);
    /* 0040479c: mov esi, dword ptr [0x8a011c] */
    esi = RD(0x008a011cu);

    /* 004047e8: fld [esi+0x38]; call __ftol; [0x8be940]=eax */
    FLD32(esi + 0x38u);
    ZCALL(wmain_0049a44c, 0x004047f0u);
    WR(0x008be940u, eax);
    /* 004047f5: fld [esi+0x24]; fsub [0x4a8054]; __ftol; edi=eax; [0x8be98c]=edi */
    FLD32(esi + 0x24u);
    XS(0) -= RDF(0x004a8054u);
    ZCALL(wmain_0049a44c, 0x00404803u);
    edi = eax;
    WR(0x008be98cu, edi);
    /* 0040480b: fld [esi+0x28]; fsub [0x4a8054]; __ftol; [0x8be990]=eax */
    FLD32(esi + 0x28u);
    XS(0) -= RDF(0x004a8054u);
    ZCALL(wmain_0049a44c, 0x00404819u);
    WR(0x008be990u, eax);

    /* 0040481e: lea edx,[edi-2] / 0040482d: add edi,2 */
    edx = edi - 2u;
    /* 00404821: fld [esi+0x2c]; fsub [0x4a8058] */
    FLD32(esi + 0x2cu);
    XS(0) -= RDF(0x004a8058u);
    /* 0040482a: lea ecx,[eax-2] / 00404830: add eax,2 */
    ecx = eax - 2u;
    edi += 2u;
    eax += 2u;
    /* 00404833/38: push 0x8be9c8; push 0x8be9c4 */
    PUSH(0x008be9c8u);
    PUSH(0x008be9c4u);
    /* 0040483d: fstp [0x59df78] */
    FSTP32(0x0059df78u);
    /* 00404843: fld [esi+0x2c]; fsub [0x4a8048] */
    FLD32(esi + 0x2cu);
    XS(0) -= RDF(0x004a8048u);
    /* 0040484c..5e: the four rounded screen rectangles */
    WR(0x0059df64u, edx);
    WR(0x0059df5cu, edi);
    WR(0x0059df6cu, ecx);
    WR(0x0059df68u, eax);
    WR(0x0059dfb8u, ebx);
    /* 00404869: fstp [0x59df74] */
    FSTP32(0x0059df74u);
    /* 0040486f: mov edx,[esi+0x38]; push edx; call 0x494238 */
    PUSH(RD(esi + 0x38u));
    ZCALL(wmain_00494238, 0x00404878u);
    /* 00404878: mov ecx,[0x8a011c] / add esp,0xc */
    ecx = RD(0x008a011cu);
    esp += 0xcu;

    /* 00404881: fld [ecx+0x34]; fcomp [0x4a805c]; fnstsw ax; test ah,1; jne 0x4048b0 */
    FLD32(ecx + 0x34u);
    FCOM_M(0x004a805cu);
    FPOP();
    FNSTSW_AX();
    if ((eax >> 8) & 1u) goto L_004048b0;
    /* 00404891: fld [ecx+0x34]; fcomp [0x4a8060]; fnstsw ax; test ah,0x41; je 0x4048b0 */
    FLD32(ecx + 0x34u);
    FCOM_M(0x004a8060u);
    FPOP();
    FNSTSW_AX();
    if (((eax >> 8) & 0x41u) == 0u) goto L_004048b0;
    /* 004048a1: fild [ebp+0x78]; [0x8be944]=1; jmp 0x4048b9 */
    FILD32(ebp + 0x78u);
    WR(0x008be944u, 1u);
    goto L_004048b9;
L_004048b0:
    /* 004048b0: [0x8be944]=ebx; fild [ebp+0x7c] */
    WR(0x008be944u, ebx);
    FILD32(ebp + 0x7cu);
L_004048b9:
    /* 004048b9: fstp [0x8be9cc] */
    FSTP32(0x008be9ccu);
    /* 004048bf: push ebp; call 0x404180 */
    PUSH(ebp);
    ZCALL(wmain_00404180, 0x004048c5u);
    /* 004048c5: fild [ebp+0x78] */
    FILD32(ebp + 0x78u);
    /* 004048c8: mov eax,[0x881344] / 004048cd: add esp,4 */
    eax = RD(0x00881344u);
    esp += 4u;
    /* 004048d2..de: the frame bookkeeping */
    WR(0x0059debcu, ebx);
    FSTP32(0x008be9ccu);
    WR(0x008be924u, ebx);
    WR8(0x0059df70u, 0u);
    /* 004048eb: jne 0x404957  (cmp eax,ebx at 0x4048d0) */
    if (eax != 0u) goto L_00404957;

    /* 004048ed: push 0x59df70; push ebp; call 0x40e42a */
    PUSH(0x0059df70u);
    PUSH(ebp);
    ZCALL(wmain_0040e42a, 0x004048f8u);
    /* 004048f8: mov dl,[0x59df48] / mov cl,[0x59df70] / mov esi,eax / mov al,dl;
     * add esp,8; xor al,cl; je 0x404922 */
    edx = (edx & 0xffffff00u) | (RD8(0x0059df48u) & 0xffu);
    ecx = (ecx & 0xffffff00u) | (RD8(0x0059df70u) & 0xffu);
    esi = eax;
    eax = (eax & 0xffffff00u) | (edx & 0xffu);
    esp += 8u;
    eax = (eax & 0xffffff00u) | (((eax & 0xffu) ^ (ecx & 0xffu)) & 0xffu);
    if ((eax & 0xffu) == 0u) goto L_00404922;
    /* 0040490f: [0x59df8c]=0x10; [0x59df90]=cl; [0x59df80]=dl */
    WR8(0x0059df8cu, 0x10u);
    WR8(0x0059df90u, ecx & 0xffu);
    WR8(0x0059df80u, edx & 0xffu);
L_00404922:
    /* 00404922: mov al,[0x59df8c]; mov [0x59df48],cl; test al,al; je 0x404938 */
    eax = (eax & 0xffffff00u) | (RD8(0x0059df8cu) & 0xffu);
    WR8(0x0059df48u, ecx & 0xffu);
    if ((eax & 0xffu) == 0u) goto L_00404938;
    /* 00404931: dec al; mov [0x59df8c],al */
    eax = (eax & 0xffffff00u) | (((eax & 0xffu) - 1u) & 0xffu);
    WR8(0x0059df8cu, eax & 0xffu);
L_00404938:
    /* 00404938: ecx=[0x8be9a0]; edx=[0x8be9ac]; eax=ecx; eax-=edx;
     * cmp esi,eax; jge 0x40495d */
    ecx = RD(0x008be9a0u);
    edx = RD(0x008be9acu);
    eax = ecx - edx;
    if ((int32_t)esi >= (int32_t)eax) goto L_0040495d;
    /* 0040494c: lea ecx,[esi+edx]; mov [0x8be9a0],ecx */
    ecx = esi + edx;
    WR(0x008be9a0u, ecx);
    goto L_0040495d;
L_00404957:
    /* 00404957: mov ecx, dword ptr [0x8be9a0] */
    ecx = RD(0x008be9a0u);
L_0040495d:
    /* 0040495d: cmp ecx,0x40; jle 0x40496d */
    if ((int32_t)ecx > 0x40) {
        /* 00404962: mov ecx,0x40; mov [0x8be9a0],ecx */
        ecx = 0x40u;
        WR(0x008be9a0u, ecx);
    }
    /* 0040496d: eax=0x100; edx=ecx+0x100; eax-=ecx; cmp ecx,ebx; [0x8be9ac]=ebx;
     * [esp+0x44]=edx; [esp+0x10]=eax; [0x8be970]=ebx; jle 0x404e49 */
    eax = 0x100u;
    edx = ecx + 0x100u;
    eax -= ecx;
    WR(0x008be9acu, ebx);
    WR(esp + 0x44u, edx);
    WR(esp + 0x10u, eax);
    WR(0x008be970u, ebx);
    if ((int32_t)ecx <= (int32_t)ebx) goto L_00404e49;

    /* 00404996: edx=[0x8be99c]; edi=[0x8be990]; esi=[0x8be98c] */
    edx = RD(0x008be99cu);
    edi = RD(0x008be990u);
    esi = RD(0x008be98cu);
L_004049a8:
    /* 004049a8: [0x8be9b4]=0 */
    WR(0x008be9b4u, 0u);
    /* 004049b2: ebx=[edx*8+0x8bea34]; ebx+=esi; esi=[edx*8+0x8bea38]; [esp+0x14]=ebx;
     * esi+=edi; fild [esp+0x14]; ebp=[esp+0xa4]; [esp+0x40]=esi */
    ebx = RD(edx * 8u + 0x008bea34u);
    ebx += esi;
    esi = RD(edx * 8u + 0x008bea38u);
    WR(esp + 0x14u, ebx);
    esi += edi;
    FILD32(esp + 0x14u);
    ebp = RD(esp + 0xa4u);
    WR(esp + 0x40u, esi);
    /* 004049d7: lea eax,[esp+0x54]; push 0x8be9ec; fstp [esp+0x58];
     * fild [esp+0x44]; push eax; push 0x8bea28; [esp+0x68]=0; fstp [esp+0x64];
     * call 0x47e34d  (0x8bea28, S+0x54, 0x8be9ec) */
    {
        uint32_t out = esp + 0x54u;
        PUSH(0x008be9ecu);
        FSTP32(esp + 0x58u);                /* [S+0x54] = (float)ebx */
        FILD32(esp + 0x44u);                /* [S+0x40] = esi */
        PUSH(out);
        PUSH(0x008bea28u);
        WR(esp + 0x68u, 0u);                /* [S+0x5c] = 0 */
        FSTP32(esp + 0x64u);                /* [S+0x58] = (float)esi */
        ZCALL(wmain_0047e34d, 0x004049ffu);
    }
    /* 004049ff: eax=[0x8bea30]; ecx=[0x8bea28]; edx=[0x8bea2c]; [0x8bea24]=eax;
     * eax=[0x59debc]; add esp,0xc; test eax,eax; [0x8bea1c]=ecx; [0x8bea20]=edx;
     * je 0x404a5e */
    eax = RD(0x008bea30u);
    ecx = RD(0x008bea28u);
    edx = RD(0x008bea2cu);
    WR(0x008bea24u, eax);
    eax = RD(0x0059debcu);
    esp += 0xcu;
    WR(0x008bea1cu, ecx);
    WR(0x008bea20u, edx);
    if (eax == 0u) goto L_00404a5e;

    /* 00404a2d: [0x8be9e8]=1 */
    WR(0x008be9e8u, 1u);
    /* 00404a37: ecx=[ebp+0x78]; dec ecx; [esp+0x14]=ecx; push ecx;
     * fild [esp+0x18]; fstp [esp]; push 0x8bea1c; push 3; call 0x403fa0
     * (3, 0x8bea1c, (float)(ecx-1)) */
    ecx = RD(ebp + 0x78u) - 1u;
    WR(esp + 0x14u, ecx);
    PUSH(ecx);
    FILD32(esp + 0x18u);
    FSTP32(esp);
    PUSH(0x008bea1cu);
    PUSH(3u);
    ZCALL(wmain_00403fa0, 0x00404a53u);
    esp += 0xcu;
    /* 00404a56: test eax,eax; jne 0x404e49 */
    if (eax != 0u) goto L_00404e49;

L_00404a5e:
    /* 00404a5e: edx=[0x8be99c]; edi=[esp+0x44]; ecx=edx; cmp edx,edi;
     * [0x8be974]=ecx; jg 0x404b71 */
    edx = RD(0x008be99cu);
    edi = RD(esp + 0x44u);
    ecx = edx;
    WR(0x008be974u, ecx);
    if ((int32_t)edx > (int32_t)edi) goto L_00404b71;

L_00404a78:
    /* 00404a78: eax=[ecx*8+0x8bea34]; edx=[0x8be98c]; eax+=edx; edx=[0x8be990];
     * [0x8be978]=eax; ecx=[ecx*8+0x8bea38]; ecx+=edx; eax-=ebx; [0x8be97c]=ecx;
     * ecx-=esi; push ecx; push eax; push 0x8bea1c; call 0x403ef0 */
    eax = RD(ecx * 8u + 0x008bea34u);
    edx = RD(0x008be98cu);
    eax += edx;
    edx = RD(0x008be990u);
    WR(0x008be978u, eax);
    ecx = RD(ecx * 8u + 0x008bea38u);
    ecx += edx;
    eax -= ebx;
    WR(0x008be97cu, ecx);
    ecx -= esi;
    PUSH(ecx);
    PUSH(eax);
    PUSH(0x008bea1cu);
    ZCALL(wmain_00403ef0, 0x00404ab1u);
    /* 00404ab1: ecx=[0x8be978]; eax=[0x8be97c]; add esp,0xc; ebx=ecx; test ecx,ecx;
     * esi=eax; jl 0x404b4b */
    ecx = RD(0x008be978u);
    eax = RD(0x008be97cu);
    esp += 0xcu;
    ebx = ecx;
    esi = eax;
    if ((int32_t)ecx < 0) goto L_00404b4b;
    /* 00404acb: edx=[ebp+0x70]; cmp ecx,edx; jge 0x404b4b */
    edx = RD(ebp + 0x70u);
    if ((int32_t)ecx >= (int32_t)edx) goto L_00404b4b;
    /* 00404ad2: test eax,eax; jl 0x404b4b */
    if ((int32_t)eax < 0) goto L_00404b4b;
    /* 00404ad6: cmp eax,[ebp+0x74]; jge 0x404b4b */
    if ((int32_t)eax >= (int32_t)RD(ebp + 0x74u)) goto L_00404b4b;
    /* 00404adb: imul eax,edx; edx=[ebp+0x90]; eax+=ecx; eax=[edx+eax*4];
     * test eax,eax; [0x8bfa34]=eax; je 0x404b4b */
    eax = (uint32_t)((int64_t)(int32_t)eax * (int64_t)(int32_t)edx);
    edx = RD(ebp + 0x90u);
    eax += ecx;
    eax = RD(edx + eax * 4u);
    WR(0x008bfa34u, eax);
    if (eax == 0u) goto L_00404b4b;
    /* 00404af2: cmp byte [eax],0; je 0x404b4b */
    if (RD8(eax) == 0u) goto L_00404b4b;
    /* 00404af7: ecx=[0x8be9b4]; edx=[0x8be9b0]; cmp ecx,edx; jle 0x404b2d */
    ecx = RD(0x008be9b4u);
    edx = RD(0x008be9b0u);
    if ((int32_t)ecx <= (int32_t)edx) goto L_00404b2d;
    /* 00404b07: edx=[eax+4]; push edx; push 0x8bea1c; push 0; call 0x403fa0;
     * add esp,0xc; test eax,eax; jne 0x404b6b */
    edx = RD(eax + 4u);
    PUSH(edx);
    PUSH(0x008bea1cu);
    PUSH(0u);
    ZCALL(wmain_00403fa0, 0x00404b17u);
    esp += 0xcu;
    if (eax != 0u) goto L_00404b6b;
    /* 00404b1e: eax=[0x8be9b4]; [0x8be9b0]=eax; eax=[0x8bfa34] */
    eax = RD(0x008be9b4u);
    WR(0x008be9b0u, eax);
    eax = RD(0x008bfa34u);
L_00404b2d:
    /* 00404b2d: push 0x8bea1c; push eax; call 0x4056c0; eax=[0x59debc];
     * add esp,8; cmp eax,0x2000; jae 0x404e49 */
    PUSH(0x008bea1cu);
    PUSH(eax);
    ZCALL(wmain_004056c0, 0x00404b38u);
    eax = RD(0x0059debcu);
    esp += 8u;
    if (eax >= 0x2000u) goto L_00404e49;
L_00404b4b:
    /* 00404b4b: eax=[0x8be9b4]; ecx=[0x8be974]; inc eax; inc ecx; cmp ecx,edi;
     * [0x8be9b4]=eax; [0x8be974]=ecx; jle 0x404a78 */
    eax = RD(0x008be9b4u);
    ecx = RD(0x008be974u);
    eax++;
    ecx++;
    WR(0x008be9b4u, eax);
    WR(0x008be974u, ecx);
    if ((int32_t)ecx <= (int32_t)edi) goto L_00404a78;
L_00404b6b:
    /* 00404b6b: mov edx, dword ptr [0x8be99c] */
    edx = RD(0x008be99cu);
L_00404b71:
    /* 00404b71: the second (depth) pass over the same rows */
    ecx = RD(0x008bea28u);
    eax = RD(0x008bea2cu);
    esi = RD(0x008be98cu);
    edi = RD(0x008be990u);
    WR(0x008bea1cu, ecx);
    ecx = RD(0x008bea30u);
    WR(0x008bea20u, eax);
    eax = RD(esp + 0x10u);
    WR(0x008bea24u, ecx);
    ebp = RD(edx * 8u + 0x008bea34u);
    ebx = RD(edx * 8u + 0x008bea38u);
    ecx = edx - 1u;
    ebp += esi;
    ebx += edi;
    WR(0x008be9bcu, 0u);
    WR(0x008be974u, ecx);
    if ((int32_t)ecx < (int32_t)eax) goto L_00404ce4;
    goto L_00404bde;
L_00404bd2:
    /* 00404bd2: edi=[0x8be990]; esi=[0x8be98c] */
    edi = RD(0x008be990u);
    esi = RD(0x008be98cu);
L_00404bde:
    /* 00404bde: eax=[ecx*8+0x8bea34]; eax+=esi; [0x8be978]=eax;
     * ecx=[ecx*8+0x8bea38]; ecx+=edi; eax-=ebp; [0x8be97c]=ecx; ecx-=ebx;
     * push ecx; push eax; push 0x8bea1c; call 0x403ef0 */
    eax = RD(ecx * 8u + 0x008bea34u);
    eax += esi;
    WR(0x008be978u, eax);
    ecx = RD(ecx * 8u + 0x008bea38u);
    ecx += edi;
    eax -= ebp;
    WR(0x008be97cu, ecx);
    ecx -= ebx;
    PUSH(ecx);
    PUSH(eax);
    PUSH(0x008bea1cu);
    ZCALL(wmain_00403ef0, 0x00404c0bu);
    /* 00404c0b: ecx=[0x8be978]; eax=[0x8be97c]; add esp,0xc; ebp=ecx; test ecx,ecx;
     * ebx=eax; jl 0x404cac */
    ecx = RD(0x008be978u);
    eax = RD(0x008be97cu);
    esp += 0xcu;
    ebp = ecx;
    ebx = eax;
    if ((int32_t)ecx < 0) goto L_00404cac;
    /* 00404c25: esi=[esp+0xa4]; edx=[esi+0x70]; cmp ecx,edx; jge 0x404cac */
    esi = RD(esp + 0xa4u);
    edx = RD(esi + 0x70u);
    if ((int32_t)ecx >= (int32_t)edx) goto L_00404cac;
    /* 00404c33: test eax,eax; jl 0x404cac */
    if ((int32_t)eax < 0) goto L_00404cac;
    /* 00404c37: cmp eax,[esi+0x74]; jge 0x404cac */
    if ((int32_t)eax >= (int32_t)RD(esi + 0x74u)) goto L_00404cac;
    /* 00404c3c: imul eax,edx; edx=[esi+0x90]; eax+=ecx; eax=[edx+eax*4];
     * [0x8bfa34]=eax; je 0x404cac */
    eax = (uint32_t)((int64_t)(int32_t)eax * (int64_t)(int32_t)edx);
    edx = RD(esi + 0x90u);
    eax += ecx;
    eax = RD(edx + eax * 4u);
    WR(0x008bfa34u, eax);
    if (eax == 0u) goto L_00404cac;
    /* 00404c53: cmp byte [eax],0; je 0x404cac */
    if (RD8(eax) == 0u) goto L_00404cac;
    /* 00404c58: ecx=[0x8be9bc]; edx=[0x8be9b8]; cmp ecx,edx; jle 0x404c8e */
    ecx = RD(0x008be9bcu);
    edx = RD(0x008be9b8u);
    if ((int32_t)ecx <= (int32_t)edx) goto L_00404c8e;
    /* 00404c68: edx=[eax+4]; push edx; push 0x8bea1c; push 1; call 0x403fa0;
     * add esp,0xc; test eax,eax; jne 0x404cd2 */
    edx = RD(eax + 4u);
    PUSH(edx);
    PUSH(0x008bea1cu);
    PUSH(1u);
    ZCALL(wmain_00403fa0, 0x00404c78u);
    esp += 0xcu;
    if (eax != 0u) goto L_00404cd2;
    /* 00404c7f: eax=[0x8be9bc]; [0x8be9b8]=eax; eax=[0x8bfa34] */
    eax = RD(0x008be9bcu);
    WR(0x008be9b8u, eax);
    eax = RD(0x008bfa34u);
L_00404c8e:
    /* 00404c8e: push 0x8bea1c; push eax; call 0x4056c0; eax=[0x59debc];
     * add esp,8; cmp eax,0x2000; jae 0x404e49 */
    PUSH(0x008bea1cu);
    PUSH(eax);
    ZCALL(wmain_004056c0, 0x00404c99u);
    eax = RD(0x0059debcu);
    esp += 8u;
    if (eax >= 0x2000u) goto L_00404e49;
L_00404cac:
    /* 00404cac: esi=[0x8be9bc]; ecx=[0x8be974]; eax=[esp+0x10]; inc esi; dec ecx;
     * [0x8be9bc]=esi; cmp ecx,eax; [0x8be974]=ecx; jge 0x404bd2 */
    esi = RD(0x008be9bcu);
    ecx = RD(0x008be974u);
    eax = RD(esp + 0x10u);
    esi++;
    ecx--;
    WR(0x008be9bcu, esi);
    WR(0x008be974u, ecx);
    if ((int32_t)ecx >= (int32_t)eax) goto L_00404bd2;
L_00404cd2:
    /* 00404cd2: edx=[0x8be99c]; edi=[0x8be990]; esi=[0x8be98c] */
    edx = RD(0x008be99cu);
    edi = RD(0x008be990u);
    esi = RD(0x008be98cu);
L_00404ce4:
    /* 00404ce4: ebp=[0x8be970]; ecx=[0x8be9c0]; ebx=0; eax=ebp+0x101;
     * cmp ecx,ebx; [0x8be988]=eax; jne 0x404d94 */
    ebp = RD(0x008be970u);
    ecx = RD(0x008be9c0u);
    ebx = 0;
    eax = ebp + 0x101u;
    WR(0x008be988u, eax);
    if (ecx != ebx) goto L_00404d94;
    /* 00404d05: edi=[eax*8+0x8bea34]; ecx=[0x8be998]; edi+=ecx; ecx=[0x8be994];
     * [0x8be990]=edi; ecx-=[eax*8+0x8bea38]; [0x8be978]=ecx;
     * eax=[edx*8+0x8bea34]; eax+=esi; cmp ecx,eax; je 0x404d89 */
    edi = RD(eax * 8u + 0x008bea34u);
    ecx = RD(0x008be998u);
    edi += ecx;
    ecx = RD(0x008be994u);
    WR(0x008be990u, edi);
    ecx -= RD(eax * 8u + 0x008bea38u);
    WR(0x008be978u, ecx);
    eax = RD(edx * 8u + 0x008bea34u);
    eax += esi;
    if (ecx == eax) goto L_00404d89;
    /* 00404d3a: cmp [0x8be9e4],ebx; jne 0x404d89 */
    if (RD(0x008be9e4u) != ebx) goto L_00404d89;
    /* 00404d42: fld [0x8be9c8]; fcomp [0x4a802c]; fld [0x8be9c4]; fnstsw ax;
     * fcomp [0x4a802c]; test ah,1; fnstsw ax; jne 0x404d6b */
    {
        uint32_t t1;
        FLD32(0x008be9c8u);
        FCOM_M(0x004a802cu);
        FPOP();
        FLD32(0x008be9c4u);
        FNSTSW_AX();
        t1 = (eax >> 8) & 1u;
        FCOM_M(0x004a802cu);
        FPOP();
        FNSTSW_AX();
        if (t1) goto L_00404d6b;
    }
    /* 00404d63: test ah,1; je 0x404d73 */
    if (((eax >> 8) & 1u) == 0u) goto L_00404d73;
    /* 00404d68: inc edx; jmp 0x404d74 */
    edx++;
    goto L_00404d74;
L_00404d6b:
    /* 00404d6b: test ah,1; jne 0x404d73 */
    if (((eax >> 8) & 1u) != 0u) goto L_00404d73;
    /* 00404d70: inc edx; jmp 0x404d74 */
    edx++;
    goto L_00404d74;
L_00404d73:
    /* 00404d73: dec edx */
    edx--;
L_00404d74:
    /* 00404d74: [0x8be99c]=edx; [0x8be9e4]=1; jmp 0x404e1c */
    WR(0x008be99cu, edx);
    WR(0x008be9e4u, 1u);
    goto L_00404e1c;
L_00404d89:
    /* 00404d89: [0x8be9e4]=ebx; jmp 0x404e1c */
    WR(0x008be9e4u, ebx);
    goto L_00404e1c;
L_00404d94:
    /* 00404d94: esi=[0x8be994]; ecx=[eax*8+0x8bea38]; esi-=ecx; ecx=[0x8be998];
     * [0x8be98c]=esi; eax=[eax*8+0x8bea34]; eax+=ecx; [0x8be97c]=eax;
     * ecx=[edx*8+0x8bea38]; ecx+=edi; cmp eax,ecx; je 0x404e16 */
    esi = RD(0x008be994u);
    ecx = RD(eax * 8u + 0x008bea38u);
    esi -= ecx;
    ecx = RD(0x008be998u);
    WR(0x008be98cu, esi);
    eax = RD(eax * 8u + 0x008bea34u);
    eax += ecx;
    WR(0x008be97cu, eax);
    ecx = RD(edx * 8u + 0x008bea38u);
    ecx += edi;
    if (eax == ecx) goto L_00404e16;
    /* 00404dca: cmp [0x8be9e0],ebx; jne 0x404e16 */
    if (RD(0x008be9e0u) != ebx) goto L_00404e16;
    /* 00404dd2: fld [0x8be9c4]; fcomp [0x4a802c]; fld [0x8be9c8]; fnstsw ax;
     * fcomp [0x4a802c]; test ah,1; fnstsw ax; jne 0x404dfb */
    {
        uint32_t t1;
        FLD32(0x008be9c4u);
        FCOM_M(0x004a802cu);
        FPOP();
        FLD32(0x008be9c8u);
        FNSTSW_AX();
        t1 = (eax >> 8) & 1u;
        FCOM_M(0x004a802cu);
        FPOP();
        FNSTSW_AX();
        if (t1) goto L_00404dfb;
    }
    /* 00404df3: test ah,1; je 0x404e03 */
    if (((eax >> 8) & 1u) == 0u) goto L_00404e03;
    /* 00404df8: dec edx; jmp 0x404e04 */
    edx--;
    goto L_00404e04;
L_00404dfb:
    /* 00404dfb: test ah,1; jne 0x404e03 */
    if (((eax >> 8) & 1u) != 0u) goto L_00404e03;
    /* 00404e00: dec edx; jmp 0x404e04 */
    edx--;
    goto L_00404e04;
L_00404e03:
    /* 00404e03: inc edx */
    edx++;
L_00404e04:
    /* 00404e04: [0x8be99c]=edx; [0x8be9e0]=1; jmp 0x404e1c */
    WR(0x008be99cu, edx);
    WR(0x008be9e0u, 1u);
    goto L_00404e1c;
L_00404e16:
    /* 00404e16: [0x8be9e0]=ebx */
    WR(0x008be9e0u, ebx);
L_00404e1c:
    /* 00404e1c: ecx=[0x8be9ac]; eax=[0x59debc]; inc ecx; cmp eax,0x2000;
     * [0x8be9ac]=ecx; jae 0x404e49 */
    ecx = RD(0x008be9acu);
    eax = RD(0x0059debcu);
    ecx++;
    WR(0x008be9acu, ecx);
    if (eax >= 0x2000u) goto L_00404e49;
    /* 00404e35: eax=[0x8be9a0]; inc ebp; cmp ebp,eax; [0x8be970]=ebp;
     * jl 0x4049a8 */
    eax = RD(0x008be9a0u);
    ebp++;
    WR(0x008be970u, ebp);
    if ((int32_t)ebp < (int32_t)eax) goto L_004049a8;
L_00404e49:
    /* 00404e49: mov edx,[0x59df70]; and edx,0xff; mov [0x8be920],edx */
    edx = RD(0x0059df70u) & 0xffu;
    WR(0x008be920u, edx);
    /* 00404e5b: pop edi; pop esi; pop ebp; pop ebx; add esp,0x90; ret */
    goto L_004056a9;

/* ---------------------------------------------------------------------------------
 * 00404e66: the path taken while [0x8822a8] == 0 (no world): camera/screen setup,
 * the clip rectangle from the projection at 0x6f83e4, and the cell-grid walk.
 * ------------------------------------------------------------------------------- */
L_00404e66:
    /* 00404e66: eax=[0x89fe24]; ecx=[0x89fe20]; neg eax; sbb al,al;
     * [esp+0x70]=ebx; and al,4; [esp+0x6c]=ebx; add al,4; cmp ecx,ebx;
     * [esp+0x44]=ebx; [esp+0x40]=ebx; [esp+0x60]=ebx; [esp+0x68]=ebx; esi=1;
     * je 0x404e9e */
    eax = RD(0x0089fe24u);
    ecx = RD(0x0089fe20u);
    {   /* neg eax; sbb al,al -> al = 0xff if [0x89fe24] != 0 */
        uint32_t cf = (eax != 0u);
        eax = (uint32_t)(0u - eax);
        eax = (eax & 0xffffff00u) | (cf ? 0xffu : 0u);
    }
    WR(esp + 0x70u, ebx);
    eax = (eax & 0xffffff00u) | ((eax & 0xffu) & 4u);
    WR(esp + 0x6cu, ebx);
    eax = (eax & 0xffffff00u) | (((eax & 0xffu) + 4u) & 0xffu);
    WR(esp + 0x44u, ebx);
    WR(esp + 0x40u, ebx);
    WR(esp + 0x60u, ebx);
    WR(esp + 0x68u, ebx);
    esi = 1;
    if (ecx == 0u) goto L_00404e9e;
    eax = (eax & 0xffffff00u) | ((eax & 0xffu) | 2u);
    goto L_00404ea0;
L_00404e9e:
    eax = (eax & 0xffffff00u) | ((eax & 0xffu) | 1u);
L_00404ea0:
    /* 00404ea0: fld [0x868718]; fsub [0x59dec0]; [0x59df54]=al; eax=[0x868718];
     * [0x59dec0]=eax; eax=[0x6f83e4]; cmp eax,ebx; fstp [esp+0x14]; je 0x4056a9 */
    FLD32(0x00868718u);
    XS(0) -= RDF(0x0059dec0u);
    WR8(0x0059df54u, eax & 0xffu);
    eax = RD(0x00868718u);
    WR(0x0059dec0u, eax);
    eax = RD(0x006f83e4u);
    FSTP32(esp + 0x14u);
    if (eax == 0u) goto L_004056a9;
    /* 00404ecc/ed8/ee6: the same four gates as the world path */
    if (RD(ebp + 0x90u) == 0u) goto L_004056a9;
    ecx = ebp + 0xc4u;
    if (ecx == 0u) goto L_004056a9;
    if (RD(ebp + 0xb8u) == 0u) goto L_004056a9;
    /* 00404ef2: fld [eax+0x38]; fsub [0x4a8064]; eax=[0x881344]; [0x59df70]=0;
     * cmp eax,ebx; fst [esp+0x10]; fmul [0x4a8068]; fmul [0x4a806c];
     * fstp [esp+0x20]; jne 0x404fe0 */
    FLD32(eax + 0x38u);
    XS(0) -= RDF(0x004a8064u);
    eax = RD(0x00881344u);
    WR8(0x0059df70u, 0u);
    FST32(esp + 0x10u);
    XS(0) *= RDF(0x004a8068u);
    XS(0) *= RDF(0x004a806cu);
    FSTP32(esp + 0x20u);
    if (eax != 0u) goto L_00404fe0;

    /* 00404f23: push 0x59df70; push ebp; call 0x40e42a; ecx=eax; add esp,8 */
    PUSH(0x0059df70u);
    PUSH(ebp);
    ZCALL(wmain_0040e42a, 0x00404f28u);
    ecx = eax;
    esp += 8u;
    WR(esp + 0x1cu, ecx);
    /* 00404f33: clamp the cell count to [2, 0x40] */
    if ((int32_t)ecx >= 2) {
        if ((int32_t)ecx > 0x40) ecx = 0x40u;
    } else {
        ecx = 2u;
    }
    WR(esp + 0x1cu, ecx);
    /* 00404f51: al=[0x59df48]; dl=[0x59df70]; bl=al; bl^=dl; je 0x404f75 */
    eax = (eax & 0xffffff00u) | (RD8(0x0059df48u) & 0xffu);
    edx = (edx & 0xffffff00u) | (RD8(0x0059df70u) & 0xffu);
    ebx = (ebx & 0xffffff00u) | (eax & 0xffu);
    ebx = (ebx & 0xffffff00u) | (((ebx & 0xffu) ^ (edx & 0xffu)) & 0xffu);
    if ((ebx & 0xffu) == 0u) goto L_00404f75;
    /* 00404f62: fld [0x4a8058]; [0x59df90]=dl; [0x59df80]=al; jmp 0x404f7b */
    FLD32(0x004a8058u);
    WR8(0x0059df90u, edx & 0xffu);
    WR8(0x0059df80u, eax & 0xffu);
    goto L_00404f7b;
L_00404f75:
    /* 00404f75: fld [0x59deac] */
    FLD32(0x0059deacu);
L_00404f7b:
    /* 00404f7b: fld [esp+0x14]; fadd st(0),st(0); [0x59df48]=dl;
     * fsubr st(1)  (D8 E9 = FSUBR ST(0),ST(1): st0 = st1 - st0);
     * fstp [0x59deac]; fstp st(0) */
    FLD32(esp + 0x14u);
    XS(0) += XS(0);
    WR8(0x0059df48u, edx & 0xffu);
    XS(0) = XS(1) - XS(0);
    FSTP32(0x0059deacu);
    FPOP();
    /* 00404f91: fld [0x59deac]; fcomp [0x4a802c]; fnstsw ax; test ah,0x41;
     * jne 0x404fc4 */
    FLD32(0x0059deacu);
    FCOM_M(0x004a802cu);
    FPOP();
    FNSTSW_AX();
    if (((eax >> 8) & 0x41u) != 0u) goto L_00404fc4;
    /* 00404fa4: cmp [0x4ac538],esi; je 0x404fc4 */
    if (RD(0x004ac538u) == esi) goto L_00404fc4;
    /* 00404fac: eax=[0x89fdf0]; test eax,eax; je 0x404fc4 */
    eax = RD(0x0089fdf0u);
    if (eax == 0u) goto L_00404fc4;
    /* 00404fb5: [0x59df44]=0; [0x59df60]=esi; jmp 0x404fe9 */
    WR8(0x0059df44u, 0u);
    WR(0x0059df60u, esi);
    goto L_00404fe9;
L_00404fc4:
    /* 00404fc4: [0x59deac]=0; [0x59df60]=0; [0x59df44]=dl; jmp 0x404fe9 */
    WR(0x0059deacu, 0u);
    WR(0x0059df60u, 0u);
    WR8(0x0059df44u, edx & 0xffu);
    goto L_00404fe9;
L_00404fe0:
    /* 00404fe0: mov ecx,0x40; [esp+0x1c]=ecx */
    ecx = 0x40u;
    WR(esp + 0x1cu, ecx);
L_00404fe9:
    /* 00404fe9: edx=ecx; imul edx,ecx; ecx=[0x8a011c]; [esp+0x14]=edx;
     * fild [esp+0x14]; eax=ecx+0x24; ecx+=0x34; edx=eax; push ecx;
     * esi=[edx]; ecx=esp+0x58; fstp [esp+0x88]; fild [esp+0x20];
     * [esp+0x78]=esi; esi=[edx+4]; [esp+0x7c]=esi; push ecx;
     * edx=[edx+8]; [esp+0x84]=edx; edx=[eax]; [esp+0x38]=edx;
     * edx=[eax+4]; eax=[eax+8]; [esp+0x3c]=edx; fstp [esp+0x24];
     * [esp+0x40]=eax; [esp+0x5c]=0; [esp+0x60]=0x3f800000; [esp+0x64]=0;
     * call 0x486828 (S+0x54, world+0x34) */
    edx = ecx * ecx;
    ecx = RD(0x008a011cu);
    WR(esp + 0x14u, edx);
    FILD32(esp + 0x14u);
    eax = ecx + 0x24u;
    ecx += 0x34u;
    edx = eax;
    PUSH(ecx);
    esi = RD(edx);
    ecx = esp + 0x58u;
    FSTP32(esp + 0x88u);                    /* S+0x84 = (float)(count*count) */
    FILD32(esp + 0x20u);                    /* S+0x1c = count */
    WR(esp + 0x78u, esi);                   /* S+0x74 = [world+0x24] */
    esi = RD(edx + 4u);
    WR(esp + 0x7cu, esi);                   /* S+0x78 = [world+0x28] */
    PUSH(ecx);                              /* arg0 = S+0x54 */
    edx = RD(edx + 8u);
    WR(esp + 0x84u, edx);                   /* S+0x7c = [world+0x2c] */
    edx = RD(eax);
    WR(esp + 0x38u, edx);                   /* S+0x30 = [world+0x24] */
    edx = RD(eax + 4u);
    eax = RD(eax + 8u);
    WR(esp + 0x3cu, edx);                   /* S+0x34 = [world+0x28] */
    FSTP32(esp + 0x24u);                    /* S+0x1c = (float)count */
    WR(esp + 0x40u, eax);                   /* S+0x38 = [world+0x2c] */
    WR(esp + 0x5cu, 0u);                    /* S+0x54 = 0.0f */
    WR(esp + 0x60u, 0x3f800000u);           /* S+0x58 = 1.0f */
    WR(esp + 0x64u, 0u);                    /* S+0x5c = 0.0f */
    ZCALL(wmain_00486828, 0x00405061u);

    /* 00405061: fld [esp+0x5c]  (esp = S-8 -> S+0x54); fmul [0x4a8048];
     * the ctx publishes; then add esp,8 at 0x405092 */
    FLD32(esp + 0x5cu);
    XS(0) *= RDF(0x004a8048u);
    WR(0x0059df58u, ebp);
    WR(0x0059df88u, RD(ebp + 0xbcu));
    WR(0x0059df38u, RD(ebp + 0x50u));
    WR(0x0059debcu, 0u);
    ecx = RD(ebp + 0xcu);
    esp += 8u;
    ecx++;
    eax = 0;
    /* 00405098: fadd [esp+0x30]; rep stosd at 0x8bc920 (edx = ecx before shr) */
    XS(0) += RDF(esp + 0x30u);
    edx = ecx;
    edi = 0x008bc920u;
    { uint32_t n = ecx >> 2; while (n) { WR(edi, 0u); edi += 4 * df; n--; } }
    /* 004050a8: fstp [esp+0x30]; fld [esp+0x58]; fmul [0x4a8048]; ... */
    FSTP32(esp + 0x30u);
    FLD32(esp + 0x58u);
    XS(0) *= RDF(0x004a8048u);
    ecx = edx;
    esp -= 8u;                              /* 004050b8: sub esp,8 */
    ecx &= 3u;
    { uint32_t n = ecx; while (n) { WR8(edi, 0u); edi += df; n--; } }   /* 004050be */
    ecx = 0x21u;
    eax = 0;
    edi = 0x008bc880u;
    XS(0) += RDF(esp + 0x3cu);              /* 004050cc: [S+0x34] */
    { uint32_t n = ecx; while (n) { WR(edi, 0u); edi += 4 * df; n--; } } /* 004050d0 */
    FSTP32(esp + 0x3cu);                    /* 004050d2 */
    FLD32(esp + 0x7cu);                     /* 004050d6: [S+0x74] */
    XS(0) -= RDF(0x004a8054u);
    ecx = 0x3fu;
    edi = 0x008bfaa0u;
    { uint32_t n = ecx; while (n) { WR(edi, 0u); edi += 4 * df; n--; } } /* 004050ea */
    /* 004050ec: fstp qword [esp]  (esp = S-8) + stosw + stosb */
    { uint64_t u = gmem_u64_d(XS(0));
      WR(esp, (uint32_t)u); WR(esp + 4u, (uint32_t)(u >> 32)); FPOP(); }
    WR(edi, 0u); edi += 2 * df;             /* 004050ef: stosw */
    WR8(edi, 0u);                           /* 004050f1: stosb */
    ZCALL(wmain_0049a480, 0x004050f7u);     /* frnd (double at S-8) */
    esp += 8u;

    /* 004050fa: __ftol -> eax; 0x4050ff fld [esp+0x78] ... */
    ZCALL(wmain_0049a44c, 0x004050fau);
    FLD32(esp + 0x78u);
    XS(0) -= RDF(0x004a8054u);
    esp -= 8u;
    esi = eax;                              /* 0040510c */
    { uint64_t u = gmem_u64_d(XS(0));
      WR(esp, (uint32_t)u); WR(esp + 4u, (uint32_t)(u >> 32)); FPOP(); }
    ZCALL(wmain_0049a480, 0x00405116u);
    esp += 8u;
    ZCALL(wmain_0049a44c, 0x0040511eu);
    FLD32(esp + 0x30u);
    XS(0) -= RDF(0x004a8054u);
    esp -= 8u;
    ebx = eax;                              /* 0040512b */
    { uint64_t u = gmem_u64_d(XS(0));
      WR(esp, (uint32_t)u); WR(esp + 4u, (uint32_t)(u >> 32)); FPOP(); }
    ZCALL(wmain_0049a480, 0x00405135u);
    esp += 8u;
    ZCALL(wmain_0049a44c, 0x0040513du);
    FLD32(esp + 0x34u);
    XS(0) -= RDF(0x004a8054u);
    esp -= 8u;
    edi = eax;                              /* 0040514a */
    { uint64_t u = gmem_u64_d(XS(0));
      WR(esp, (uint32_t)u); WR(esp + 4u, (uint32_t)(u >> 32)); FPOP(); }
    ZCALL(wmain_0049a480, 0x00405154u);
    esp += 8u;
    ZCALL(wmain_0049a44c, 0x0040515cu);

    /* 0040515c: max/min of the four rounded screen edges */
    WR(esp + 0x28u, esi);
    if ((int32_t)esi <= (int32_t)edi) WR(esp + 0x28u, edi);
    WR(esp + 0x24u, esi);
    if ((int32_t)esi >= (int32_t)edi) WR(esp + 0x24u, edi);
    WR(esp + 0x2cu, ebx);
    if ((int32_t)ebx <= (int32_t)eax) WR(esp + 0x2cu, eax);
    ebp = ebx;
    if ((int32_t)ebx >= (int32_t)eax) ebp = eax;

    /* 00405188: eax=[0x8a011c]; [esp+0x64]=1;
     * fld [0x4a8044]; fsub [eax+0x34]; fld [0x4a8044]; fsub [eax+0x38];
     * fxch st(1); fst [esp+0x3c]; fxch; fxch; fcomp [0x4a8074];
     * fstp [esp+0x14]; fnstsw ax; test ah,0x41; je 0x4051ca */
    eax = RD(0x008a011cu);
    WR(esp + 0x64u, 1u);
    FLD32(0x004a8044u);
    XS(0) -= RDF(eax + 0x34u);
    FLD32(0x004a8044u);
    XS(0) -= RDF(eax + 0x38u);
    FXCH(1);
    FST32(esp + 0x3cu);
    FXCH(1);
    FXCH(1);
    FCOM_M(0x004a8074u);
    FPOP();
    FSTP32(esp + 0x14u);
    FNSTSW_AX();
    if (((eax >> 8) & 0x41u) == 0u) goto L_004051ca;
    WR(esp + 0x64u, 0u);
L_004051ca:
    /* 004051ca: fld [esp+0x10]; fmul [0x4a8038]; fstp [esp+0x10];
     * eax=[esp+0x10]; push eax; call 0x4942bf; fst [0x59df7c];
     * fmul [esp+0x20]; add esp,4; fstp [esp+0x80] */
    FLD32(esp + 0x10u);
    XS(0) *= RDF(0x004a8038u);
    FSTP32(esp + 0x10u);
    eax = RD(esp + 0x10u);
    PUSH(eax);
    ZCALL(wmain_004942bf, 0x004051e2u);
    FST32(0x0059df7cu);
    XS(0) *= RDF(esp + 0x20u);              /* esp = S-4 -> S+0x1c */
    esp += 4u;
    FSTP32(esp + 0x80u);
    /* 004051f6: fld [esp+0x20]; fmul [0x4a8038]; fstp [esp+0x18];
     * ecx=[esp+0x18]; push ecx; call 0x4942bf; fst [0x59dec4];
     * fmul [esp+0x20]; add esp,4; push ecx; fstp [esp+0x24] */
    FLD32(esp + 0x20u);
    XS(0) *= RDF(0x004a8038u);
    FSTP32(esp + 0x18u);
    ecx = RD(esp + 0x18u);
    PUSH(ecx);
    ZCALL(wmain_004942bf, 0x0040520eu);
    FST32(0x0059dec4u);
    XS(0) *= RDF(esp + 0x20u);              /* esp = S-4 -> S+0x1c */
    esp += 4u;
    PUSH(ecx);                              /* 0040521b (placeholder for the arg) */
    FSTP32(esp + 0x24u);                    /* S+0x20 */
    /* 00405220: fld [0x4a804c]; fsub [esp+0x14] (S+0x10); fsubr [0x4a804c];
     * fstp [esp]; call 0x49428d; fdivr [0x4a8078]; add esp,4;
     * [0x59df3c] = ... */
    FLD32(0x004a804cu);
    XS(0) -= RDF(esp + 0x14u);
    XS(0) = RDF(0x004a804cu) - XS(0);
    FSTP32(esp);
    ZCALL(wmain_0049428d, 0x00405238u);
    XS(0) = RDF(0x004a8078u) / XS(0);
    esp += 4u;
    PUSH(ecx);                              /* 00405241 */
    FSTP32(0x0059df3cu);
    /* 00405248: the same for [esp+0x1c] (S+0x18) -> [0x59dea8] */
    FLD32(0x004a804cu);
    XS(0) -= RDF(esp + 0x1cu);
    XS(0) = RDF(0x004a804cu) - XS(0);
    FSTP32(esp);
    ZCALL(wmain_0049428d, 0x00405260u);
    XS(0) = RDF(0x004a8078u) / XS(0);
    esp += 4u;
    FSTP32(0x0059dea8u);

    /* 0040526f: call 0x41f700; lea edx,[esp+0x74]; push edx; call 0x41f80b */
    ZCALL(wmain_0041f700, 0x00405274u);
    edx = esp + 0x74u;
    PUSH(edx);
    ZCALL(wmain_0041f80b, 0x0040527eu);
    eax = RD(esp + 0x18u);                  /* 0040527e: esp = S-4 -> S+0x14 */
    esp += 4u;
    /* 00405285: push eax; push 0xbf800000; push 0; push 0; call 0x41f7df */
    PUSH(eax);
    PUSH(0xbf800000u);
    PUSH(0u);
    PUSH(0u);
    ZCALL(wmain_0041f7df, 0x00405294u);
    ecx = RD(esp + 0x4cu);                  /* 00405294: esp = S-16 -> S+0x3c */
    esp += 0x10u;
    /* 0040529b: push ecx; push 0; push 0; push 0xbf800000; call 0x41f7df */
    PUSH(ecx);
    PUSH(0u);
    PUSH(0u);
    PUSH(0xbf800000u);
    ZCALL(wmain_0041f7df, 0x004052aau);
    esp += 0x10u;
    /* 004052ad: call 0x41f71b; edx=[esp+0x1c]; push 0; push edx; push 0;
     * call 0x41f837; add esp,0xc */
    ZCALL(wmain_0041f71b, 0x004052b2u);
    edx = RD(esp + 0x1cu);
    PUSH(0u);
    PUSH(edx);
    PUSH(0u);
    ZCALL(wmain_0041f837, 0x004052c0u);
    esp += 0xcu;

    /* 004052c3: the projected clip rectangle sampled on a 2 px grid */
    WR(esp + 0x10u, 0xffffffffu);
L_004052cb:
    /* 004052cb: fild [esp+0x10]; or edi,-1; [esp+0x18]=edi; fmul [esp+0x80];
     * fstp [esp+0x14]; ebx=[esp+0x14] */
    FILD32(esp + 0x10u);
    edi = 0xffffffffu;
    WR(esp + 0x18u, edi);
    XS(0) *= RDF(esp + 0x80u);
    FSTP32(esp + 0x14u);
    ebx = RD(esp + 0x14u);
L_004052e5:
    /* 004052e5: call 0x41f71b; fild [esp+0x18]; push ecx; fmul [esp+0x24];
     * fstp [esp]; push 0; push ebx; call 0x41f837 (0, ebx, st0) */
    ZCALL(wmain_0041f71b, 0x004052eau);
    FILD32(esp + 0x18u);
    PUSH(ecx);
    XS(0) *= RDF(esp + 0x24u);              /* esp = S-4 -> S+0x20 */
    FSTP32(esp);
    PUSH(0u);
    PUSH(ebx);
    ZCALL(wmain_0041f837, 0x004052feu);
    esp += 0xcu;
    /* 00405301: lea eax,[esp+0x88]; push eax; call 0x41fa37; add esp,4;
     * call 0x41f770 */
    eax = esp + 0x88u;
    PUSH(eax);
    ZCALL(wmain_0041fa37, 0x0040530eu);
    esp += 4u;
    ZCALL(wmain_0041f770, 0x00405316u);
    /* 00405316: fld [esp+0x88]; fsub [0x4a8054]; sub esp,8; fstp qword [esp];
     * frnd; add esp,8; __ftol */
    FLD32(esp + 0x88u);
    XS(0) -= RDF(0x004a8054u);
    esp -= 8u;
    { uint64_t u = gmem_u64_d(XS(0));
      WR(esp, (uint32_t)u); WR(esp + 4u, (uint32_t)(u >> 32)); FPOP(); }
    ZCALL(wmain_0049a480, 0x00405329u);
    esp += 8u;
    ZCALL(wmain_0049a44c, 0x00405331u);
    /* 00405336: fld [esp+0x8c]; fsub; sub esp,8; esi=eax; fstp qword [esp];
     * frnd; add esp,8; __ftol */
    FLD32(esp + 0x8cu);
    XS(0) -= RDF(0x004a8054u);
    esp -= 8u;
    esi = eax;
    { uint64_t u = gmem_u64_d(XS(0));
      WR(esp, (uint32_t)u); WR(esp + 4u, (uint32_t)(u >> 32)); FPOP(); }
    ZCALL(wmain_0049a480, 0x0040534bu);
    esp += 8u;
    ZCALL(wmain_0049a44c, 0x00405358u);
    /* 00405358: grow/shrink the clip rectangle */
    if ((int32_t)RD(esp + 0x28u) < (int32_t)esi) WR(esp + 0x28u, esi);
    if ((int32_t)RD(esp + 0x24u) > (int32_t)esi) WR(esp + 0x24u, esi);
    if ((int32_t)RD(esp + 0x2cu) < (int32_t)eax) WR(esp + 0x2cu, eax);
    if ((int32_t)ebp > (int32_t)eax) ebp = eax;
    /* 0040537c: add edi,2; cmp edi,1; [esp+0x18]=edi; jle 0x4052e5 */
    edi += 2u;
    WR(esp + 0x18u, edi);
    if ((int32_t)edi <= 1) goto L_004052e5;
    /* 0040538c: eax=[esp+0x10]; add eax,2; cmp eax,1; [esp+0x10]=eax; jle 0x4052cb */
    eax = RD(esp + 0x10u);
    eax += 2u;
    WR(esp + 0x10u, eax);
    if ((int32_t)eax <= 1) goto L_004052cb;

    /* 004053a0: call 0x41f770; clamp the rectangle to the grid */
    ZCALL(wmain_0041f770, 0x004053a5u);
    ecx = RD(esp + 0xa4u);
    edx = RD(esp + 0x28u);
    eax = RD(ecx + 0x70u);
    if ((int32_t)edx >= (int32_t)eax) { eax--; WR(esp + 0x28u, eax); }
    eax = RD(esp + 0x24u);
    if ((int32_t)eax < 0) WR(esp + 0x24u, 0u);
    eax = RD(ecx + 0x74u);
    ecx = RD(esp + 0x2cu);
    if ((int32_t)ecx >= (int32_t)eax) { eax--; WR(esp + 0x2cu, eax); }
    if ((int32_t)ebp < 0) ebp = 0;
    /* 004053e2: ecx=[0x6f83e4]; eax=[esp+0x2c]; fld [esp+0x1c];
     * fsub [0x4a8054]; esi=ecx+8; ecx=0xc; edi=0x59dec8; ebx=ebp;
     * rep movsd; fstp [0x59df94]; cmp ebp,eax; [esp+0x14]=ebx; jg 0x40567f */
    ecx = RD(0x006f83e4u);
    eax = RD(esp + 0x2cu);
    FLD32(esp + 0x1cu);
    XS(0) -= RDF(0x004a8054u);
    esi = ecx + 8u;
    ecx = 0xcu;
    edi = 0x0059dec8u;
    ebx = ebp;
    while (ecx) { WR(edi, RD(esi)); esi += 4 * df; edi += 4 * df; ecx--; }
    FSTP32(0x0059df94u);
    WR(esp + 0x14u, ebx);
    if ((int32_t)ebp > (int32_t)eax) goto L_0040567f;
    /* 00405419: edi=[esp+0x24] */
    edi = RD(esp + 0x24u);
L_0040541d:
    /* 0040541d: eax=[esp+0x28]; [esp+0x20]=edi; cmp edi,eax; jg 0x40566e */
    eax = RD(esp + 0x28u);
    WR(esp + 0x20u, edi);
    if ((int32_t)edi > (int32_t)eax) goto L_0040566e;
L_0040542d:
    /* 0040542d: eax=[esp+0xa4]; edx=ebx; imul edx,[eax+0x70]; eax=[eax+0x90];
     * edx+=edi; esi=[eax+edx*4]; test esi,esi; je 0x405659 */
    eax = RD(esp + 0xa4u);
    edx = (uint32_t)((int64_t)(int32_t)ebx * (int64_t)(int32_t)RD(eax + 0x70u));
    eax = RD(eax + 0x90u);
    edx += edi;
    esi = RD(eax + edx * 4u);
    if (esi == 0u) goto L_00405659;
    /* 0040544d: cmp byte [esi],0; jbe 0x405659 */
    if (RD8(esi) <= 0u) goto L_00405659;
    /* 00405456: the squared distance test: (row-k)^2 + (col-k)^2 < (count-0.5)^2
     * fild [esp+0x14]; fild [esp+0x20]; fsub k; fxch; fsub k; fxch;
     * fst [esp+0x10]; fxch; fstp [esp+0x18]; ... */
    FILD32(esp + 0x14u);
    FILD32(esp + 0x20u);
    XS(0) -= RDF(0x004a8054u);
    FXCH(1);
    XS(0) -= RDF(0x004a8054u);
    FXCH(1);
    FST32(esp + 0x10u);
    FXCH(1);
    FSTP32(esp + 0x18u);
    ecx = RD(esp + 0x18u);
    WR(esp + 0x98u, ecx);
    /* 00405483: fld [esp+0x98]; fxch; fsub [esp+0x74]; fxch; fsub [esp+0x78];
     * fxch; fst [esp+0x3c]; fxch; fst [esp+0x20]; fxch; fmul [esp+0x3c];
     * fxch; fmul [esp+0x20]; faddp st(1); fcomp [esp+0x84] */
    FLD32(esp + 0x98u);
    FXCH(1);
    XS(0) -= RDF(esp + 0x74u);
    FXCH(1);
    XS(0) -= RDF(esp + 0x78u);
    FXCH(1);
    FST32(esp + 0x3cu);
    FXCH(1);
    FST32(esp + 0x20u);
    FXCH(1);
    XS(0) *= RDF(esp + 0x3cu);
    FXCH(1);
    XS(0) *= RDF(esp + 0x20u);
    XS(1) += XS(0);
    FPOP();
    FCOM_M(esp + 0x84u);
    FPOP();
    FNSTSW_AX();
    if (((eax >> 8) & 1u) == 0u) goto L_00405659;
    /* 004054c2: push esi; call 0x405cf0 */
    PUSH(esi);
    ZCALL(wmain_00405cf0, 0x004054c8u);
    edx = RD(esp + 0x14u);                  /* 004054c8: esp = S-4 -> S+0x10 */
    eax = RD(esp + 0x1cu);                  /* S+0x18 */
    esp += 4u;
    WR(esp + 0x30u, edx);
    WR(esp + 0x34u, eax);
    ecx = RD(esi + 8u);
    edx = esp + 0x30u;
    PUSH(0x0059dec8u);
    PUSH(edx);
    WR(esp + 0x40u, ecx);                   /* esp = S-8 -> S+0x38 */
    ZCALL(wmain_0047e40a, 0x004054f1u);
    /* 004054f1: fld [esp+0x24] (S+0x1c = count); fsub [0x4a807c]; add esp,8;
     * fcomp [esp+0x34]; fnstsw ax; test ah,1; je 0x405512 */
    FLD32(esp + 0x24u);
    XS(0) -= RDF(0x004a807cu);
    esp += 8u;
    FCOM_M(esp + 0x34u);
    FPOP();
    FNSTSW_AX();
    if (((eax >> 8) & 1u) == 0u) goto L_00405512;
    /* 00405509: inc dword [esp+0x68]; jmp 0x405659 */
    WR(esp + 0x68u, RD(esp + 0x68u) + 1u);
    goto L_00405659;
L_00405512:
    /* 00405512: ebp=[esp+0x64]; test ebp,ebp; jne 0x405546 */
    ebp = RD(esp + 0x64u);
    if (ebp != 0u) goto L_00405546;
    /* 0040551a: fld [esp+0x38]; fld [esp+0x34]; fmul [0x59dec4]; fxch st(1);
     * fadd [0x59dea8]; fxch st(1); fchs; fcompp; fnstsw ax; test ah,0x41;
     * jne 0x405546 */
    FLD32(esp + 0x38u);
    FLD32(esp + 0x34u);
    XS(0) *= RDF(0x0059dec4u);
    FXCH(1);
    XS(0) += RDF(0x0059dea8u);
    FXCH(1);
    XS(0) = -XS(0);
    FCOM(cg, XS(0), XS(1)); FPOP(); FPOP();
    FNSTSW_AX();
    if (((eax >> 8) & 0x41u) != 0u) goto L_00405546;
    /* 0040553d: inc dword [esp+0x6c]; jmp 0x405659 */
    WR(esp + 0x6cu, RD(esp + 0x6cu) + 1u);
    goto L_00405659;
L_00405546:
    /* 00405546: eax=[esp+0x10]; ecx=[esp+0x18]; [esp+0x48]=eax; [esp+0x4c]=ecx;
     * edx=[esi+4]; eax=esp+0x48; push 0x59dec8; push eax; [esp+0x58]=edx;
     * call 0x47e40a */
    eax = RD(esp + 0x10u);
    ecx = RD(esp + 0x18u);
    WR(esp + 0x48u, eax);
    WR(esp + 0x4cu, ecx);
    edx = RD(esi + 4u);
    eax = esp + 0x48u;
    PUSH(0x0059dec8u);
    PUSH(eax);
    WR(esp + 0x58u, edx);                   /* esp = S-8 -> S+0x50 */
    ZCALL(wmain_0047e40a, 0x0040556cu);
    /* 0040556c: fld [esp+0x54] (S+0x4c); fsub [0x4a8078]; add esp,8;
     * fcomp [0x4a8048]; fnstsw ax; test ah,1; je 0x40558f */
    FLD32(esp + 0x54u);
    XS(0) -= RDF(0x004a8078u);
    esp += 8u;
    FCOM_M(0x004a8048u);
    FPOP();
    FNSTSW_AX();
    if (((eax >> 8) & 1u) == 0u) goto L_0040558f;
    /* 00405586: inc dword [esp+0x60]; jmp 0x405659 */
    WR(esp + 0x60u, RD(esp + 0x60u) + 1u);
    goto L_00405659;
L_0040558f:
    /* 0040558f: test ebp,ebp; jne 0x4055bb */
    if (ebp != 0u) goto L_004055bb;
    /* 00405593: fld [esp+0x4c]; fld [esp+0x50]; fsub [0x59dea8]; fxch st(1);
     * fmul [0x59dec4]; fcompp; fnstsw ax; test ah,1; je 0x4055bb */
    FLD32(esp + 0x4cu);
    FLD32(esp + 0x50u);
    XS(0) -= RDF(0x0059dea8u);
    FXCH(1);
    XS(0) *= RDF(0x0059dec4u);
    FCOM(cg, XS(0), XS(1)); FPOP(); FPOP();
    FNSTSW_AX();
    if (((eax >> 8) & 1u) == 0u) goto L_004055bb;
    /* 004055b2: inc dword [esp+0x70]; jmp 0x405659 */
    WR(esp + 0x70u, RD(esp + 0x70u) + 1u);
    goto L_00405659;
L_004055bb:
    /* 004055bb: fld [esp+0x34]; fmul [0x59df7c]; fld [esp+0x30];
     * fadd [0x59df3c]; fld st(1); fchs; fcompp; fnstsw ax; test ah,0x41;
     * jne 0x40560c */
    FLD32(esp + 0x34u);
    XS(0) *= RDF(0x0059df7cu);
    FLD32(esp + 0x30u);
    XS(0) += RDF(0x0059df3cu);
    FPUSH(XS(1));
    XS(0) = -XS(0);
    FCOM(cg, XS(0), XS(1)); FPOP(); FPOP();
    FNSTSW_AX();
    if (((eax >> 8) & 0x41u) != 0u) goto L_0040560c;
    /* 004055dc: fld [esp+0x48]; fld [esp+0x4c]; fmul [0x59df7c]; fxch st(1);
     * fadd [0x59df3c]; fxch st(1); fchs; fcompp; fnstsw ax; test ah,0x41;
     * jne 0x40560c */
    FLD32(esp + 0x48u);
    FLD32(esp + 0x4cu);
    XS(0) *= RDF(0x0059df7cu);
    FXCH(1);
    XS(0) += RDF(0x0059df3cu);
    FXCH(1);
    XS(0) = -XS(0);
    FCOM(cg, XS(0), XS(1)); FPOP(); FPOP();
    FNSTSW_AX();
    if (((eax >> 8) & 0x41u) != 0u) goto L_0040560c;
    /* 004055ff: eax=[esp+0x44]; inc eax; fstp st(0); [esp+0x44]=eax; jmp 0x405659 */
    eax = RD(esp + 0x44u);
    eax++;
    FPOP();
    WR(esp + 0x44u, eax);
    goto L_00405659;
L_0040560c:
    /* 0040560c: fld [esp+0x30]; fsub [0x59df3c]; fcomp st(1); fnstsw ax;
     * test ah,0x41; fstp st(0); jne 0x405646 */
    FLD32(esp + 0x30u);
    XS(0) -= RDF(0x0059df3cu);
    FCOM(cg, XS(0), XS(1));
    FPOP();
    FNSTSW_AX();
    FPOP();
    if (((eax >> 8) & 0x41u) != 0u) goto L_00405646;
    /* 00405621: fld [esp+0x4c]; fld [esp+0x48]; fsub [0x59df3c]; fxch st(1);
     * fmul [0x59df7c]; fcompp; fnstsw ax; test ah,1; je 0x405646 */
    FLD32(esp + 0x4cu);
    FLD32(esp + 0x48u);
    XS(0) -= RDF(0x0059df3cu);
    FXCH(1);
    XS(0) *= RDF(0x0059df7cu);
    FCOM(cg, XS(0), XS(1)); FPOP(); FPOP();
    FNSTSW_AX();
    if (((eax >> 8) & 1u) == 0u) goto L_00405646;
    /* 00405640: inc dword [esp+0x40]; jmp 0x405659 */
    WR(esp + 0x40u, RD(esp + 0x40u) + 1u);
    goto L_00405659;
L_00405646:
    /* 00405646: ecx=[esp+0x18]; edx=[esp+0x10]; push ecx; push edx; push esi;
     * call 0x4064b0; add esp,0xc */
    ecx = RD(esp + 0x18u);
    edx = RD(esp + 0x10u);
    PUSH(ecx);
    PUSH(edx);
    PUSH(esi);
    ZCALL(wmain_004064b0, 0x00405656u);
    esp += 0xcu;
L_00405659:
    /* 00405659: eax=[esp+0x28]; inc edi; cmp edi,eax; [esp+0x20]=edi;
     * jle 0x40542d */
    eax = RD(esp + 0x28u);
    edi++;
    WR(esp + 0x20u, edi);
    if ((int32_t)edi <= (int32_t)eax) goto L_0040542d;
    /* 0040566a: edi=[esp+0x24] */
    edi = RD(esp + 0x24u);
L_0040566e:
    /* 0040566e: eax=[esp+0x2c]; inc ebx; cmp ebx,eax; [esp+0x14]=ebx;
     * jle 0x40541d */
    eax = RD(esp + 0x2cu);
    ebx++;
    WR(esp + 0x14u, ebx);
    if ((int32_t)ebx <= (int32_t)eax) goto L_0040541d;
L_0040567f:
    /* 0040567f: eax=0; the eight 0x59df9x counters = 0 */
    eax = 0;
    WR(0x0059dfacu, eax);
    WR(0x0059dfa8u, eax);
    WR(0x0059dfa4u, eax);
    WR(0x0059df98u, eax);
    WR(0x0059df9cu, eax);
    WR(0x0059dfa0u, eax);
    WR(0x0059dfb0u, eax);
    WR(0x0059dfb4u, eax);

L_004056a9:
    /* 004056a9: pop edi; pop esi; pop ebp; pop ebx; add esp,0x90; ret */
    esp += 4u; edi = RD(esp - 4u);
    esp += 4u; esi = RD(esp - 4u);
    esp += 4u; ebp = RD(esp - 4u);
    esp += 4u; ebx = RD(esp - 4u);
    esp += 0x90u;
    esp += 4u;                              /* ret */

    memcpy(cg->st, xs, sizeof xs);
    cg->ftop = xt;
    cg->r[EAX] = eax; cg->r[ECX] = ecx; cg->r[EDX] = edx;
    cg->r[EBX] = ebx; cg->r[EBP] = ebp; cg->r[ESI] = esi; cg->r[EDI] = edi;
    cg->r[ESP] = esp;
}


void wmain_00404730(cpu_t *cg)
{
    if (sk_off()) { wmain_00404730_ref(cg); return; }
    { static int once; if (!once) { once = 1; atexit(sk_at_exit); } }
    sk_calls++;
    sk_body(cg);
}
