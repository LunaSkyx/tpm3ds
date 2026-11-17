#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/pe_load.h"
#include "pal/pal.h"

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define FAIL(...) do { snprintf(img->err, sizeof img->err, __VA_ARGS__); goto fail; } while (0)

static int load_imports(pe_image *img, uint32_t rva)
{
    gptr b = img->base;
    /* pass 0 counts, pass 1 fills */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t n = 0;
        for (gptr d = b + rva;; d += 20) {
            uint32_t oft = gmem_rd32(d), name = gmem_rd32(d + 12), ft = gmem_rd32(d + 16);
            if (!name && !ft) break;
            char dll[32];
            gmem_strcpy_out(dll, sizeof dll, b + name);
            for (gptr t = b + (oft ? oft : ft), iat = b + ft;; t += 4, iat += 4) {
                uint32_t v = gmem_rd32(t);
                if (!v) break;
                if (pass) {
                    pe_import *im = &img->imports[n];
                    memcpy(im->dll, dll, sizeof dll);
                    im->iat = iat;
                    im->by_ordinal = (v & 0x80000000u) != 0;
                    if (im->by_ordinal) { im->ordinal = (uint16_t)v; im->name[0] = 0; }
                    else { im->ordinal = gmem_rd16(b + v); gmem_strcpy_out(im->name, sizeof im->name, b + v + 2); }
                }
                if (++n > 100000) return 0;
            }
        }
        if (!pass) {
            img->nimports = n;
            img->imports = calloc(n ? n : 1, sizeof *img->imports);
            if (!img->imports) return 0;
        }
    }
    return 1;
}

static int load_exports(pe_image *img, uint32_t rva)
{
    gptr d = img->base + rva, b = img->base;
    uint32_t obase = gmem_rd32(d + 16), nfun = gmem_rd32(d + 20), nnames = gmem_rd32(d + 24);
    gptr funcs = b + gmem_rd32(d + 28), names = b + gmem_rd32(d + 32), ords = b + gmem_rd32(d + 36);
    if (nfun > 65536 || nnames > nfun) return 0;
    img->exports = calloc(nfun ? nfun : 1, sizeof *img->exports);
    if (!img->exports) return 0;
    for (uint32_t i = 0; i < nfun; i++) {
        uint32_t f = gmem_rd32(funcs + 4 * i);
        if (!f) continue;
        pe_export *e = &img->exports[img->nexports++];
        e->ordinal = (uint16_t)(obase + i);
        e->addr = b + f;
        for (uint32_t k = 0; k < nnames; k++)
            if (gmem_rd16(ords + 2 * k) == i) { gmem_strcpy_out(e->name, sizeof e->name, b + gmem_rd32(names + 4 * k)); break; }
    }
    return 1;
}

static int apply_relocs(pe_image *img, uint32_t rva, uint32_t size)
{
    uint32_t delta = img->base - img->pref_base;
    gptr p = img->base + rva, end = p + size;
    while (p + 8 <= end) {
        uint32_t page = gmem_rd32(p), bsize = gmem_rd32(p + 4);
        if (bsize < 8 || p + bsize > end) return 0;
        for (gptr e = p + 8; e < p + bsize; e += 2) {
            uint32_t v = gmem_rd16(e), type = v >> 12;
            if (type == 0) continue;                      /* padding */
            if (type != 3) return 0;                      /* only HIGHLOW in PE32 */
            gptr at = img->base + page + (v & 0xfff);
            gmem_wr32(at, gmem_rd32(at) + delta);
            img->nrelocs++;
        }
        p += bsize;
    }
    return 1;
}

int pe_load(const char *path, gptr load_base, pe_image *img)
{
    memset(img, 0, sizeof *img);
    uint32_t faults0 = g_gmem_faults;
    uint8_t h[4096];
    pal_file *f = pal_file_open(path);
    if (!f) FAIL("cannot open");
    size_t hn = pal_file_read_at(f, 0, h, sizeof h);
    if (hn < 0x40 || h[0] != 'M' || h[1] != 'Z') FAIL("no MZ header");
    uint32_t pe = rd32(h + 0x3c);
    if (pe > hn - 0xf8 || memcmp(h + pe, "PE\0\0", 4) != 0) FAIL("no PE header");
    if (rd16(h + pe + 4) != 0x14c) FAIL("not i386");
    uint32_t nsec = rd16(h + pe + 6), optsz = rd16(h + pe + 20);
    const uint8_t *opt = h + pe + 24;
    if (rd16(opt) != 0x10b) FAIL("not PE32");
    if (pe + 24 + optsz + 40 * nsec > hn) FAIL("section table past 4 KB");

    img->pref_base = rd32(opt + 28);
    img->base      = load_base ? load_base : img->pref_base;
    img->size      = rd32(opt + 56);
    img->entry     = img->base + rd32(opt + 16);
    uint32_t hdr_size = rd32(opt + 60), ndirs = rd32(opt + 92);
    const uint8_t *dirs = opt + 96;
    if (!gmem_ok(img->base, img->size)) FAIL("image %08lx+%lx outside arena", (unsigned long)img->base, (unsigned long)img->size);
    if (img->base & 0xfff) FAIL("base not page aligned");

    /* map headers and sections */
    memset(g_gmem + img->base, 0, img->size);
    memcpy(g_gmem + img->base, h, hdr_size < hn ? hdr_size : hn);
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *s = opt + optsz + 40 * i;
        uint32_t vsize = rd32(s + 8), va = rd32(s + 12), rsize = rd32(s + 16), raw = rd32(s + 20);
        uint32_t n = (vsize && vsize < rsize) ? vsize : rsize;
        if (!n) continue;
        if (va + n > img->size) FAIL("section %u past SizeOfImage", (unsigned)i);
        if (pal_file_read_at(f, raw, g_gmem + img->base + va, n) != n) FAIL("section %u short read", (unsigned)i);
    }
    pal_file_close(f);
    f = NULL;

    if (img->base != img->pref_base) {
        if (ndirs <= 5 || !rd32(dirs + 5 * 8)) FAIL("needs relocation but has no .reloc");
        if (!apply_relocs(img, rd32(dirs + 5 * 8), rd32(dirs + 5 * 8 + 4))) FAIL("bad relocation block");
    }
    if (ndirs > 1 && rd32(dirs + 8) && !load_imports(img, rd32(dirs + 8))) FAIL("bad import table");
    if (ndirs > 0 && rd32(dirs) && !load_exports(img, rd32(dirs))) FAIL("bad export table");
    if (g_gmem_faults != faults0) FAIL("tables point outside the arena");

    pal_log("pe: %s at %08lx (pref %08lx) size %lx entry %08lx: %lu imports, %lu exports, %lu relocs",
            path, (unsigned long)img->base, (unsigned long)img->pref_base, (unsigned long)img->size,
            (unsigned long)img->entry, (unsigned long)img->nimports, (unsigned long)img->nexports,
            (unsigned long)img->nrelocs);
    return 1;

fail:
    if (f) pal_file_close(f);
    pal_log("pe: %s: %s", path, img->err);
    free(img->imports); img->imports = NULL; img->nimports = 0;
    free(img->exports); img->exports = NULL; img->nexports = 0;
    return 0;
}

void pe_free(pe_image *img)
{
    free(img->imports);
    free(img->exports);
    img->imports = NULL;
    img->exports = NULL;
}

gptr pe_find_export(const pe_image *img, const char *name)
{
    for (uint32_t i = 0; i < img->nexports; i++)
        if (strcmp(img->exports[i].name, name) == 0) return img->exports[i].addr;
    return 0;
}
