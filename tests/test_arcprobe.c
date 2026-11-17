/* Archive header probe vs. the original CD files (US release, 1999-04-20).
 * usage: test_arcprobe <game_files dir>   (SKIP if the files are absent) */
#include <stdio.h>
#include <string.h>
#include "core/arcprobe.h"

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int probe(const char *root, const char *rel, arc_info *a)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    int ok = arc_probe(p, a);
    char d[256];
    arc_describe(a, d, sizeof d);
    printf("  %-22s %s\n", rel, d);
    return ok;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "game_files";
    arc_info a;
    if (probe(root, "GAMEDATA/GOBS/LOCALIZE.LAB", &a) == 0 && a.kind == ARC_MISSING) {
        printf("SKIP test_arcprobe: no original files under %s\n", root);
        return 0;
    }
    CHECK(a.ok && a.kind == ARC_LAB && a.lab_version == 0x10000);
    CHECK(a.lab_entries == 13 && a.lab_names_size == 145 && a.file_size == 705209);
    CHECK(a.lab_max_end == a.file_size);
    CHECK(strcmp(a.lab_first_name, "INFO.TXT") == 0 && a.lab_first_off == 369 && a.lab_first_size == 4095);
    CHECK(a.lab_ntypes == 3);

    probe(root, "GAMEDATA/GOBS/VOICE.LAB", &a);
    CHECK(a.ok && a.lab_entries == 2624 && a.lab_names_size == 28864 && a.lab_max_end == 114199347);
    CHECK(strcmp(a.lab_first_name, "3GD6715.WV") == 0 && a.lab_ntypes == 1 && strcmp(a.lab_type[0], "BSND") == 0);

    probe(root, "GAMEDATA/GOBS/BIG.Z", &a);
    CHECK(a.ok && a.kind == ARC_IS3Z && a.z_files == 1 && strcmp(a.z_name, "big.lab") == 0);
    CHECK(a.z_orig == 120859357 && a.z_comp == 80977329 && a.z_data_off == 255);
    CHECK(a.z_data_off + a.z_comp == a.z_dir_off);
    CHECK(a.z_dcl_lit == 0 && a.z_dcl_dict == 6);

    /* negative cases must report, not crash */
    probe(root, "GAMEDATA/BIN/WMAIN.EXE", &a);
    CHECK(!a.ok && a.kind == ARC_UNKNOWN);
    probe(root, "GAMEDATA/GOBS/NOPE.LAB", &a);
    CHECK(!a.ok && a.kind == ARC_MISSING);

    printf("%s test_arcprobe: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail != 0;
}
