/* Native 0x00488210: the render-state diff (__cdecl f(uint flags) -> old flags; ~600-1300 calls/frame).
 *
 * Transcribed from the x86 listing (Ghidra FUN_00488210). It compares the new flag word (param | 0x8000) with the one
 * stored at [0x855964] and issues IDirect3DDevice3::SetRenderState(state, value) through the device vtable (+0x58) for
 * every group that changed, then stores the new word and returns the old one in EAX. The lifted body spends ~3 us per
 * call, most of it in register sync and in the thunk dispatch of each SetRenderState. Here, when the vtable entry is the
 * HLE SetRenderState thunk, the call goes straight to r3d_set_render_state (what dv_SetRenderState does); the HLE tick
 * (watchdog + guest thread preemption) is still advanced per call. Any other vtable entry is called as a guest call.
 *
 * Contract: EBX/EBP/ESI/EDI untouched (the original saves and restores them), EAX = old word, ESP = entry + 4, ECX/EDX and
 * the flags are caller-saved under cdecl and not preserved. Host check: TPM_VERIFY_SRS=1 runs native and lifted reference
 * on the same input and compares the (state,value) sequence, EAX, ESP and [0x855964]. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "core/cpu.h"
#include "core/gmem.h"
#include "core/hle/hle.h"
#include "core/render/r3d.h"

void wmain_00488210_ref(cpu_t *c);
void wmain_0048aaf9(cpu_t *c);
extern int g_native_off;
extern void (*g_srs_rec)(uint32_t state, uint32_t value);

#define G_FLAGS  0x00855964u
#define G_DEV    0x006fc984u

typedef struct {
    cpu_t   *cg;
    gptr     E;          /* entry ESP */
    uint32_t dev, fn;
} srs_ctx;

static uint32_t g_srs_thunk;

static void srs(srs_ctx *x, uint32_t st, uint32_t v)
{
    cpu_t *cg = x->cg;
    if (x->fn == g_srs_thunk) {
        if (g_srs_rec) g_srs_rec(st, v);
        r3d_set_render_state(st, v);
        cg->r[ESP] = x->E - 0x30u;                 /* a consistent frame below ours for a possible guest thread switch */
        hle_fast_tick(cg);
        return;
    }
    {   /* some other target: a real guest call (stdcall, three arguments) */
        const gptr s = x->E - 0x30u;
        gmem_wr32(s, 0x0048824fu);
        gmem_wr32(s + 4u, x->dev);
        gmem_wr32(s + 8u, st);
        gmem_wr32(s + 12u, v);
        cg->r[ESP] = s;
        cpu_call(cg, x->fn);
    }
}

static void srs_native(cpu_t *cg)
{
    const gptr E = cg->r[ESP];
    const uint32_t old = gmem_rd32(G_FLAGS);
    const uint32_t p = gmem_rd32(E + 4u);
    const uint32_t b = p | 0x8000u;
    const uint32_t d = (b ^ old) & 0xfffffe4cu;
    srs_ctx x;

    if (d) {
        x.cg = cg; x.E = E;
        x.dev = gmem_rd32(G_DEV);
        x.fn = gmem_rd32(gmem_rd32(x.dev) + 0x58u);
        if (!g_srs_thunk) g_srs_thunk = hle_thunk_for("COM", "IDirect3DDevice3::SetRenderState");

        if (d & 0x2000u)   srs(&x, 3, (b & 0x2000u) ? 3u : 1u);
        if (d & 0x100000u) srs(&x, 0x2f, (b & 0x100000u) ? 1u : 0u);
        if (d & 0x8000u)   srs(&x, 0xb, (b & 0x8000u) ? 0u : 1u);
        if (d & 0xf0600u) {
            const uint32_t edi = b & 0x200u;
            if (!edi) {
                srs(&x, 0x1b, 0);
            } else {
                const uint32_t m = b & gmem_rd32(0x00855958u);
                if (m == 0x30000u)      { srs(&x, 0x13, 5); srs(&x, 0x14, 5); }
                else if (m == 0x40000u) { srs(&x, 0x13, 5); srs(&x, 0x14, 2); }
                else if (m == 0x20000u) { srs(&x, 0x13, 2); srs(&x, 0x14, 2); }
                else                    { srs(&x, 0x13, m == 0x10000u ? 2u : 5u); srs(&x, 0x14, 6); }
                srs(&x, 0x1b, 1);
            }
            if (!(b & 0x80400u)) {
                srs(&x, 0x15, 2);
            } else {
                const uint32_t esi = b & 0x80000u;
                if (!esi && !gmem_rd32(0x0089fde8u)) {
                    srs(&x, 0x15, 3);
                } else {
                    srs(&x, 0x15, 4);
                    if (gmem_rd32(0x0089fde8u) != 0 && esi && !edi) {
                        srs(&x, 0x13, 5); srs(&x, 0x14, 6); srs(&x, 0x1b, 1);
                    }
                }
            }
        }
        if (d & 0x1800u) {
            if (b & 0x800u) {
                const gptr s = E - 0x30u;
                gmem_wr32(s, 0x0048842eu);
                gmem_wr32(s + 4u, gmem_rd32(0x00866fc8u));
                cg->r[ESP] = s;
                wmain_0048aaf9(cg);
                srs(&x, 0x17, cg->r[EAX]);
            } else {
                srs(&x, 0x17, 8);
            }
            srs(&x, 0xe, (b & 0x1000u) ? 1u : 0u);
        }
        if (d & 0x40u) {
            if (b & 0x40u) {
                srs(&x, 0x1c, 1);
                srs(&x, 0x22, (((gmem_rd32(0x006fc988u) << 8) | gmem_rd32(0x00854e50u)) << 8) | gmem_rd32(0x006fc980u));
                srs(&x, 0x23, 3);
                srs(&x, 0x24, gmem_rd32(0x00866fa8u));
                srs(&x, 0x25, gmem_rd32(0x00866fa4u));
            } else {
                srs(&x, 0x1c, 0);
            }
        }
    }
    gmem_wr32(G_FLAGS, b);
    cg->r[EAX] = old;
    cg->r[ESP] = E + 4u;
}

#ifndef __3DS__
static int sv_verify = -1;
static uint64_t sv_checked, sv_differed;
static uint32_t sv_rec[2][1024][2];
static int sv_n[2], sv_cur;
static void sv_hook(uint32_t s, uint32_t v)
{
    if (sv_n[sv_cur] < 1024) { sv_rec[sv_cur][sv_n[sv_cur]][0] = s; sv_rec[sv_cur][sv_n[sv_cur]][1] = v; }
    sv_n[sv_cur]++;
}
static void sv_at_exit(void)
{
    fprintf(stderr, "render-state diff verify: %llu calls checked, %llu differed\n",
            (unsigned long long)sv_checked, (unsigned long long)sv_differed);
}

static void srs_shadow(cpu_t *cg)
{
    const cpu_t c0 = *cg;
    const uint32_t old = gmem_rd32(G_FLAGS);
    const uint32_t param = gmem_rd32(c0.r[ESP] + 4u);
    cpu_t cn;
    uint32_t fl_n;
    int bad = 0, i;

    g_srs_rec = sv_hook;
    sv_n[0] = sv_n[1] = 0;
    sv_cur = 0;
    srs_native(cg);
    cn = *cg;
    fl_n = gmem_rd32(G_FLAGS);
    gmem_wr32(G_FLAGS, old);
    *cg = c0;
    sv_cur = 1;
    wmain_00488210_ref(cg);
    g_srs_rec = NULL;
    if (sv_n[0] != sv_n[1]) bad = 1;
    else for (i = 0; i < sv_n[0] && i < 1024; i++)
        if (sv_rec[0][i][0] != sv_rec[1][i][0] || sv_rec[0][i][1] != sv_rec[1][i][1]) { bad = 1; break; }
    if (cn.r[EAX] != cg->r[EAX] || cn.r[EBX] != cg->r[EBX] || cn.r[ESI] != cg->r[ESI] || cn.r[EDI] != cg->r[EDI] ||
        cn.r[EBP] != cg->r[EBP] || cn.r[ESP] != cg->r[ESP] || fl_n != gmem_rd32(G_FLAGS)) bad = 1;
    if (bad && sv_differed < 6) {
        fprintf(stderr, "srs verify: DIFF param=%08x old=%08x n=%d/%d eax %08x/%08x esp %08x/%08x flags %08x/%08x\n",
                param, old, sv_n[0], sv_n[1], cn.r[EAX], cg->r[EAX], cn.r[ESP], cg->r[ESP], fl_n, gmem_rd32(G_FLAGS));
        for (i = 0; i < sv_n[0] && i < 12; i++) fprintf(stderr, "   native (%x,%x)\n", sv_rec[0][i][0], sv_rec[0][i][1]);
        for (i = 0; i < sv_n[1] && i < 12; i++) fprintf(stderr, "   ref    (%x,%x)\n", sv_rec[1][i][0], sv_rec[1][i][1]);
    }
    sv_checked++;
    if (bad) sv_differed++;
}
#endif

void wmain_00488210(cpu_t *cg)
{
#ifndef __3DS__
    if (sv_verify < 0) {
        const char *e = getenv("TPM_VERIFY_SRS");
        sv_verify = (e && *e == '1');
        if (sv_verify) atexit(sv_at_exit);
    }
    if (getenv("TPM_AX_OFF")) { wmain_00488210_ref(cg); return; }
    if (sv_verify) { srs_shadow(cg); return; }
#endif
    if (g_native_off) { wmain_00488210_ref(cg); return; }
    srs_native(cg);
}
