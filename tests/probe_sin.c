/* Probe for the CRT sin (guest 0x494272, lifted reference only).
 * Calls the lifted function with edge-case float arguments and logs every call
 * before it runs, so a crash (e.g. a Windows SEH exception from _fpieee_flt at
 * 0x49da7b) is attributed to the last logged input.
 * Not part of the test suite. Build: see docs/PERF_PLAN.md section 8. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "core/gheap.h"
#include "core/guest.h"
#include "core/gmem.h"
#include "gen/wmain_gen.h"

#define STACK_SZ 0x10000u

static void run_one(uint32_t esp, uint32_t bits, uint16_t fcw, uint16_t fsw_in)
{
    cpu_t c;
    float x;
    memcpy(&x, &bits, 4);
    memset(&c, 0, sizeof c);
    c.r[ESP] = esp;
    c.fcw = fcw;
    c.fsw = fsw_in;
    c.ftop = 0;
    gmem_wr32(esp, 0x00404040u);          /* return address */
    gmem_wr32(esp + 4u, bits);            /* float argument */
    printf("call bits=%08x x=%.9g fcw=%04x fsw=%04x ... ", bits, (double)x, fcw, fsw_in);
    fflush(stdout);
    wmain_00494272(&c);
    {
        double r = c.st[c.ftop & 7u];
        printf("ok -> st0=%.17g fsw=%04x fcw=%04x ftop=%u esp=%08x\n",
               r, c.fsw, c.fcw, c.ftop, c.r[ESP]);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/GAMEDATA", argc > 1 ? argv[1] : "game_files");
    guest_config cfg = {0};
    cfg.data_dir = dir;
    cfg.save_dir = "build-host/save-probe";
    cfg.max_ms = 5000;
    if (!guest_init(&cfg)) { printf("SKIP: WMAIN.EXE not found under %s\n", dir); return 0; }

    uint32_t stack = gheap_alloc(STACK_SZ);
    uint32_t esp = stack + 0x8000u;
    uint32_t vals[] = {
        0x00000000u, 0x80000000u,                 /* +0, -0 */
        0x3f800000u, 0xbf800000u,                 /* 1, -1 */
        0x42b40000u,                              /* 90 */
        0x47c35000u,                              /* 100000 */
        0x4b189680u,                              /* 1e7 */
        0x7f7fffffu,                              /* FLT_MAX */
        0x00000001u,                              /* smallest denormal */
        0x7f800000u, 0xff800000u,                 /* +inf, -inf */
        0x7fc00000u, 0xffc00000u,                 /* qNaN, -qNaN */
        0x7f800001u,                              /* sNaN */
    };
    uint16_t fcws[] = { 0x037fu, 0x037eu };
    for (unsigned f = 0; f < 2; f++)
        for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++)
            run_one(esp, vals[i], fcws[f], 0);
    guest_fini();
    printf("probe done\n");
    return 0;
}
