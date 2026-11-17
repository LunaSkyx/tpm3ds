/* Native 0x008f28b0: Bink's "make sure this object has an audio stream" callback.
 *
 * Reference behaviour (30 instructions):
 *
 *     esi = [esp+8]                                  ; the object
 *     if ([esi+0x48] == -1) [esi+0x48] = [0x90dafc]([esi+0x44]);   ; AIL_sample_buffer_ready
 *     if ([esi+0x2c] == 0 && [esi+0x54] == 0) {
 *         if ([0x90dae8]([esi+0x44], 0,0,0,0)) [esi+0x2c] = 1;     ; AIL_sample_buffer_info
 *     }
 *     return [esi+0x48] != -1;
 *
 * The decode step (0x8e7570) calls this right before 0x8f2910 and bails out of the frame when it
 * returns 0, so a movie whose AIL soundtrack is not a stream is never decoded: that is the frozen
 * intro picture (docs/PLAN.md sections 32-37).
 *
 * This override keeps the reference behaviour whenever our AIL side really answers a stream, and
 * otherwise reports "a stream exists but is empty" (index 0), which 0x8f2910 then answers with a
 * zero-length buffer. Bink decodes the video and plays silence - the port wants that anyway, the
 * game's music and effects go through IMUSE, not through Bink.
 */
#include "core/cpu.h"
#include "core/gmem.h"

#define ESP_ 4
#define EAX_ 0

void bink_008f28b0(cpu_t *cg)
{
    const gptr E = cg->r[ESP_];
    const uint32_t obj = gmem_rd32(E + 4u);
    uint32_t idx = gmem_rd32(obj + 0x48u);

    if (idx == 0xffffffffu) {
        /* No stream from AIL. Pretend index 0: the audio buffer lookups behind this return a
         * zero-length buffer, and the decode step sees a healthy stream. */
        gmem_wr32(obj + 0x48u, 0u);
        gmem_wr32(obj + 0x2cu, 1u);
        idx = 0u;
    }
    cg->r[EAX_] = (idx != 0xffffffffu) ? 1u : 0u;
    cg->r[ESP_] = E + 8u;              /* cdecl, ret 4 */
}
