/* Native 0x004616cc: the game's software blitter for run-length-compressed 16-bit sprites (the menu's
 * background and tiles are drawn with it straight into the locked back buffer: 22 % of a menu frame as
 * lifted code, measured with the sampler, docs/PLAN.md 46f).
 *
 * cdecl void f(int x, int y, sprite *s, surface *d, int keep):
 *   s+0xc width, s+0x10 height, s+0x5c -> block { int count; ... uint16 stream at +4 }
 *   d+0x5c pixels, d+0x18 pitch (bytes, used for the first row only), d+0x1c row advance (uint16s), d+0x24 bpp
 *   globals 0x6cfd58 / 0x6cfd5c: x / y origin added to (x, y)
 * The stream is a list of runs per row: token & 0xfff = length, token & 0xf000 == 0xf000 = skip run (written as
 * zeros unless keep != 0), otherwise `length` pixels follow. Rows end when the runs add up to the width; the
 * loop ends when `count - 2` stream words are consumed.
 *
 * Only the fully on-screen case is done here (no clipping); everything else - clipped sprites, a
 * malformed stream, memory that gmem_span refuses - runs the lifted body (wmain_004616cc_ref), which
 * redraws the whole sprite: the writes are idempotent, so falling back after a partial draw is safe.
 * Callee-saved registers are untouched; EAX/ECX/EDX/flags are free in cdecl.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_004616cc_ref(cpu_t *c);

static int rb_off(void)
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

void wmain_004616cc(cpu_t *cg)
{
    const gptr E = cg->r[ESP];
    const int32_t x = (int32_t)gmem_rd32(E + 4u), y = (int32_t)gmem_rd32(E + 8u);
    const gptr spr = gmem_rd32(E + 12u), dst = gmem_rd32(E + 16u);
    const uint32_t keep = gmem_rd32(E + 20u);
    if (rb_off()) { wmain_004616cc_ref(cg); return; }
    const int32_t w = (int32_t)gmem_rd32(spr + 0xcu), h = (int32_t)gmem_rd32(spr + 0x10u);
    if (!(x < 0x280 && 0 < x + w && y < 0x1e0 && 0 < y + h)) { cg->r[ESP] = E + 4u; return; }      /* nothing visible */
    if (x < 0 || y < 0 || x - 1 + w > 0x27f || y - 1 + h > 0x1df || w <= 0) { wmain_004616cc_ref(cg); return; }   /* clipped */

    const gptr block = gmem_rd32(spr + 0x5cu);
    int32_t rem = (int32_t)gmem_rd32(block) - 2;
    if (rem <= 0) { cg->r[ESP] = E + 4u; return; }
    const uint16_t *sp = (const uint16_t *)gmem_span(block + 4u, ((uint32_t)rem + 2u) * 2u, 0);
    const uint16_t *const send = sp + rem + 2;
    const gptr pix = gmem_rd32(dst + 0x5cu);
    const uint32_t pitch = gmem_rd32(dst + 0x18u), adv = gmem_rd32(dst + 0x1cu), bpp8 = gmem_rd32(dst + 0x24u) >> 3;
    gptr row = pix + (uint32_t)(y + (int32_t)gmem_rd32(0x006cfd5cu)) * pitch + (uint32_t)(x + (int32_t)gmem_rd32(0x006cfd58u)) * bpp8;
    if (!sp || bpp8 != 2u) { wmain_004616cc_ref(cg); return; }

    while (rem > 0) {
        uint16_t *dp = (uint16_t *)gmem_span(row, (uint32_t)w * 2u, 1);
        int32_t col = w;
        if (!dp) { wmain_004616cc_ref(cg); return; }
        while (col > 0) {
            if (sp >= send) { wmain_004616cc_ref(cg); return; }
            const uint32_t tok = *sp, run = tok & 0xfffu;
            if ((int32_t)run > col) { wmain_004616cc_ref(cg); return; }
            if ((tok & 0xf000u) == 0xf000u) {
                if (!keep) memset(dp, 0, run * 2u);
                sp += 1;
                rem -= 1;
            } else {
                if (sp + 1 + run > send) { wmain_004616cc_ref(cg); return; }
                if (sp + 1 != dp) memcpy(dp, sp + 1, run * 2u);
                sp += run + 1u;
                rem -= 1 + (int32_t)run;
            }
            dp += run;
            col -= (int32_t)run;
        }
        row += adv * 2u;
    }
    cg->r[ESP] = E + 4u;
}
