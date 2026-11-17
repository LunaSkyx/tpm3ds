/* Hand-written implementations of the hottest small lifted guest functions.
 *
 * tools/lifter/lift.py keeps the lifted bodies of exactly these addresses as
 * wmain_<va>_ref (compiled only with TPM_OVERRIDE_REF) and makes the dispatch
 * table and every call site use the functions below. tests/test_override.c
 * checks every observable effect (registers, lazy-flag fields, eflags, x87
 * stack/control/status, guest memory) against those references.
 *
 * Calling model: same as lifted code. Arguments are on the guest stack (cdecl),
 * the return address is [esp], the callee adds 4 to ESP for `ret`. Values read
 * from guest memory use the bounds-checked gmem helpers; floats go through
 * memcpy so unaligned guest pointers work.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "core/cpu.h"
#include "core/gmem.h"

/* Guest memory through the arena base cached in locals, like GMEM_LOCAL/GRD32
 * in the generated code: one compare against the size, then a direct memcpy. */
#define NF_LOCAL uint8_t *const gm = g_gmem; const uint32_t gsz = g_gmem_size

static inline float nf_ldf(uint8_t *b, uint32_t sz, gptr a)
{
    return gmem_f32_l(gmem_rd32_l(b, sz, a));
}

static inline void nf_stf(uint8_t *b, uint32_t sz, gptr a, float f)
{
    gmem_wr32_l(b, sz, a, gmem_u32_f(f));
}

static inline uint32_t nf_rd32(uint8_t *b, uint32_t sz, gptr a) { return gmem_rd32_l(b, sz, a); }
static inline uint8_t  nf_rd8(uint8_t *b, uint32_t sz, gptr a) { return gmem_rd8_l(b, sz, a); }
static inline uint16_t nf_rd16(uint8_t *b, uint32_t sz, gptr a) { return gmem_rd16_l(b, sz, a); }
static inline float nf_d2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline void nf_wr32(uint8_t *b, uint32_t sz, gptr a, uint32_t v) { gmem_wr32_l(b, sz, a, v); }
static inline void nf_wr8(uint8_t *b, uint32_t sz, gptr a, uint8_t v) { gmem_wr8_l(b, sz, a, v); }

/* cpu_fsw_at() with a compile-time-known TOP, as the lifted static-x87 code has. */
static inline uint32_t nf_fsw_at(cpu_t *c, uint32_t top)
{
    return (c->fsw & ~0x3800u) | ((top & 7u) << 11);
}

/* 004075c6(a, out, s): per-sprite/material setup. out[0..3] all become
 * (float)(s * 255.0f) (the constant at 0x4A8090), then up to three material
 * records adjust them: a[0x1c], a[0x1d], a[0xc] index a 128-byte record; when
 * record[0x38] == 1 (and, for the first two, record[0x30] == a[0x1e]/a[0x1f])
 * a delta of record[0x40]*a[0x10] (or a[0x18] for the third) is added to out[3]
 * and record[0..2]*delta to out[0..2]; otherwise the selector byte is cleared.
 * One x87 slot (ftop+7) is live throughout; the return spill writes it back.
 * EAX/ECX/EDX follow the original's own last writes on every path. */
void wmain_004075c6(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    uint32_t top = cg->ftop;
    gptr a = nf_rd32(gm, gsz, esp + 4u);
    gptr out = nf_rd32(gm, gsz, esp + 8u);
    double fs7 = cg->st[(top + 7u) & 7u];
    uint32_t eax, ecx, edx, sel;
    float tmp;
    double d;

    /* fld dword [esp+0xc]; fmul dword [0x4a8090]; fstp dword [out];
     * then [out+4] = [out+8] = [out+0xc] = [out] (dword copies) */
    d = (double)nf_ldf(gm, gsz, esp + 0xcu) * (double)nf_ldf(gm, gsz, 0x4a8090u);
    fs7 = d;
    nf_stf(gm, gsz, out, (float)d);
    ecx = out;
    edx = out;
    eax = nf_rd32(gm, gsz, edx);
    nf_wr32(gm, gsz, ecx + 4u, eax);
    ecx = out;
    edx = out;
    eax = nf_rd32(gm, gsz, edx);
    nf_wr32(gm, gsz, ecx + 8u, eax);
    ecx = out;
    edx = out;
    eax = nf_rd32(gm, gsz, edx);
    nf_wr32(gm, gsz, ecx + 0xcu, eax);

    /* ---- block 1: selector a[0x1c], compare a[0x1e], scale a[0x10],
     * record base 0x59dffc (+0x30 cmp, +0x38 flag, +0x40 depth) ---- */
    ecx = a;
    edx = 0;
    edx = (edx & 0xffffff00u) | nf_rd8(gm, gsz, ecx + 0x1cu);
    if (edx != 0u) {
        eax = a;
        ecx = 0;
        ecx = (ecx & 0xffffff00u) | nf_rd8(gm, gsz, eax + 0x1cu);
        sel = ecx;                              /* [ebp-4] */
        edx = sel << 7;
        if (nf_rd32(gm, gsz, edx + 0x59e034u) == 1u) {
            eax = sel << 7;
            ecx = a;
            edx = 0;
            edx = (edx & 0xffffff00u) | nf_rd8(gm, gsz, ecx + 0x1eu);
            if (nf_rd32(gm, gsz, eax + 0x59e02cu) == edx) {
                eax = sel << 7;
                fs7 = (double)nf_ldf(gm, gsz, eax + 0x59e03cu);
                ecx = a;
                fs7 = fs7 * (double)nf_ldf(gm, gsz, ecx + 0x10u);
                tmp = (float)fs7;               /* fstp dword [ebp-8] */
                edx = out;
                fs7 = (double)nf_ldf(gm, gsz, edx + 0xcu) + (double)tmp;
                nf_stf(gm, gsz, edx + 0xcu, (float)fs7);
                ecx = sel << 7;
                fs7 = (double)nf_ldf(gm, gsz, ecx + 0x59dffcu) * (double)tmp;
                edx = out;
                fs7 = fs7 + (double)nf_ldf(gm, gsz, edx);
                eax = out;
                nf_stf(gm, gsz, eax, (float)fs7);
                ecx = sel << 7;
                fs7 = (double)nf_ldf(gm, gsz, ecx + 0x59e000u) * (double)tmp;
                edx = out;
                fs7 = fs7 + (double)nf_ldf(gm, gsz, edx + 4u);
                eax = out;
                nf_stf(gm, gsz, eax + 4u, (float)fs7);
                ecx = sel << 7;
                fs7 = (double)nf_ldf(gm, gsz, ecx + 0x59e004u) * (double)tmp;
                edx = out;
                fs7 = fs7 + (double)nf_ldf(gm, gsz, edx + 8u);
                eax = out;
                nf_stf(gm, gsz, eax + 8u, (float)fs7);
                goto after1;
            }
        }
        ecx = a;
        nf_wr8(gm, gsz, ecx + 0x1cu, 0u);
    }
after1:

    /* ---- block 2: selector a[0x1d], compare a[0x1f], scale a[0x14],
     * record base 0x5a5ffc (+0x30 cmp, +0x38 flag, +0x40 depth) ---- */
    edx = a;
    eax = 0;
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, edx + 0x1du);
    if (eax != 0u) {
        ecx = a;
        edx = 0;
        edx = (edx & 0xffffff00u) | nf_rd8(gm, gsz, ecx + 0x1du);
        sel = edx;
        eax = sel << 7;
        if (nf_rd32(gm, gsz, eax + 0x5a6034u) == 1u) {
            ecx = sel << 7;
            edx = a;
            eax = 0;
            eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, edx + 0x1fu);
            if (nf_rd32(gm, gsz, ecx + 0x5a602cu) == eax) {
                ecx = sel << 7;
                fs7 = (double)nf_ldf(gm, gsz, ecx + 0x5a603cu);
                edx = a;
                fs7 = fs7 * (double)nf_ldf(gm, gsz, edx + 0x14u);
                tmp = (float)fs7;
                eax = out;
                fs7 = (double)nf_ldf(gm, gsz, eax + 0xcu) + (double)tmp;
                ecx = out;
                nf_stf(gm, gsz, ecx + 0xcu, (float)fs7);
                edx = sel << 7;
                fs7 = (double)nf_ldf(gm, gsz, edx + 0x5a5ffcu) * (double)tmp;
                eax = out;
                fs7 = fs7 + (double)nf_ldf(gm, gsz, eax);
                ecx = out;
                nf_stf(gm, gsz, ecx, (float)fs7);
                edx = sel << 7;
                fs7 = (double)nf_ldf(gm, gsz, edx + 0x5a6000u) * (double)tmp;
                eax = out;
                fs7 = fs7 + (double)nf_ldf(gm, gsz, eax + 4u);
                ecx = out;
                nf_stf(gm, gsz, ecx + 4u, (float)fs7);
                edx = sel << 7;
                fs7 = (double)nf_ldf(gm, gsz, edx + 0x5a6004u) * (double)tmp;
                eax = out;
                fs7 = fs7 + (double)nf_ldf(gm, gsz, eax + 8u);
                ecx = out;
                nf_stf(gm, gsz, ecx + 8u, (float)fs7);
                goto after2;
            }
        }
        edx = a;
        nf_wr8(gm, gsz, edx + 0x1du, 0u);
    }
after2:

    /* ---- block 3: selector a[0xc], value a[0x18], record base 0x5adffc
     * (+0x38 flag); no per-record compare ---- */
    eax = a;
    ecx = 0;
    ecx = (ecx & 0xffffff00u) | nf_rd8(gm, gsz, eax + 0xcu);
    if (ecx != 0u) {
        edx = a;
        eax = 0;
        eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, edx + 0xcu);
        sel = eax;
        ecx = sel << 7;
        if (nf_rd32(gm, gsz, ecx + 0x5ae034u) == 1u) {
            edx = a;
            tmp = nf_ldf(gm, gsz, edx + 0x18u);    /* [edx+0x18] read as float */
            ecx = out;
            fs7 = (double)nf_ldf(gm, gsz, ecx + 0xcu) + (double)tmp;
            edx = out;
            nf_stf(gm, gsz, edx + 0xcu, (float)fs7);
            eax = sel << 7;
            fs7 = (double)nf_ldf(gm, gsz, eax + 0x5adffcu) * (double)tmp;
            ecx = out;
            fs7 = fs7 + (double)nf_ldf(gm, gsz, ecx);
            edx = out;
            nf_stf(gm, gsz, edx, (float)fs7);
            eax = sel << 7;
            fs7 = (double)nf_ldf(gm, gsz, eax + 0x5ae000u) * (double)tmp;
            ecx = out;
            fs7 = fs7 + (double)nf_ldf(gm, gsz, ecx + 4u);
            edx = out;
            nf_stf(gm, gsz, edx + 4u, (float)fs7);
            eax = sel << 7;
            fs7 = (double)nf_ldf(gm, gsz, eax + 0x5ae004u) * (double)tmp;
            ecx = out;
            fs7 = fs7 + (double)nf_ldf(gm, gsz, ecx + 8u);
            edx = out;
            nf_stf(gm, gsz, edx + 8u, (float)fs7);
        } else {
            eax = a;
            nf_wr8(gm, gsz, eax + 0xcu, 0u);
        }
    }

    cg->r[EAX] = eax;
    cg->r[ECX] = ecx;
    cg->r[EDX] = edx;
    cg->r[ESP] = esp + 4u;                  /* EBP pushed/popped */
    cg->st[(top + 7u) & 7u] = fs7;
}

/* 004064b0(recs, r0, r1): per-sprite submit. Walks recs[0] 0x40-byte records
 * (with an optional 0x40-byte-per-sprite-index advance), projects a point with
 * 0047e34d against matrix 0x59DEC8, clips against the camera constants, and for
 * accepted sprites writes the record pointer into the vertex buffer at 0x8A4880
 * (12 bytes per entry, counter at 0x59DEBC), advances the per-slot pointer at
 * 0x8BC880[rec[0x24]], sets the palette byte rec[0xe], bumps the
 * 0x59DF98/0x59DFA4/0x59DFA8/0x59DFAC counters and marks the palette entry at
 * 0x8BC920[rec[0x30]]. Two calls to the CRT float->int64 helper 0x49A44C.
 * Transcribed line by line from the generated wmain_004064b0, including the
 * exact register/x87-slot/eflags round-trip at every call and the scratch use
 * of the caller's second argument slot. */
#define AF_CALL(cg, target, ret, depth) do { \
    nf_wr32(gm, gsz, esp - 4u, (ret)); esp -= 4u; \
    cg->r[EAX] = eax; cg->r[ECX] = ecx; cg->r[EDX] = edx; cg->r[EBX] = ebx; \
    cg->r[ESP] = esp; cg->r[EBP] = ebp; cg->r[ESI] = esi; cg->r[EDI] = edi; \
    cg->eflags = eflags; \
    cg->st[(T + 5u) & 7u] = v5; cg->st[(T + 6u) & 7u] = v6; cg->st[(T + 7u) & 7u] = v7; \
    cg->ftop = (T - (uint32_t)(depth)) & 7u; \
    cpu_call(cg, (target)); \
    eax = cg->r[EAX]; ecx = cg->r[ECX]; edx = cg->r[EDX]; ebx = cg->r[EBX]; \
    esp = cg->r[ESP]; ebp = cg->r[EBP]; esi = cg->r[ESI]; edi = cg->r[EDI]; \
    eflags = cg->eflags; \
    T = cg->ftop; \
    v5 = cg->st[(T + 5u) & 7u]; v6 = cg->st[(T + 6u) & 7u]; v7 = cg->st[(T + 7u) & 7u]; \
} while (0)

void wmain_004064b0(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    uint32_t T = cg->ftop;
    uint32_t eax = cg->r[EAX], ecx = cg->r[ECX], edx = cg->r[EDX];
    uint32_t ebx = cg->r[EBX], ebp = cg->r[EBP], esi = cg->r[ESI], edi = cg->r[EDI];
    uint32_t ebx_in = ebx, ebp_in = ebp, esi_in = esi, edi_in = edi;
    uint32_t eflags = cg->eflags;
    double v5 = cg->st[(T + 5u) & 7u], v6 = cg->st[(T + 6u) & 7u], v7 = cg->st[(T + 7u) & 7u];

    /* 004064b0: mov eax,[0x59debc] */
    eax = nf_rd32(gm, gsz, 0x59debcu);
    /* 004064b5: sub esp,0x18 ; 004064bd: push ebx/esi/edi ; 004064c0: jae */
    esp -= 0x18u;
    nf_wr32(gm, gsz, esp - 4u, ebx); esp -= 4u;
    nf_wr32(gm, gsz, esp - 4u, esi); esp -= 4u;
    nf_wr32(gm, gsz, esp - 4u, edi); esp -= 4u;
    if (eax >= 0x2000u) goto L_81c;
    /* 004064c6: mov ebx,[esp+0x28] */
    ebx = nf_rd32(gm, gsz, esp + 0x28u);
    /* 004064ca: mov eax,[esp+0x2c] */
    eax = nf_rd32(gm, gsz, esp + 0x2cu);
    /* 004064ce: mov ecx,[esp+0x30] */
    ecx = nf_rd32(gm, gsz, esp + 0x30u);
    /* 004064d2: mov [esp+0x18],eax */
    nf_wr32(gm, gsz, esp + 0x18u, eax);
    /* 004064d6: mov al,[ebx] */
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, ebx);
    /* 004064d8: mov [esp+0x1c],ecx */
    nf_wr32(gm, gsz, esp + 0x1cu, ecx);
    /* 004064dc: or cl,0xff */
    ecx = (ecx & ~0xffu) | 0xffu;
    /* 004064df: xor edi,edi */
    edi = 0u;
    /* 004064e3: lea esi,[ebx+0x10] ; 004064e6: mov dword [esp+0x20],0 */
    esi = ebx + 0x10u;
    nf_wr32(gm, gsz, esp + 0x20u, 0u);
    /* 004064e1/004064ee: test al,al ; jbe */
    if ((uint8_t)eax == 0u) goto L_81c;
    /* 004064f4: push ebp ; 004064f5: mov ebp,0x100 */
    nf_wr32(gm, gsz, esp - 4u, ebp); esp -= 4u;
    ebp = 0x100u;

L_fa:
    /* 004064fa: cmp word [esi],0xffff ; je */
    if (nf_rd16(gm, gsz, esi) == 0xffffu) goto L_80b;
    /* 00406505: test byte [esi+0x2c],0x80 ; jne */
    if (nf_rd8(gm, gsz, esi + 0x2cu) & 0x80u) goto L_80b;
    /* 0040650f: mov dl,[0x59df44] */
    edx = (edx & 0xffffff00u) | nf_rd8(gm, gsz, 0x59df44u);
    /* 00406515: mov al,[esi+0x38] ; 00406518: test al,dl ; 0040651a: je 0x40654e */
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x38u);
    if (((uint8_t)eax & (uint8_t)edx) == 0u) goto L_54e;
    /* 0040651c: mov al,[esi+0x1e] ; 0040651f: test al,al ; je */
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x1eu);
    if ((uint8_t)eax == 0u) goto L_80b;
    /* 00406527: mov edx,[0x59dfa4] ; 0040652d: and eax,0xff ; add edx,eax ; xor eax,eax */
    edx = nf_rd32(gm, gsz, 0x59dfa4u);
    eax &= 0xffu;
    edx += eax;
    eax = 0u;
    /* 00406536: mov [0x59dfa4],edx */
    nf_wr32(gm, gsz, 0x59dfa4u, edx);
    /* 0040653c: mov al,[esi+0x1e] ; add edi,eax ; shl eax,6 ; add esi,eax */
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x1eu);
    edi += eax;
    eax <<= 6;
    esi += eax;
    /* 00406546: or cl,0xff ; 00406549: jmp 0x40680b */
    ecx = (ecx & ~0xffu) | 0xffu;
    goto L_80b;

L_54e:
    /* 0040654e: mov al,[esi+0x1c] ; 00406551: cmp al,cl ; 00406553: je 0x40662e */
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x1cu);
    if ((uint8_t)eax == (uint8_t)ecx) goto L_62e;
    /* 00406559: test byte [esi+0x28],4 ; 0040655d: jne 0x40662e */
    if (nf_rd8(gm, gsz, esi + 0x28u) & 0x04u) goto L_62e;
    /* 00406563: and eax,0xff ; 00406568: push 0x59dec8 */
    eax &= 0xffu;
    nf_wr32(gm, gsz, esp - 4u, 0x59dec8u); esp -= 4u;
    /* 0040656d: mov [esp+0x34],eax */
    nf_wr32(gm, gsz, esp + 0x34u, eax);
    /* 00406571: lea eax,[esp+0x20] ; 00406575: fild dword [esp+0x34] */
    eax = esp + 0x20u;
    v7 = (double)(int32_t)nf_rd32(gm, gsz, esp + 0x34u);
    /* 00406579: lea ecx,[esp+0x14] ; 0040657d: push eax ; 0040657e: push ecx */
    ecx = esp + 0x14u;
    nf_wr32(gm, gsz, esp - 4u, eax); esp -= 4u;
    nf_wr32(gm, gsz, esp - 4u, ecx); esp -= 4u;
    /* 0040657f: fsub dword [0x4a8054] ; 00406585: fstp dword [esp+0x30] */
    v7 = v7 - (double)nf_ldf(gm, gsz, 0x4a8054u);
    nf_stf(gm, gsz, esp + 0x30u, (float)v7);
    /* 00406589: call 0x47e34d */
    AF_CALL(cg, 0x0047e34du, 0x0040658eu, 0);
    /* 0040658e: fld [esp+0x20] ; 00406592: fmul [0x59dec4] */
    v7 = (double)nf_ldf(gm, gsz, esp + 0x20u) * (double)nf_ldf(gm, gsz, 0x59dec4u);
    /* 00406598: fld [esp+0x24] ; 0040659c: fsub [0x59dea8] */
    v6 = (double)nf_ldf(gm, gsz, esp + 0x24u) - (double)nf_ldf(gm, gsz, 0x59dea8u);
    /* 004065a2: fld st(1) ; 004065a4: fcompp */
    v5 = v7;
    FCOM(cg, v5, v6);
    /* 004065a6: add esp,0xc */
    esp += 0xcu;
    /* 004065a9: fnstsw ax ; 004065ab: test ah,1 ; 004065ae: jne 0x406823 */
    eax = (eax & 0xffff0000u) | (nf_fsw_at(cg, (T - 1u) & 7u) & 0xffffu);
    if (eax & 0x0100u) goto L_823;
    /* 004065b4: fld [esp+0x18] ; 004065b8: fadd [0x59dea8] */
    v6 = (double)nf_ldf(gm, gsz, esp + 0x18u) + (double)nf_ldf(gm, gsz, 0x59dea8u);
    /* 004065be: fxch st(1) ; 004065c0: fchs */
    { double t = v6; v6 = v7; v7 = t; }
    v6 = -v6;
    /* 004065c2: fcompp */
    FCOM(cg, v6, v7);
    /* 004065c4: fnstsw ax ; 004065c6: test ah,0x41 ; 004065c9: je 0x406825 */
    eax = (eax & 0xffff0000u) | (nf_fsw_at(cg, (T - 0u) & 7u) & 0xffffu);
    if ((eax & 0x4100u) == 0u) goto L_825;
    /* 004065cf: fld [esp+0x14] ; 004065d3: fmul [0x59df7c] */
    v7 = (double)nf_ldf(gm, gsz, esp + 0x14u) * (double)nf_ldf(gm, gsz, 0x59df7cu);
    /* 004065d9: fld [esp+0x10] ; 004065dd: fadd [0x59df3c] */
    v6 = (double)nf_ldf(gm, gsz, esp + 0x10u) + (double)nf_ldf(gm, gsz, 0x59df3cu);
    /* 004065e3: fxch st(1) ; 004065e5: fchs ; 004065e7: fxch st(1) */
    { double t = v6; v6 = v7; v7 = t; }
    v6 = -v6;
    { double t = v6; v6 = v7; v7 = t; }
    /* 004065e9: fld st(1) ; 004065eb: fcompp */
    v5 = v7;
    FCOM(cg, v5, v6);
    /* 004065ed: fnstsw ax ; 004065ef: test ah,0x41 ; 004065f2: je 0x406823 */
    eax = (eax & 0xffff0000u) | (nf_fsw_at(cg, (T - 1u) & 7u) & 0xffffu);
    if ((eax & 0x4100u) == 0u) goto L_823;
    /* 004065f8: fld [esp+0x10] ; 004065fc: fsub [0x59df3c] */
    v6 = (double)nf_ldf(gm, gsz, esp + 0x10u) - (double)nf_ldf(gm, gsz, 0x59df3cu);
    /* 00406602: fxch st(1) ; 00406604: fchs ; 00406606: fcompp */
    { double t = v6; v6 = v7; v7 = t; }
    v6 = -v6;
    FCOM(cg, v6, v7);
    /* 00406608: fnstsw ax ; 0040660a: test ah,1 ; 0040660d: jne 0x406825 */
    eax = (eax & 0xffff0000u) | (nf_fsw_at(cg, (T - 0u) & 7u) & 0xffffu);
    if (eax & 0x0100u) goto L_825;
    /* 00406613: fld [esp+0x14] ; 00406617: fsub [0x4a807c] ; 0040661d: fcomp [0x59df94] */
    v7 = (double)nf_ldf(gm, gsz, esp + 0x14u) - (double)nf_ldf(gm, gsz, 0x4a807cu);
    FCOM(cg, v7, (double)nf_ldf(gm, gsz, 0x59df94u));
    /* 00406623: fnstsw ax ; 00406625: test ah,1 ; 00406628: je 0x406825 */
    eax = (eax & 0xffff0000u) | (nf_fsw_at(cg, (T - 0u) & 7u) & 0xffffu);
    if ((eax & 0x0100u) == 0u) goto L_825;
    /* fallthrough 0040662e: xor eax,eax */
L_62e:
    eax = 0u;
L_630:
    /* 00406630: test eax,eax ; 00406632: je 0x406666 */
    if (eax == 0u) goto L_666;
    /* 00406634: mov ecx,[0x59dfac] ; 0040663a: mov eax,[0x59dfa8] ; inc ecx */
    ecx = nf_rd32(gm, gsz, 0x59dfacu);
    eax = nf_rd32(gm, gsz, 0x59dfa8u);
    ecx += 1u;
    /* 00406640: xor edx,edx ; 00406642: mov [0x59dfac],ecx ; mov dl,[esi+0x1d] */
    edx = 0u;
    nf_wr32(gm, gsz, 0x59dfacu, ecx);
    edx = (edx & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x1du);
    /* 0040664b: add eax,edx ; 0040664d: mov [0x59dfa8],eax ; xor eax,eax */
    eax += edx;
    nf_wr32(gm, gsz, 0x59dfa8u, eax);
    eax = 0u;
    /* 00406654: mov al,[esi+0x1d] ; add edi,eax ; shl eax,6 ; add esi,eax */
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x1du);
    edi += eax;
    eax <<= 6;
    esi += eax;
    /* 0040665e: or cl,0xff ; 00406661: jmp 0x40680b */
    ecx = (ecx & ~0xffu) | 0xffu;
    goto L_80b;

L_666:
    /* 00406666: mov al,[0x59df54] ; 0040666b: mov cl,[esi+0xc] */
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, 0x59df54u);
    ecx = (ecx & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0xcu);
    /* 0040666e: test al,cl ; 00406670: jne 0x406804 */
    if (((uint8_t)eax & (uint8_t)ecx) != 0u) goto L_804;
    /* 00406676: mov al,[esi+0x28] ; 00406679: test al,0x10 ; jne */
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x28u);
    if ((uint8_t)eax & 0x10u) goto L_804;
    /* 00406681: test al,4 ; 00406683: je 0x406697 */
    if (((uint8_t)eax & 0x04u) == 0u) goto L_697;
    /* 00406685: xor ecx,ecx ; 00406687: mov cl,[esi+0x2a] ; 0040668a: cmp byte [ecx+0x8bfaa0],2 */
    ecx = 0u;
    ecx = (ecx & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x2au);
    if (nf_rd8(gm, gsz, ecx + 0x8bfaa0u) == 2u) goto L_804;
L_697:
    /* 00406697: test word [esi+0x2c],bp ; 0040669b: je 0x4066b1 */
    if ((nf_rd16(gm, gsz, esi + 0x2cu) & (uint16_t)ebp) == 0u) goto L_6b1;
    /* 0040669d: xor edx,edx ; mov dx,[esi+0x30] ; mov al,[edx+0x8bc920] */
    edx = 0u;
    edx = (edx & 0xffff0000u) | nf_rd16(gm, gsz, esi + 0x30u);
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, edx + 0x8bc920u);
    /* 004066a9: test al,al ; 004066ab: jne 0x406804 */
    if ((uint8_t)eax != 0u) goto L_804;
L_6b1:
    /* 004066b1: mov al,[0x59df44] ; 004066b6: mov cl,[esi+0x38] */
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, 0x59df44u);
    ecx = (ecx & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x38u);
    /* 004066b9: test cl,al ; 004066bb: je 0x4066c8 */
    if (((uint8_t)ecx & (uint8_t)eax) == 0u) goto L_6c8;
    /* 004066bd: inc dword [0x59df98] ; 004066c3: jmp 0x406804 */
    nf_wr32(gm, gsz, 0x59df98u, nf_rd32(gm, gsz, 0x59df98u) + 1u);
    goto L_804;
L_6c8:
    /* 004066c8: mov cl,[esi+0x2b] ; 004066cb: xor eax,eax ; 004066cd: test cl,cl */
    ecx = (ecx & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x2bu);
    eax = 0u;
    nf_wr32(gm, gsz, esp + 0x30u, eax);
    if ((uint8_t)ecx == 0u) goto L_6e4;
    /* 004066d5: and ecx,0x1f ; mov edx,ecx ; sar edx,2 ; lea eax,[edx+ecx*8] */
    ecx &= 0x1fu;
    edx = ecx;
    edx = (uint32_t)((int32_t)edx >> 2);
    eax = edx + ecx * 8u;
    nf_wr32(gm, gsz, esp + 0x30u, eax);
L_6e4:
    /* 004066e4: mov byte [esi+0xe],al ; 004066e7: mov ecx,[0x59df60] */
    nf_wr8(gm, gsz, esi + 0xeu, (uint8_t)eax);
    ecx = nf_rd32(gm, gsz, 0x59df60u);
    /* 004066ed: test ecx,ecx ; 004066ef: je 0x40678f */
    if (ecx == 0u) goto L_78f;
    /* 004066f5: mov cl,[esi+0x38] ; mov dl,[0x59df90] ; mov bl,[0x59df80] */
    ecx = (ecx & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x38u);
    edx = (edx & 0xffffff00u) | nf_rd8(gm, gsz, 0x59df90u);
    ebx = (ebx & 0xffffff00u) | nf_rd8(gm, gsz, 0x59df80u);
    /* 00406704: and dl,cl ; 00406706: je 0x406710 */
    edx = (edx & 0xffffff00u) | (uint8_t)((uint8_t)edx & (uint8_t)ecx);
    if ((uint8_t)edx == 0u) goto L_710;
    /* 00406708: test cl,bl ; 0040670a: jne 0x406804 */
    if (((uint8_t)ecx & (uint8_t)ebx) != 0u) goto L_804;
L_710:
    /* 00406710: test dl,dl ; 00406712: je 0x40674d */
    if ((uint8_t)edx == 0u) goto L_74d;
    /* 00406714: test eax,eax ; 00406716: jne 0x406720 */
    if (eax != 0u) goto L_720;
    /* 00406718: mov dword [esp+0x30],0xff */
    nf_wr32(gm, gsz, esp + 0x30u, 0xffu);
L_720:
    /* 00406720: fild dword [esp+0x30] ; 00406724: fmul [0x59deac] */
    v7 = (double)(int32_t)nf_rd32(gm, gsz, esp + 0x30u) * (double)nf_ldf(gm, gsz, 0x59deacu);
    /* 0040672a: call 0x49a44c */
    AF_CALL(cg, 0x0049a44cu, 0x0040672fu, 1);
    /* 0040672f: test eax,eax ; 00406731: jge 0x406737 */
    if ((int32_t)eax >= 0) goto L_737;
    eax = 0u;
    goto L_743;
L_737:
    /* 00406737: cmp eax,0xff ; 0040673c: jle 0x406743 */
    if ((int32_t)eax <= 0xff) goto L_743;
    eax = 0xffu;
L_743:
    /* 00406743: test eax,eax ; 00406745: je 0x406804 */
    if (eax == 0u) goto L_804;
    /* 0040674b: jmp 0x40678c */
    goto L_78c;
L_74d:
    /* 0040674d: test cl,bl ; 0040674f: je 0x40678f */
    if (((uint8_t)ecx & (uint8_t)ebx) == 0u) goto L_78f;
    /* 00406751: test eax,eax ; 00406753: jne 0x40675d */
    if (eax != 0u) goto L_75d;
    /* 00406755: mov dword [esp+0x30],0xff */
    nf_wr32(gm, gsz, esp + 0x30u, 0xffu);
L_75d:
    /* 0040675d: fld [0x4a8058] ; fsub [0x59deac] ; fild [esp+0x30] ; fmulp st(1) */
    v7 = (double)nf_ldf(gm, gsz, 0x4a8058u) - (double)nf_ldf(gm, gsz, 0x59deacu);
    v6 = (double)(int32_t)nf_rd32(gm, gsz, esp + 0x30u);
    v7 = v7 * v6;
    /* 0040676f: call 0x49a44c */
    AF_CALL(cg, 0x0049a44cu, 0x00406774u, 1);
    /* 00406774: test eax,eax ; 00406776: jge 0x40677c */
    if ((int32_t)eax >= 0) goto L_77c;
    eax = 0u;
    goto L_788;
L_77c:
    /* 0040677c: cmp eax,0xff ; 00406781: jle 0x406788 */
    if ((int32_t)eax <= 0xff) goto L_788;
    eax = 0xffu;
L_788:
    /* 00406788: test eax,eax ; 0040678a: je 0x406804 */
    if (eax == 0u) goto L_804;
L_78c:
    /* 0040678c: mov byte [esi+0xe],al */
    nf_wr8(gm, gsz, esi + 0xeu, (uint8_t)eax);
L_78f:
    /* 0040678f: mov edx,[0x59df88] ; xor ecx,ecx ; mov cl,[esi+0x24] */
    edx = nf_rd32(gm, gsz, 0x59df88u);
    ecx = 0u;
    ecx = (ecx & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x24u);
    /* 0040679a: cmp ecx,edx ; 0040679c: jge 0x406804 */
    if ((int32_t)ecx >= (int32_t)edx) goto L_804;
    /* 0040679e: mov bl,[esi+0x2b] ; 004067a1: test bl,bl ; 004067a3: jne 0x4067a9 */
    ebx = (ebx & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x2bu);
    if ((uint8_t)ebx != 0u) goto L_7a9;
    /* 004067a5: test al,al ; 004067a7: je 0x4067c2 */
    if ((uint8_t)eax == 0u) goto L_7c2;
L_7a9:
    /* 004067a9: test al,al ; 004067ab: mov ecx,edx ; 004067ad: jne 0x4067c2 */
    {
        int nz = ((uint8_t)eax != 0u);
        ecx = edx;
        if (nz) goto L_7c2;
    }
    /* 004067af: mov al,[esi+0x2b] ; and eax,0x1f ; mov dl,al ; shl dl,3 ; shr eax,2 ; add dl,al */
    eax = (eax & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x2bu);
    eax &= 0x1fu;
    edx = (edx & 0xffffff00u) | ((uint8_t)eax);
    edx = (edx & 0xffffff00u) | (uint8_t)(((uint8_t)edx << 3) & 0xffu);
    eax >>= 2;
    edx = (edx & 0xffffff00u) | (uint8_t)(((uint8_t)edx + (uint8_t)eax) & 0xffu);
    /* 004067bf: mov byte [esi+0xe],dl */
    nf_wr8(gm, gsz, esi + 0xeu, (uint8_t)edx);
L_7c2:
    /* 004067c2: mov edx,[0x59debc] ; mov ebx,[ecx*4+0x8bc880] */
    edx = nf_rd32(gm, gsz, 0x59debcu);
    ebx = nf_rd32(gm, gsz, ecx * 4u + 0x8bc880u);
    /* 004067cf: lea eax,[edx+edx*2] ; shl eax,2 ; inc edx */
    eax = edx + edx * 2u;
    eax <<= 2;
    edx += 1u;
    /* 004067d6: mov [eax+0x8a4884],ebx */
    nf_wr32(gm, gsz, eax + 0x8a4884u, ebx);
    /* 004067dc: lea eax,[eax+0x8a4880] ; 004067e2: mov [ecx*4+0x8bc880],eax */
    eax += 0x8a4880u;
    nf_wr32(gm, gsz, ecx * 4u + 0x8bc880u, eax);
    /* 004067e9: mov [0x59debc],edx ; 004067ef: mov dword [eax],esi */
    nf_wr32(gm, gsz, 0x59debcu, edx);
    nf_wr32(gm, gsz, eax, esi);
    /* 004067f1: test word [esi+0x2c],bp ; 004067f5: je 0x406804 */
    if ((nf_rd16(gm, gsz, esi + 0x2cu) & (uint16_t)ebp) == 0u) goto L_804;
    /* 004067f7: xor eax,eax ; mov ax,[esi+0x30] ; mov byte [eax+0x8bc920],1 */
    eax = 0u;
    eax = (eax & 0xffff0000u) | nf_rd16(gm, gsz, esi + 0x30u);
    nf_wr8(gm, gsz, eax + 0x8bc920u, 1u);
L_804:
    /* 00406804: mov cl,[esi+0x1c] ; 00406807: mov ebx,[esp+0x2c] */
    ecx = (ecx & 0xffffff00u) | nf_rd8(gm, gsz, esi + 0x1cu);
    ebx = nf_rd32(gm, gsz, esp + 0x2cu);
L_80b:
    /* 0040680b: xor edx,edx ; 0040680d: inc edi ; mov dl,[ebx] ; add esi,0x40 */
    edx = 0u;
    edi += 1u;
    edx = (edx & 0xffffff00u) | nf_rd8(gm, gsz, ebx);
    esi += 0x40u;
    /* 00406813: cmp edi,edx ; 00406815: jl 0x4064fa */
    if ((int32_t)edi < (int32_t)edx) goto L_fa;
    /* 0040681b: pop ebp */
    esp += 4u;
L_81c:
    /* 0040681c: pop edi ; pop esi ; pop ebx ; add esp,0x18 ; ret */
    esp += 4u;
    esp += 4u;
    esp += 4u;
    esp += 0x18u;
    esp += 4u;

    cg->r[EAX] = eax;
    cg->r[ECX] = ecx;
    cg->r[EDX] = edx;
    cg->r[EBX] = ebx_in;
    cg->r[ESP] = esp;
    cg->r[EBP] = ebp_in;
    cg->r[ESI] = esi_in;
    cg->r[EDI] = edi_in;
    cg->eflags = eflags;
    cg->st[(T + 5u) & 7u] = v5;
    cg->st[(T + 6u) & 7u] = v6;
    cg->st[(T + 7u) & 7u] = v7;
    cg->ftop = T;
    return;

L_823:
    /* 00406823: fstp st(0) */
    (void)v7;
L_825:
    /* 00406825: mov eax,1 ; 0040682a: jmp 0x406630 */
    eax = 1u;
    goto L_630;
}
/* 0047e40a(out, M): transform the 3-vector at out in place by the row-vector
 * affine 4x3 matrix M. The original copies out[0..2] (as dwords) to a stack
 * temporary and calls 0047e34d(out, tmp, M); the math and that call's effects
 * are inlined: out[i] = ((tmp0*M[i] + tmp1*M[3+i]) + tmp2*M[6+i]) + M[9+i]
 * in double, stored as float, with the x87 slots the callee leaves behind
 * (ftop+6 = M[8]*tmp2, ftop+7 = the out[2] double) and the register values the
 * call produces (EAX = M, ECX = EDX = &tmp = esp-0x10). fsw/fcw/ftop and
 * eflags are unchanged. */
void wmain_0047e40a(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    uint32_t top = cg->ftop;
    gptr out = nf_rd32(gm, gsz, esp + 4u);
    gptr mp = nf_rd32(gm, gsz, esp + 8u);
    uint32_t tmp = esp - 0x10u;
    double r0 = (double)nf_ldf(gm, gsz, out);
    double r1 = (double)nf_ldf(gm, gsz, out + 4u);
    double r2 = (double)nf_ldf(gm, gsz, out + 8u);
    double p0, p1, p2, acc;
    int i;

    for (i = 0; i < 3; i++) {
        p0 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)i) * r0;
        p1 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(3 + i)) * r1;
        acc = p0 + p1;
        p2 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(6 + i)) * r2;
        acc = acc + p2;
        acc = acc + (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(9 + i));
        nf_stf(gm, gsz, out + 4u * (uint32_t)i, (float)acc);
    }
    cg->r[EAX] = mp;
    cg->r[ECX] = tmp;
    cg->r[EDX] = tmp;
    cg->r[ESP] = esp + 4u;
    cg->st[(top + 6u) & 7u] = p2;
    cg->st[(top + 7u) & 7u] = acc;
}/* 0047e263(out, M): out = M transposed-ish times out for a 3x3 matrix laid out
 * at M with stride 3 (M[3k+i]); the three results go through the frame
 * temporaries, so only the float bits are written back:
 *   out[i] = ((M[i]*out0 + M[3+i]*out1) + M[6+i]*out2)
 * in double, left to right. ECX ends as the out[2] float bits, EDX as the
 * out[1] bits, EAX as out. ftop+6 ends as M[8]*out2, ftop+7 as the out[2]
 * double; fsw/fcw/ftop and eflags unchanged. */
void wmain_0047e263(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    uint32_t top = cg->ftop;
    gptr out = nf_rd32(gm, gsz, esp + 4u);
    gptr mp = nf_rd32(gm, gsz, esp + 8u);
    double o0 = (double)nf_ldf(gm, gsz, out);
    double o1 = (double)nf_ldf(gm, gsz, out + 4u);
    double o2 = (double)nf_ldf(gm, gsz, out + 8u);
    uint32_t b0, b1, b2;
    double p0, p1, p2, acc;
    float f;

    p0 = (double)nf_ldf(gm, gsz, mp) * o0;
    p1 = (double)nf_ldf(gm, gsz, mp + 0xcu) * o1;
    acc = p0 + p1;
    p2 = (double)nf_ldf(gm, gsz, mp + 0x18u) * o2;
    acc = acc + p2;
    f = (float)acc;
    memcpy(&b0, &f, 4);

    p0 = (double)nf_ldf(gm, gsz, mp + 4u) * o0;
    p1 = (double)nf_ldf(gm, gsz, mp + 0x10u) * o1;
    acc = p0 + p1;
    p2 = (double)nf_ldf(gm, gsz, mp + 0x1cu) * o2;
    acc = acc + p2;
    f = (float)acc;
    memcpy(&b1, &f, 4);

    p0 = (double)nf_ldf(gm, gsz, mp + 8u) * o0;
    p1 = (double)nf_ldf(gm, gsz, mp + 0x14u) * o1;
    acc = p0 + p1;
    p2 = (double)nf_ldf(gm, gsz, mp + 0x20u) * o2;
    acc = acc + p2;
    f = (float)acc;
    memcpy(&b2, &f, 4);

    nf_wr32(gm, gsz, out, b0);
    nf_wr32(gm, gsz, out + 4u, b1);
    nf_wr32(gm, gsz, out + 8u, b2);
    cg->r[EAX] = out;
    cg->r[ECX] = b2;
    cg->r[EDX] = b1;
    cg->r[ESP] = esp + 4u;
    cg->st[(top + 6u) & 7u] = p2;
    cg->st[(top + 7u) & 7u] = acc;
}/* 0047ceb9(dst, src): copy the 3x3 part of the 4x3 matrix at src into dst
 * transposed (dst[3j+i] = src[3i+j], written in the original order) and build
 *   dst[9+i] = -(src[3i+0]*src[9] + src[3i+1]*src[10] + src[3i+2]*src[11])
 * with the same double multiply/add/negate sequence. EAX = ECX = src,
 * EDX = dst; ftop+6 ends as src[8]*src[11], ftop+7 as the negated dst[11]
 * double; fsw/fcw/ftop and eflags unchanged. */
void wmain_0047ceb9(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    uint32_t top = cg->ftop;
    gptr dst = nf_rd32(gm, gsz, esp + 4u);
    gptr src = nf_rd32(gm, gsz, esp + 8u);
    uint32_t t9, t10, t11;
    double p0, p1, p2, acc;
    int i;

    nf_wr32(gm, gsz, dst + 0x04u, nf_rd32(gm, gsz, src + 0x0cu));
    nf_wr32(gm, gsz, dst + 0x08u, nf_rd32(gm, gsz, src + 0x18u));
    nf_wr32(gm, gsz, dst + 0x14u, nf_rd32(gm, gsz, src + 0x1cu));
    nf_wr32(gm, gsz, dst + 0x0cu, nf_rd32(gm, gsz, src + 0x04u));
    nf_wr32(gm, gsz, dst + 0x18u, nf_rd32(gm, gsz, src + 0x08u));
    nf_wr32(gm, gsz, dst + 0x1cu, nf_rd32(gm, gsz, src + 0x14u));
    nf_wr32(gm, gsz, dst + 0x00u, nf_rd32(gm, gsz, src + 0x00u));
    nf_wr32(gm, gsz, dst + 0x10u, nf_rd32(gm, gsz, src + 0x10u));
    nf_wr32(gm, gsz, dst + 0x20u, nf_rd32(gm, gsz, src + 0x20u));    t9 = nf_rd32(gm, gsz, src + 0x24u);
    t10 = nf_rd32(gm, gsz, src + 0x28u);
    t11 = nf_rd32(gm, gsz, src + 0x2cu);

    for (i = 0; i < 3; i++) {
        /* dst[9+i] uses the three consecutive src[i*3..] entries */
        p0 = (double)nf_ldf(gm, gsz, src + 12u * (uint32_t)i) * (double)nf_d2f(t9);
        p1 = (double)nf_ldf(gm, gsz, src + 12u * (uint32_t)i + 4u) * (double)nf_d2f(t10);
        acc = p0 + p1;
        p2 = (double)nf_ldf(gm, gsz, src + 12u * (uint32_t)i + 8u) * (double)nf_d2f(t11);
        acc = acc + p2;
        acc = -acc;
        nf_stf(gm, gsz, dst + 4u * (uint32_t)(9 + i), (float)acc);
    }
    cg->r[EAX] = src;
    cg->r[ECX] = src;
    cg->r[EDX] = dst;
    cg->r[ESP] = esp + 4u;
    cg->st[(top + 6u) & 7u] = p2;
    cg->st[(top + 7u) & 7u] = acc;
}
/* 0047e34d(out, r, M): out[i] = r0*M[i] + r1*M[3+i] + r2*M[6+i] + M[9+i], in
 * double, left to right, stored as float. It loads to x87 slots 6/7 and the
 * return spill writes (ftop+6/ftop+7) back, ending as the last product and the
 * last result. EAX=M, ECX=r; EBX is pushed/popped with ESI/EDI, EBP is the frame
 * pointer, and EDX plus the x87 control/status words are never touched. */
void wmain_0047e34d(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    gptr out = nf_rd32(gm, gsz, esp + 4u), rp = nf_rd32(gm, gsz, esp + 8u), mp = nf_rd32(gm, gsz, esp + 12u);
    uint32_t top = cg->ftop;
    double r0 = (double)nf_ldf(gm, gsz, rp), r1 = (double)nf_ldf(gm, gsz, rp + 4u), r2 = (double)nf_ldf(gm, gsz, rp + 8u);
    double fs6 = cg->st[(top + 6u) & 7u], fs7 = cg->st[(top + 7u) & 7u];
    int i;

    for (i = 0; i < 3; i++) {
        /* fld M[i]; fmul r0; fld M[3+i]; fmul r1; faddp; fld M[6+i]; fmul r2;
         * faddp; fadd M[9+i]; fstp out[i] */
        double a0 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)i);
        double b0 = r0;
        double p0 = a0 * b0;
        double a1 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(3 + i));
        double b1 = r1;
        double p1 = a1 * b1;
        double acc = p0 + p1;
        double a2 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(6 + i));
        double b2 = r2;
        double p2 = a2 * b2;
        acc = acc + p2;
        acc = acc + (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(9 + i));
        nf_stf(gm, gsz, out + 4u * (uint32_t)i, (float)acc);
        fs6 = p2;
        fs7 = acc;
    }
    cg->r[EAX] = mp;
    cg->r[ECX] = rp;                /* EBX is pushed/popped: it stays the caller's */
    cg->r[ESP] = esp + 4u;
    cg->st[(top + 6u) & 7u] = fs6;
    cg->st[(top + 7u) & 7u] = fs7;
}

/* 004074d4(p): 0xFF000000 | clamp(p[0])<<16 | clamp(p[1])<<8 | clamp(p[2]), the
 * clamps on the x87 fist result (fcw rounding), signed. The original pushes
 * ecx/ebx/esi/edi, restores all but ecx, and spills the packed value in EAX and
 * EDX; the last fld value (p[0]) stays in x87 slot ftop+7. */
void wmain_004074d4(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    gptr p = nf_rd32(gm, gsz, esp + 4u);
    uint32_t top = cg->ftop;
    uint32_t packed = 0xff000000u;
    int i;

    /* fld p[2] / p[1] / p[0] then fistp, clamped to 0..255 */
    static const uint32_t order[3] = {2u, 1u, 0u};
    for (i = 0; i < 3; i++) {
        uint32_t k = order[i];
        float f = nf_ldf(gm, gsz, p + 4u * k);
        uint32_t v = cpu_fist32(cg->fcw, (double)f);
        int32_t iv = (int32_t)v;
        if (iv < 0) iv = 0;
        else if (iv > 0xff) iv = 0xff;
        if (k == 2u)      packed |= (uint32_t)iv;             /* mov dl, al  */
        else if (k == 1u) packed |= (uint32_t)iv << 8;         /* mov dh, al  */
        else              packed |= (uint32_t)iv << 16;        /* shl eax,16; or edx,eax */
    }
    cg->r[EAX] = packed;
    cg->r[EDX] = packed;
    cg->r[ECX] = p;                 /* pushed, never popped */
    cg->r[ESP] = esp + 4u;
    cg->st[(top + 7u) & 7u] = (double)nf_ldf(gm, gsz, p);   /* last fld: p[0] */
}

/* 0040e96b(src, dst, mode): gather 8-byte entries.
 * mode & 0x80: perm = T1[mode&7] (4 indices), sel = T2[(mode&0x30)>>4] (3),
 *              dst[i] = src[perm[sel[i]]], i < 3.
 * else:        perm = T1[mode&7], 4 entries.
 * mode == 0 first runs the original's 32-byte rep movsd (DF-honouring), which
 * only matters for overlapping buffers. The masked mode and the advanced dst
 * pointer are written back to the argument slots, exactly like the original.
 * EAX=4; ECX/EDX are the values the loops leave behind. */
void wmain_0040e96b(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    gptr src = nf_rd32(gm, gsz, esp + 4u), dst = nf_rd32(gm, gsz, esp + 8u);
    uint32_t mode = nf_rd32(gm, gsz, esp + 0xc);
    uint32_t ecx_out = cg->r[ECX], edx_out = cg->r[EDX];
    int i;

    /* mode == 0 first runs the original's 8-dword rep movsd dst <- src (DF is
     * honoured, so an overlapping pair sees the same intermediate bytes). The
     * gather below then rewrites the same 32 bytes in identity order. */
    if (mode == 0u) {
        int32_t st = (cg->eflags & F_DF) ? -4 : 4;
        uint32_t si = src, di = dst;
        for (i = 0; i < 8; i++) {
            nf_wr32(gm, gsz, di, nf_rd32(gm, gsz, si));
            si += (uint32_t)st;
            di += (uint32_t)st;
        }
    }

    if (mode & 0x80u) {
        uint32_t row = mode & 7u;
        uint32_t a = (mode & 0x30u) >> 4;
        uint32_t perm[4];
        nf_wr32(gm, gsz, esp + 0xcu, row);   /* [ebp+0x10] = mode & 7 */
        for (i = 0; i < 4; i++) perm[i] = nf_rd32(gm, gsz, 0x4aa340u + row * 16u + 4u * (uint32_t)i);
        for (i = 0; i < 3; i++) {
            uint32_t sel = nf_rd32(gm, gsz, 0x4aa3c0u + (a & 3u) * 16u + 4u * (uint32_t)i);
            uint32_t idx = perm[sel & 3u];
            nf_wr32(gm, gsz, dst + 8u * (uint32_t)i, nf_rd32(gm, gsz, src + 8u * idx));
            nf_wr32(gm, gsz, dst + 8u * (uint32_t)i + 4u, nf_rd32(gm, gsz, src + 8u * idx + 4u));
        }
        nf_wr32(gm, gsz, esp + 8u, dst + 24u);        /* mov [ebp+0xc], dst+8/iter */
        ecx_out = dst + 24u;
        edx_out = dst + 16u;
    } else {
        uint32_t row = mode & 7u;
        nf_wr32(gm, gsz, esp + 0xcu, row);   /* [ebp+0x10] = mode & 7 */
        for (i = 0; i < 4; i++) {
            uint32_t idx = nf_rd32(gm, gsz, 0x4aa340u + row * 16u + 4u * (uint32_t)i);
            nf_wr32(gm, gsz, dst + 8u * (uint32_t)i, nf_rd32(gm, gsz, src + 8u * idx));
            ecx_out = nf_rd32(gm, gsz, src + 8u * idx + 4u);
            nf_wr32(gm, gsz, dst + 8u * (uint32_t)i + 4u, ecx_out);
            edx_out = dst + 8u * (uint32_t)i + 8u;
        }
        nf_wr32(gm, gsz, esp + 8u, edx_out);          /* mov [ebp+0xc], dst+8/iter */
    }

    cg->r[EAX] = 4u;
    cg->r[ECX] = ecx_out;
    cg->r[EDX] = edx_out;
    cg->r[ESP] = esp + 4u;                  /* EBP/ESI/EDI are pushed and popped */
}

/* 0047dad6(dst, A, B): row-vector 4x3 multiply,
 *   dst[3j+c] = A[c]*B[3j] + A[3+c]*B[3j+1] + A[6+c]*B[3j+2]      (j<3)
 *   dst[9+c]  = A[c]*B[9]  + A[3+c]*B[10]  + A[6+c]*B[11] + A[9+c]
 * each term a double multiply, summed left to right, stored as float; reads are
 * interleaved with the stores like the original. EAX=B, ECX=A, EDX=dst. */
void wmain_0047dad6(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    gptr dst = nf_rd32(gm, gsz, esp + 4u), ap = nf_rd32(gm, gsz, esp + 8u), bp = nf_rd32(gm, gsz, esp + 12u);
    uint32_t top = cg->ftop;
    double fs6 = cg->st[(top + 6u) & 7u], fs7 = cg->st[(top + 7u) & 7u];
    int j, c;

    for (j = 0; j < 3; j++)
        for (c = 0; c < 3; c++) {
            double x0 = (double)nf_ldf(gm, gsz, ap + 4u * (uint32_t)c);
            double y0 = (double)nf_ldf(gm, gsz, bp + 12u * (uint32_t)j);
            double p0 = x0 * y0;
            double x1 = (double)nf_ldf(gm, gsz, ap + 4u * (uint32_t)(3 + c));
            double y1 = (double)nf_ldf(gm, gsz, bp + 12u * (uint32_t)j + 4u);
            double p1 = x1 * y1;
            double acc = p0 + p1;
            double x2 = (double)nf_ldf(gm, gsz, ap + 4u * (uint32_t)(6 + c));
            double y2 = (double)nf_ldf(gm, gsz, bp + 12u * (uint32_t)j + 8u);
            double p2 = x2 * y2;
            acc = acc + p2;
            nf_stf(gm, gsz, dst + 4u * (uint32_t)(3 * j + c), (float)acc);
            fs6 = p2;
            fs7 = acc;
        }
    for (c = 0; c < 3; c++) {
        double x0 = (double)nf_ldf(gm, gsz, ap + 4u * (uint32_t)c);
        double y0 = (double)nf_ldf(gm, gsz, bp + 36u);
        double p0 = x0 * y0;
        double x1 = (double)nf_ldf(gm, gsz, ap + 4u * (uint32_t)(3 + c));
        double y1 = (double)nf_ldf(gm, gsz, bp + 40u);
        double p1 = x1 * y1;
        double acc = p0 + p1;
        double x2 = (double)nf_ldf(gm, gsz, ap + 4u * (uint32_t)(6 + c));
        double y2 = (double)nf_ldf(gm, gsz, bp + 44u);
        double p2 = x2 * y2;
        acc = acc + p2;
        acc = acc + (double)nf_ldf(gm, gsz, ap + 4u * (uint32_t)(9 + c));
        nf_stf(gm, gsz, dst + 4u * (uint32_t)(9 + c), (float)acc);
        fs6 = p2;
        fs7 = acc;
    }
    cg->r[EAX] = bp;
    cg->r[ECX] = ap;
    cg->r[EDX] = dst;
    cg->r[ESP] = esp + 4u;
    cg->st[(top + 6u) & 7u] = fs6;
    cg->st[(top + 7u) & 7u] = fs7;
}

/* 0047dee9(dst, M): dst = M x dst in place, i.e. the same formula as 0047dad6
 * with A=M and B=the saved dst. The original saves the 12 floats with a 12-dword
 * rep movsd into its frame; emulated on the guest stack (DF=1 then clobbers the
 * saved ESI/EDI, which the pops then return). EAX=ECX=M, EDX=dst. */
void wmain_0047dee9(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP], ebp = esp - 4u;
    NF_LOCAL;
    gptr dst = nf_rd32(gm, gsz, esp + 4u), mp = nf_rd32(gm, gsz, esp + 8u);
    uint32_t top = cg->ftop;
    double S[12];
    double fs6 = cg->st[(top + 6u) & 7u], fs7 = cg->st[(top + 7u) & 7u];
    uint32_t si, di;
    int k, j, c;

    /* push ebp; sub esp,0x30; push esi; push edi; rep movsd 12 dwords */
    nf_wr32(gm, gsz, ebp, cg->r[EBP]);
    nf_wr32(gm, gsz, ebp - 0x34u, cg->r[ESI]);
    nf_wr32(gm, gsz, ebp - 0x38u, cg->r[EDI]);
    {
        int32_t st = (cg->eflags & F_DF) ? -4 : 4;
        si = dst;
        di = ebp - 0x30u;
        for (k = 0; k < 12; k++) {
            nf_wr32(gm, gsz, di, nf_rd32(gm, gsz, si));
            si += (uint32_t)st;
            di += (uint32_t)st;
        }
    }
    for (k = 0; k < 12; k++) S[k] = (double)nf_ldf(gm, gsz, ebp - 0x30u + 4u * (uint32_t)k);

    for (j = 0; j < 3; j++)
        for (c = 0; c < 3; c++) {
            double x0 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)c);
            double y0 = S[3 * j];
            double p0 = x0 * y0;
            double x1 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(3 + c));
            double y1 = S[3 * j + 1];
            double p1 = x1 * y1;
            double acc = p0 + p1;
            double x2 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(6 + c));
            double y2 = S[3 * j + 2];
            double p2 = x2 * y2;
            acc = acc + p2;
            nf_stf(gm, gsz, dst + 4u * (uint32_t)(3 * j + c), (float)acc);
            fs6 = p2;
            fs7 = acc;
        }
    for (c = 0; c < 3; c++) {
        double x0 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)c);
        double y0 = S[9];
        double p0 = x0 * y0;
        double x1 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(3 + c));
        double y1 = S[10];
        double p1 = x1 * y1;
        double acc = p0 + p1;
        double x2 = (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(6 + c));
        double y2 = S[11];
        double p2 = x2 * y2;
        acc = acc + p2;
        acc = acc + (double)nf_ldf(gm, gsz, mp + 4u * (uint32_t)(9 + c));
        nf_stf(gm, gsz, dst + 4u * (uint32_t)(9 + c), (float)acc);
        fs6 = p2;
        fs7 = acc;
    }

    cg->r[EAX] = mp;
    cg->r[ECX] = mp;
    cg->r[EDX] = dst;
    cg->r[ESP] = esp + 4u;
    cg->r[EBP] = nf_rd32(gm, gsz, ebp);
    cg->r[ESI] = nf_rd32(gm, gsz, ebp - 0x34u);
    cg->r[EDI] = nf_rd32(gm, gsz, ebp - 0x38u);
    cg->st[(top + 6u) & 7u] = fs6;
    cg->st[(top + 7u) & 7u] = fs7;
}

/* 00486245(v): normalise a 3-vector in place and return the length on the x87
 * stack as (double)(float)len. len2 is summed in double as (v0^2+v1^2)+v2^2; the
 * square root is the CRT's own lifted sqrt (0x49a390), because that routine also
 * decides NaN/inf/denormal handling, fcw restore and the caller-visible EAX/ECX/
 * EDX spills - calling it reproduces all of it exactly. Scaling happens only
 * when the sqrt is not equal to 0.0f (C3 clear: equal or unordered both skip)
 * and the float length is not exactly 1.0f. */
void wmain_00486245(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    gptr v = nf_rd32(gm, gsz, esp + 4u);
    uint32_t top = cg->ftop;
    double fs6, fs7, acc, p;
    float d32;
    uint32_t sw, after, ft0, dbits;

    /* fld v0; fmul v0; fld v1; fmul v1; faddp; fld v2; fmul v2; faddp */
    {
        double v0 = (double)nf_ldf(gm, gsz, v);
        acc = v0 * v0;
        double v1 = (double)nf_ldf(gm, gsz, v + 4u);
        p = v1 * v1;
        acc = acc + p;
        double v2 = (double)nf_ldf(gm, gsz, v + 8u);
        fs6 = v2 * v2;
        acc = acc + fs6;
    }
    fs7 = acc;

    /* call 0x49a390 with the value as ST(0): push the return address, spill the
     * x87 top, then reload the depth the callee left (one slot pushed). The
     * callee also leaves EAX/ECX/EDX in cg, which is where the original's
     * post-call reload gets them from. */
    cg->st[(top + 6u) & 7u] = fs6;
    cg->st[(top + 7u) & 7u] = fs7;
    cg->ftop = (top - 1u) & 7u;
    cg->r[EAX] = v;
    cg->r[ECX] = v;
    cg->r[EDX] = v;
    cg->r[ESP] = esp - 0x14u;                 /* push ebp; sub esp,0x10 */
    nf_wr32(gm, gsz, cg->r[ESP] - 4u, 0x00486279u);  /* call 0x49a390 */
    cg->r[ESP] -= 4u;
    cpu_call(cg, 0x0049a390u);                /* the CRT's own lifted sqrt */

    after = cg->ftop;                         /* the callee leaves one slot pushed */
    ft0 = (after + 1u) & 7u;
    fs7 = cg->st[(ft0 + 7u) & 7u];            /* ST(0) after the call */
    fs6 = cg->st[(ft0 + 6u) & 7u];
    d32 = (float)fs7;
    memcpy(&dbits, &d32, 4);

    FCOM(cg, fs7, (double)nf_ldf(gm, gsz, 0x4a8d60u));
    sw = nf_fsw_at(cg, ft0);

    if (!(sw & 0x4000u) && dbits != 0x3f800000u) {
        /* fld 1.0f; fdiv [ebp-8]=d32; fstp [ebp-4]; fmul each component */
        float s;
        double one = (double)nf_ldf(gm, gsz, 0x4a8d6cu);
        double ds = one / (double)d32;
        s = (float)ds;
        nf_stf(gm, gsz, v,      (float)((double)nf_ldf(gm, gsz, v)      * (double)s));
        nf_stf(gm, gsz, v + 4u, (float)((double)nf_ldf(gm, gsz, v + 4u) * (double)s));
        nf_stf(gm, gsz, v + 8u, (float)((double)nf_ldf(gm, gsz, v + 8u) * (double)s));
        cg->r[EAX] = v;
        cg->r[ECX] = v;
        cg->r[EDX] = v;
    } else {
        cg->r[EAX] = (cg->r[EAX] & 0xffff0000u) | (sw & 0xffffu);   /* fnstsw ax */
        /* ECX/EDX keep what the CRT left in cg */
    }

    cg->st[(ft0 + 6u) & 7u] = fs6;
    cg->st[(ft0 + 7u) & 7u] = (double)d32;    /* fld [ebp-8] */
    cg->ftop = (ft0 - 1u) & 7u;
    cg->r[ESP] = esp + 4u;
}

/* 00487f90(a, b): comparator returning -1/0/1 in EAX. Floats at +0, u32 at
 * +8/+0x14. The float compare is the x87 FCOM test: C3 set means equal or
 * unordered (both return 0), otherwise C0 selects -1. EAX/ECX/EDX/ESI/ESP and
 * fsw are spilled; EBP/EDI, ftop, fcw and the lazy-flag fields are not. */
void wmain_00487f90(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    gptr a = nf_rd32(gm, gsz, esp + 4u), b = nf_rd32(gm, gsz, esp + 8u);
    uint32_t top = cg->ftop;
    uint32_t eax = nf_rd32(gm, gsz, b + 8u);       /* mov eax, [edx+8] */
    uint32_t ecx = cg->r[ECX];
    uint32_t res;
    double fs7 = cg->st[(top + 7u) & 7u];

    for (;;) {
        if (nf_rd32(gm, gsz, a + 8u) & 0x100000u) {
            if (!(eax & 0x100000u)) { res = 0xffffffffu; break; }      /* -1 */
            uint32_t u = nf_rd32(gm, gsz, a + 0x14u), vv = nf_rd32(gm, gsz, b + 0x14u);
            ecx = vv;
            if (u < vv) { res = 0xffffffffu; break; }                  /* jb  -> -1 */
            if (u > vv) { res = 1u; break; }                           /* ja  -> 1  */
        } else if (eax & 0x100000u) {
            res = 1u; break;
        }
        /* 0x487fcd: fld a[0]; fcomp b[0] */
        fs7 = (double)nf_ldf(gm, gsz, a);
        FCOM(cg, fs7, (double)nf_ldf(gm, gsz, b));
        if (nf_fsw_at(cg, top) & 0x4000u) { res = 0u; break; }         /* C3: == or unordered */
        fs7 = (double)nf_ldf(gm, gsz, a);
        FCOM(cg, fs7, (double)nf_ldf(gm, gsz, b));
        res = (nf_fsw_at(cg, top) & 0x4100u) ? 0xffffffffu : 1u;       /* C0|C3 */
        break;
    }
    cg->r[EAX] = res;
    cg->r[ECX] = ecx;
    cg->r[EDX] = b;                 /* ESI is pushed/popped: it stays the caller's */
    cg->r[ESP] = esp + 4u;
    cg->st[(top + 7u) & 7u] = fs7;
}

/* 0041af30(src, dst): sprite/light descriptor update, transcribed from the
 * generated wmain_0041af30 (source/gen/wmain_gen_004.c) including its x87
 * stack bookkeeping: three live slots (ftop+5..7), and the "else" branch at
 * 0x41b057 that pushes 1.0, adds it into st(1) and then stores the popped
 * 1.0 before storing the sum. All arithmetic is double with float loads and
 * float stores; the compare tests use the FCOM status bits (C0/C3), so NaN
 * behaves exactly like the lifted body. */
void wmain_0041af30(cpu_t *cg)
{
    uint32_t esp = cg->r[ESP];
    NF_LOCAL;
    gptr src = nf_rd32(gm, gsz, esp + 4u), dst = nf_rd32(gm, gsz, esp + 8u);
    uint32_t top = cg->ftop;
    double fs5 = cg->st[(top + 5u) & 7u];
    double fs6 = cg->st[(top + 6u) & 7u];
    double fs7 = cg->st[(top + 7u) & 7u];
    uint32_t last_sw;
    uint32_t ecx_out = src, edx_out = dst;

    /* mov eax,[ebp+8]; fld [eax+4]; fcomp [0x4a81a8]; fnstsw ax; test ah,0x41 */
    fs7 = (double)nf_ldf(gm, gsz, src + 4u);
    FCOM(cg, fs7, (double)nf_ldf(gm, gsz, 0x4a81a8u));
    last_sw = nf_fsw_at(cg, top);
    if (last_sw & 0x4100u) {
        /* je not taken: out of range -> marker byte, done */
        nf_wr8(gm, gsz, dst, 0x80u);
        ecx_out = dst;
        edx_out = cg->r[EDX];
        goto done;
    }

    {
        double q;

        /* fld [0x5bf9e8]; fdiv [ecx+4]; fld [ecx]; fmul st(1); fadd [0x6bfa1c];
         * fstp [edx+0x10] */
        q = (double)nf_ldf(gm, gsz, 0x5bf9e8u);
        q = q / (double)nf_ldf(gm, gsz, src + 4u);
        fs6 = (double)nf_ldf(gm, gsz, src);
        fs6 = fs6 * q;
        fs6 = fs6 + (double)nf_ldf(gm, gsz, 0x6bfa1cu);
        nf_stf(gm, gsz, dst + 0x10u, (float)fs6);
        /* fmul [ecx+8]; fchs; fadd [0x5bb580]; fstp [edx+0x14] */
        q = q * (double)nf_ldf(gm, gsz, src + 8u);
        q = -q;
        q = q + (double)nf_ldf(gm, gsz, 0x5bb580u);
        fs7 = q;
        nf_stf(gm, gsz, dst + 0x14u, (float)fs7);
        nf_wr32(gm, gsz, dst + 8u, 0xff000000u);

        if (nf_rd32(gm, gsz, 0x5bb5bcu) != 0u) {
            fs7 = (double)nf_ldf(gm, gsz, 0x5bb614u);
            FCOM(cg, fs7, (double)nf_ldf(gm, gsz, src + 4u));
            last_sw = nf_fsw_at(cg, top);
            if (last_sw & 0x0100u) {                  /* test ah,1: C0 -> enter */
                nf_wr32(gm, gsz, dst + 8u, 0u);
                fs7 = (double)nf_ldf(gm, gsz, 0x5bb5fcu);
                FCOM(cg, fs7, (double)nf_ldf(gm, gsz, src + 4u));
                last_sw = nf_fsw_at(cg, top);
                if (!(last_sw & 0x0100u)) {           /* jne not taken */
                    fs7 = (double)nf_ldf(gm, gsz, src + 4u);
                    fs7 = fs7 - (double)nf_ldf(gm, gsz, 0x5bb614u);
                    fs7 = fs7 / (double)nf_ldf(gm, gsz, 0x5bb60cu);
                    fs7 = fs7 * (double)nf_ldf(gm, gsz, 0x5bb578u);
                    fs7 = -fs7;
                    fs7 = fs7 + (double)nf_ldf(gm, gsz, 0x5bb578u);
                    nf_wr32(gm, gsz, dst + 8u, cpu_fist32(cg->fcw, fs7) << 24);
                }
            }
        }

        if (nf_rd32(gm, gsz, 0x6bfa14u) != 0u) {
            /* fld [ecx+4]; fld [edx+0x14]; fld [edx+0x10]; mov byte [edx],0 */
            fs7 = (double)nf_ldf(gm, gsz, src + 4u);
            fs6 = (double)nf_ldf(gm, gsz, dst + 0x14u);
            fs5 = (double)nf_ldf(gm, gsz, dst + 0x10u);
            nf_wr8(gm, gsz, dst, 0u);
            FCOM(cg, fs5, (double)nf_ldf(gm, gsz, 0x5bf9d4u));
            last_sw = nf_fsw_at(cg, (top - 2u) & 7u);
            if (last_sw & 0x0100u) nf_wr8(gm, gsz, dst, (uint8_t)(nf_rd8(gm, gsz, dst) | 1u));
            else {
                FCOM(cg, fs5, (double)nf_ldf(gm, gsz, 0x5bb600u));
                last_sw = nf_fsw_at(cg, (top - 2u) & 7u);
                if (!(last_sw & 0x4100u)) nf_wr8(gm, gsz, dst, (uint8_t)(nf_rd8(gm, gsz, dst) | 2u));
            }
            FCOM(cg, fs6, (double)nf_ldf(gm, gsz, 0x5bf9d8u));
            last_sw = nf_fsw_at(cg, (top - 1u) & 7u);
            if (last_sw & 0x0100u) nf_wr8(gm, gsz, dst, (uint8_t)(nf_rd8(gm, gsz, dst) | 8u));
            else {
                FCOM(cg, fs6, (double)nf_ldf(gm, gsz, 0x5bb604u));
                last_sw = nf_fsw_at(cg, (top - 1u) & 7u);
                if (!(last_sw & 0x4100u)) nf_wr8(gm, gsz, dst, (uint8_t)(nf_rd8(gm, gsz, dst) | 4u));
            }
            FCOM(cg, fs7, (double)nf_ldf(gm, gsz, 0x5bb5f8u));
            last_sw = nf_fsw_at(cg, top);
            if (last_sw & 0x0100u) nf_wr8(gm, gsz, dst, (uint8_t)(nf_rd8(gm, gsz, dst) | 0x10u));
        }

        /* fld1; fdiv [ecx+4]; fst [edx+0x18]; fmul [0x5bb5b0] */
        fs7 = 1.0;
        fs7 = fs7 / (double)nf_ldf(gm, gsz, src + 4u);
        nf_stf(gm, gsz, dst + 0x18u, (float)fs7);
        fs7 = fs7 * (double)nf_ldf(gm, gsz, 0x5bb5b0u);
        if (nf_rd32(gm, gsz, 0x866fc8u) != 0x10u) {
            /* fchs; fld1; fadd st(1),st(0); fstp [edx+0x1c]; fstp [edx+0x1c] */
            fs7 = -fs7;
            fs6 = 1.0;
            fs7 = fs7 + fs6;
            nf_stf(gm, gsz, dst + 0x1cu, (float)fs6);
        }
        nf_stf(gm, gsz, dst + 0x1cu, (float)fs7);
    }
done:
    cg->r[EAX] = (src & 0xffff0000u) | (last_sw & 0xffffu);
    cg->r[ECX] = ecx_out;
    cg->r[EDX] = edx_out;
    cg->r[ESP] = esp + 4u;
    cg->st[(top + 5u) & 7u] = fs5;
    cg->st[(top + 6u) & 7u] = fs6;
    cg->st[(top + 7u) & 7u] = fs7;
}

