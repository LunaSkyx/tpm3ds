/* Native 0x008f2910: Bink's "audio buffer for this frame" lookup.
 *
 * Reference behaviour (14 instructions):
 *
 *     eax = [esp+4]              ; the audio object
 *     ecx = [eax+0x48]           ; the stream index kept by 0x8f28b0
 *     if (ecx == -1) return 0;   ; "this stream has no soundtrack"
 *     [[esp+8]]  = [eax + ecx*4 + 0x3c];
 *     [[esp+0xc]] = [eax + 0x4c];
 *     return 1;
 *
 * The decode step (0x8e7570) calls this and treats 0 as "abort this frame", so a movie whose AIL
 * soundtrack never becomes a stream is decoded zero times and the picture freezes on whatever was on
 * screen - exactly what the intro does (docs/PLAN.md sections 32-37). Making our AIL side report a
 * working stream instead only moved the failure into Bink's audio pacing, which hung the game.
 *
 * So: identical answers to the reference, except that "no soundtrack" reports an empty buffer and
 * success instead of failure. Bink then decodes the video and plays silence, which is what this port
 * wants anyway - the game's music and effects go through IMUSE, not through Bink.
 */
#include "core/cpu.h"
#include "core/gmem.h"

void bink_008f2910(cpu_t *cg)
{
    const gptr E = cg->r[ESP];
    const uint32_t obj = gmem_rd32(E + 4u);
    const uint32_t idx = gmem_rd32(obj + 0x48u);
    const uint32_t pa = gmem_rd32(E + 8u);
    const uint32_t pb = gmem_rd32(E + 12u);

    if (idx != 0xffffffffu) {
        if (pa) gmem_wr32(pa, gmem_rd32(obj + 0x3cu + idx * 4u));
        if (pb) gmem_wr32(pb, gmem_rd32(obj + 0x4cu));
    } else {
        if (pa) gmem_wr32(pa, 0u);     /* no soundtrack: an empty buffer, but not a failure */
        if (pb) gmem_wr32(pb, 0u);
    }
    cg->r[EAX] = 1u;
    cg->r[ESP] = E + 16u;              /* cdecl, ret 0xc */
}