/* BIG.Z -> big.lab unpack against the original CD file.
 * usage: test_bigz <game_files dir>   (SKIP if BIG.Z is absent)
 * Writes only into build-host/. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "core/arcprobe.h"
#include "core/bigz.h"
#include "pal/pal.h"

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static bigz_progress g_prog;
static int abort_halfway(void) { return g_prog.done_bytes >= g_prog.total_bytes / 2; }

static int exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "game_files";
    const char *dst = "build-host/big.lab";
    char src[1024], err[160];
    snprintf(src, sizeof src, "%s/GAMEDATA/GOBS/BIG.Z", root);
    if (!exists(src)) { printf("SKIP test_bigz: %s not found\n", src); return 0; }
    remove(dst);

    /* 1. abort halfway: error, and neither big.lab nor big.lab.tmp remains */
    g_prog.should_abort = abort_halfway;
    int rc = bigz_unpack(src, dst, &g_prog, err, sizeof err);
    printf("  abort run: rc=%d '%s'\n", rc, err);
    CHECK(rc == -4);
    CHECK(!exists(dst) && !exists("build-host/big.lab.tmp"));

    /* 2. full unpack */
    g_prog.should_abort = NULL;
    clock_t t0 = clock();
    rc = bigz_unpack(src, dst, &g_prog, err, sizeof err);
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("  full run: rc=%d '%s' %u bytes in %.2f s (%.1f MB/s)\n", rc, err,
           (unsigned)g_prog.done_bytes, s, g_prog.done_bytes / 1e6 / (s > 0 ? s : 1));
    CHECK(rc == 0);
    CHECK(g_prog.done_bytes == 120859357u && g_prog.total_bytes == 120859357u);

    arc_info a;
    char d[256];
    arc_probe(dst, &a);
    arc_describe(&a, d, sizeof d);
    printf("  big.lab: %s\n", d);
    CHECK(a.ok && a.kind == ARC_LAB && a.file_size == 120859357u && a.lab_max_end <= a.file_size);
    CHECK(!exists("build-host/big.lab.tmp"));

    printf("%s test_bigz: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail != 0;
}
