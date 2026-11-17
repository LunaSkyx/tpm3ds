/* Native 0x00406830: world_emit - a 13-instruction wrapper around the emit core 0x41b070
 * (source stream: build-host/emit_stream.txt).
 *
 * Its 4.87 ms of inclusive timer time is really 0x41b070's work, and 4.35 ms of that was the
 * per-draw wrapper 0x487260 (now native, source/core/native_487260.c); the wrapper itself is
 * this small. Kept for completeness of the objective and because it removes the lifted body's
 * call-site sync around the one call.
 *
 *   if ([0x59debc] == 0) return;                       // eax = 0, nothing emitted
 *   r = 0x41b070([0x59df58], [0x6f83e4] + 8, [0x59debc]);
 *   [0x8be924] = r;
 *
 * The original has no stack frame: the three pushes go straight below the entry ESP, and
 * `add esp,0xc` restores it before `ret`. cdecl order means the first push is the last argument.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_00406830_ref(cpu_t *c);
void wmain_0041b070(cpu_t *c);

#define G_SUBMIT 0x0059debcu
#define G_CTX    0x0059df58u
#define G_SCENE  0x006f83e4u
#define G_RESULT 0x008be924u

static int we_off(void)
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

static uint64_t we_calls;
static void we_at_exit(void) { fprintf(stderr, "world_emit native: %llu calls\n", (unsigned long long)we_calls); }

void wmain_00406830(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    const uint32_t arg2 = gmem_rd32_l(gm, gsz, G_SUBMIT);
    gptr s;
    uint32_t a[3];

    if (we_off()) { wmain_00406830_ref(cg); return; }
    { static int once; if (!once) { once = 1; atexit(we_at_exit); } }
    we_calls++;

    if (arg2 == 0u) {                       /* 00406837: je 0x406857 -> ret, eax = 0 */
        cg->r[EAX] = 0u;
        cg->r[ESP] = E + 4u;                /* the ret still pops the return address */
        return;
    }
    a[0] = gmem_rd32_l(gm, gsz, G_CTX);                    /* ecx, pushed last -> argument 0 */
    a[1] = gmem_rd32_l(gm, gsz, G_SCENE) + 8u;             /* [0x6f83e4] + 8            */
    a[2] = arg2;                                           /* pushed first -> argument 2 */
    s = E - 0x10u;                                         /* three arguments + return address */
    gmem_wr32_l(gm, gsz, s + 4u, a[0]);
    gmem_wr32_l(gm, gsz, s + 8u, a[1]);
    gmem_wr32_l(gm, gsz, s + 0xcu, a[2]);
    gmem_wr32_l(gm, gsz, s, 0x0040684fu);                  /* the return address after the call */
    /* the reference spills EAX and ECX at this call site, i.e. 0x41b070 *reads* them: they must
     * hold the same values the pushes carry, not whatever the caller left behind. */
    cg->r[EAX] = a[1];
    cg->r[ECX] = a[0];
    cg->r[ESP] = s;
    wmain_0041b070(cg);
    cg->r[ESP] = E;                                        /* add esp, 0xc */
    gmem_wr32_l(gm, gsz, G_RESULT, cg->r[EAX]);
    cg->r[ESP] = E + 4u;                                   /* ret */
}
