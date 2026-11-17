/* Port options that switch on modes the original game contains but never
 * enables. Each option names the guest variable and the instructions that read
 * it; the option is applied only if those instructions are byte-identical
 * (the US CD WMAIN.EXE, 1999-04-22), so another exe version is left alone and
 * the log says so.
 *
 * hz60: the game's 60 fps mode. Its frame limiter (FUN_00475b75) waits 1/30 s
 *   per frame, or 1/60 s when the flag is set; the fixed simulation step
 *   (FUN_004756fc) is 1/32 s, or 1/64 s with the flag. Nothing in the program
 *   writes the flag (no code or data reference besides these two reads).
 * nolimit: benchmarking/profiling only - the limiter's enable flag (initialised
 *   to 1, read once in FUN_00475b75) cleared, so frames run back to back. */
#include <string.h>
#include "core/game_options.h"
#include "core/gmem.h"
#include "pal/pal.h"

typedef struct { uint32_t va; uint8_t n; uint8_t bytes[8]; } code_check;
typedef struct {
    const char *name;
    uint32_t    var, value;
    code_check  reads[2];
} game_option;

static const game_option k_options[] = {
    {"hz60", 0x00882294u, 1u,
     {{0x00475737u, 7, {0x83, 0x3d, 0x94, 0x22, 0x88, 0x00, 0x00}},
      {0x00475b79u, 7, {0x83, 0x3d, 0x94, 0x22, 0x88, 0x00, 0x00}}}},
    {"nolimit", 0x004b7d78u, 0u,
     {{0x00475baeu, 7, {0x83, 0x3d, 0x78, 0x7d, 0x4b, 0x00, 0x00}}}},
};

static int matches(const code_check *k)
{
    const uint8_t *p = gmem_span(k->va, k->n, 0);
    return p && !memcmp(p, k->bytes, k->n);
}

int game_option_apply(const char *name)
{
    for (unsigned i = 0; i < sizeof k_options / sizeof k_options[0]; i++) {
        const game_option *o = &k_options[i];
        if (strcmp(o->name, name)) continue;
        for (unsigned k = 0; k < sizeof o->reads / sizeof o->reads[0]; k++)
            if (o->reads[k].n && !matches(&o->reads[k])) {
                pal_log("options: %s not applied: code at %08lx differs (other exe version?)", name,
                        (unsigned long)o->reads[k].va);
                return 0;
            }
        gmem_wr32(o->var, o->value);
        pal_log("options: %s on", name);
        return 1;
    }
    pal_log("options: unknown option %s", name);
    return 0;
}

