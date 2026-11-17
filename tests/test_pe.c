/* Guest arena + PE loader against the original binaries.
 * usage: test_pe <game_files dir>   (SKIP if absent) */
#include <stdio.h>
#include <string.h>
#include "core/gmem.h"
#include "core/pe_load.h"

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t count_dll(const pe_image *im, const char *dll)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < im->nimports; i++) n += strcmp(im->imports[i].dll, dll) == 0;
    return n;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "game_files";
    char exe[1024], dll[1024];
    snprintf(exe, sizeof exe, "%s/GAMEDATA/BIN/WMAIN.EXE", root);
    snprintf(dll, sizeof dll, "%s/GAMEDATA/BIN/IMUSE.DLL", root);
    FILE *t = fopen(exe, "rb");
    if (!t) { printf("SKIP test_pe: %s not found\n", exe); return 0; }
    fclose(t);

    /* arena helpers: alignment-free access, null guard, bounds */
    CHECK(gmem_init(48u << 20));
    gmem_wr32(0x20001, 0x11223344);
    CHECK(gmem_rd32(0x20001) == 0x11223344 && gmem_rd8(0x20001) == 0x44 && gmem_rd16(0x20003) == 0x1122);
    CHECK(g_gmem_faults == 0);
    CHECK(gmem_rd32(0x10) == 0 && g_gmem_faults == 1);                 /* null guard */
    gmem_wr32(g_gmem_size - 2, 1);
    CHECK(g_gmem_faults == 2);                                         /* straddles end */
    CHECK(gmem_rd32(0xfffffffeu) == 0 && g_gmem_faults == 3);         /* wraparound */
    g_gmem_faults = 0;

    /* WMAIN.EXE at its preferred base */
    pe_image w;
    CHECK(pe_load(exe, 0, &w));
    CHECK(w.base == 0x400000 && w.entry == 0x49cb80 && w.nrelocs == 0);
    CHECK(w.size == 0x4d0000);
    printf("  WMAIN: base %08x size %x entry %08x imports %u\n",
           (unsigned)w.base, (unsigned)w.size, (unsigned)w.entry, (unsigned)w.nimports);
    CHECK(w.nimports == 206);   /* 2+40+12+80+29+15+4+3+9+1+1+10 */
    CHECK(count_dll(&w, "DDRAW.dll") == 2 && count_dll(&w, "mss32.dll") == 40 &&
          count_dll(&w, "binkw32.dll") == 12 && count_dll(&w, "KERNEL32.dll") == 80 &&
          count_dll(&w, "USER32.dll") == 29 && count_dll(&w, "iMUSE.DLL") == 10);
    int dsound_ord1 = 0;
    for (uint32_t i = 0; i < w.nimports; i++)
        if (!strcmp(w.imports[i].dll, "DSOUND.dll") && w.imports[i].by_ordinal && w.imports[i].ordinal == 1) dsound_ord1 = 1;
    CHECK(dsound_ord1);
    /* the IID table found by the byte scan must be where the loader put it */
    static const uint8_t iid_d3d3[16] = { 0x40,0x32,0x22,0xbb,0x2b,0xe7,0xd0,0x11,0xa9,0xb4,0x00,0xaa,0x00,0xc0,0x99,0x3e };
    const uint8_t *p = gmem_span(0x4a9230, 16, 0);
    CHECK(p && memcmp(p, iid_d3d3, 16) == 0);

    /* IMUSE.DLL relocated from 0x10000000 into the arena, after WMAIN */
    pe_image m;
    const gptr mbase = 0x01000000;
    CHECK(pe_load(dll, mbase, &m));
    printf("  IMUSE: pref %08x -> %08x, %u relocs, %u exports\n",
           (unsigned)m.pref_base, (unsigned)m.base, (unsigned)m.nrelocs, (unsigned)m.nexports);
    CHECK(m.pref_base == 0x10000000 && m.base == mbase && m.nrelocs > 0);
    CHECK(m.entry == mbase + 0xab50);

    /* every WMAIN import from iMUSE.DLL resolves to code inside IMUSE's .text */
    uint32_t resolved = 0;
    for (uint32_t i = 0; i < w.nimports; i++) {
        if (strcmp(w.imports[i].dll, "iMUSE.DLL")) continue;
        gptr a = pe_find_export(&m, w.imports[i].name);
        if (a >= mbase + 0x1000 && a < mbase + 0x1000 + 0x112bc) resolved++;
        else printf("  unresolved iMUSE import %s -> %08x\n", w.imports[i].name, (unsigned)a);
    }
    CHECK(resolved == 10);
    CHECK(g_gmem_faults == 0);

    pe_free(&w);
    pe_free(&m);
    gmem_fini();
    printf("%s test_pe: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail != 0;
}
