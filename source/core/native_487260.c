/* Native 0x00487260: the per-draw wrapper (379 calls/frame, 4.35 ms/frame inclusive).
 *
 * A transcription of build-host/wrapper_stream.txt (89 instructions after the labels), the
 * per-draw path that world_emit 0x406830 spends its time in. The body is small; the reason it
 * costs 4.35 ms is that the lifted body syncs every register around each of its six calls, and
 * this runs 379 times per frame. A native writes the call frames by hand instead.
 *
 * What it does:
 *  - if [0x882298] is set, a global render-state dance (0x488210/0x488510 twice around 0x4873e0);
 *  - if [0x734c00] is set and the vertex count (arg1) is <= 0x40 and > 0, rewrite up to 0x40
 *    vertices through the colour-key LUT: 32 bytes copied from (arg0 + i*0x20) to
 *    0x854e58 + i*0x20 + 2, then three LUT lookups (table at 0x4b88b8) on the three bytes just
 *    before each destination record; the source pointer handed to the draw becomes 0x854e58;
 *  - then one indirect call through the device vtable `[ *(0x6fc984) ] + 0x70`
 *    (DrawIndexedPrimitive) with the six arguments built from the count, the source vertices,
 *    0x1c4, 6, the device and a flag that is 0x18 or 0x1c;
 *  - finally advances the global vertex counter [0x86a434] by count - 2.
 *
 * Contract: ebx/ebp/esi/edi are restored through the emulated prologue/epilogue, eax ends as
 * (old counter + count - 2), ESP ends at entry + 4, and the guest stack below the frame gets the
 * same four saved registers and the same argument slots as the original (the callees' frames are
 * therefore at the same addresses). No x87 and no flag reads across the calls.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_00487260_ref(cpu_t *c);
void wmain_00488210(cpu_t *c);
void wmain_00488510(cpu_t *c);
void wmain_004873e0(cpu_t *c);

#define G_STATEDANCE 0x00882298u
#define G_VCGATE     0x00734c00u
#define G_VTABLE     0x006fc984u
#define G_VTXCOUNT   0x0086a434u
#define K_LUT        0x004b88b8u
#define K_DST        0x00854e58u
#define K_DST_REC    0x00854e6au

static int wv_off(void)
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

static uint64_t wv_calls;
static void wv_at_exit(void) { fprintf(stderr, "draw-wrapper native: %llu calls\n", (unsigned long long)wv_calls); }

static inline uint32_t wrd(uint8_t *gm, uint32_t gsz, gptr a) { return gmem_rd32_l(gm, gsz, a); }
static inline void wwr(uint8_t *gm, uint32_t gsz, gptr a, uint32_t v) { gmem_wr32_l(gm, gsz, a, v); }
static inline uint8_t wrd8(uint8_t *gm, uint32_t gsz, gptr a) { return gmem_rd8_l(gm, gsz, a); }
static inline void wwr8(uint8_t *gm, uint32_t gsz, gptr a, uint8_t v) { gmem_wr8_l(gm, gsz, a, v); }

/* cdecl guest call with the arguments on the guest stack below sp. */
static void wv_call(cpu_t *cg, void (*fn)(cpu_t *), gptr sp, uint32_t ret, uint32_t n, const uint32_t *a)
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

void wmain_00487260(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    const gptr S = E - 0x10u;             /* push ebx; push ebp; push esi; push edi */
    const uint32_t count = wrd(gm, gsz, E + 8u);      /* arg1 -> ebx */
    uint32_t src = wrd(gm, gsz, E + 4u);              /* arg0 -> esi */
    const uint32_t flag = wrd(gm, gsz, E + 0xcu);     /* arg2, read at the call */
    uint32_t a[6];

    if (wv_off()) { wmain_00487260_ref(cg); return; }
    { static int once; if (!once) { once = 1; atexit(wv_at_exit); } }
    wv_calls++;

    /* the four saved registers, exactly where the original pushes them */
    wwr(gm, gsz, S + 0x0u, cg->r[EDI]);
    wwr(gm, gsz, S + 0x4u, cg->r[EBP]);
    wwr(gm, gsz, S + 0x8u, cg->r[ESI]);
    wwr(gm, gsz, S + 0xcu, cg->r[EBX]);

    if (wrd(gm, gsz, G_STATEDANCE) != 0u) {           /* 00487273: je 0x4872b4 */
        const uint32_t edi = wrd(gm, gsz, 0x00855964u);
        const uint32_t ebp = wrd(gm, gsz, 0x004b88acu);
        a[0] = 0x9000u;
        wv_call(cg, wmain_00488210, S, 0x0048728bu, 1, a);
        a[0] = 0u;
        wv_call(cg, wmain_00488510, S, 0x00487295u, 1, a);
        a[0] = count; a[1] = src;
        wv_call(cg, wmain_004873e0, S, 0x0048729fu, 2, a);
        a[0] = edi;
        wv_call(cg, wmain_00488210, S, 0x004872a8u, 1, a);
        a[0] = ebp;
        wv_call(cg, wmain_00488510, S, 0x004872b1u, 1, a);
    }

    if (wrd(gm, gsz, G_VCGATE) != 0u && count <= 0x40u) {    /* 004872b4: the colour-key rewrite */
        if ((int32_t)count > 0) {
            uint32_t eax = K_DST_REC;
            const uint32_t delta = src - K_DST_REC;          /* edx = esi - eax */
            uint32_t n = count;                              /* ebp */
            do {
                const gptr s = (uint32_t)(delta + eax);
                const gptr d = eax - 0x12u;
                uint32_t k;
                for (k = 0; k < 8u; k++)                     /* rep movsd, 8 dwords */
                    wwr(gm, gsz, d + 4u * k, wrd(gm, gsz, s + 4u * k));
                eax += 0x20u;
                for (k = 0; k < 3u; k++) {                   /* the three LUT bytes */
                    const gptr slot = eax - 0x22u + k;
                    wwr8(gm, gsz, slot, wrd8(gm, gsz, K_LUT + wrd8(gm, gsz, slot)));
                }
                n--;
            } while (n != 0u);
        }
        src = K_DST;                                         /* mov esi, 0x854e58 */
    }

    /* 00487313: the indirect call through the device vtable */
    {
        const uint32_t dev = wrd(gm, gsz, G_VTABLE);
        const uint32_t vt = wrd(gm, gsz, dev);
        const uint32_t fn = wrd(gm, gsz, vt + 0x70u);
        uint32_t ecx = flag == 0u ? 4u : 0u;
        gptr s;
        ecx |= 0x18u;
        /* the original pushes ecx, count, src, 0x1c4, 6, dev - so the argument order is the
         * reverse of the push order: dev lands at [esp+4] and the flag at [esp+0x18]. */
        a[0] = dev; a[1] = 6u; a[2] = 0x1c4u; a[3] = src; a[4] = count; a[5] = ecx;
        s = S - 4u * 7u;
        for (int i = 0; i < 6; i++) wwr(gm, gsz, s + 4u + 4u * (uint32_t)i, a[i]);
        wwr(gm, gsz, s, 0x00487339u);
        cg->r[ESP] = s;
        cpu_call(cg, fn);
        cg->r[ESP] = S;
    }

    cg->r[EAX] = wrd(gm, gsz, G_VTXCOUNT) + count - 2u;       /* lea eax, [edx+ebx-2] */
    wwr(gm, gsz, G_VTXCOUNT, cg->r[EAX]);
    /* the pops */
    cg->r[EDI] = wrd(gm, gsz, S + 0x0u);
    cg->r[ESI] = wrd(gm, gsz, S + 0x8u);
    cg->r[EBP] = wrd(gm, gsz, S + 0x4u);
    cg->r[EBX] = wrd(gm, gsz, S + 0xcu);
    cg->r[ESP] = E + 4u;
}
