#include <stdio.h>
#include <string.h>
#include "core/arcprobe.h"
#include "pal/pal.h"

#define IS3Z_MAGIC 0x8C655D13u

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define FAIL(...) do { snprintf(a->err, sizeof a->err, __VA_ARGS__); return 0; } while (0)

/* LAB: header {char magic[4]="LABN"; u32 version; u32 count; u32 names_size}
 * then count x {u32 name_off; u32 offset; u32 size; u32 fourcc (byte-reversed)}
 * then the name table. */
static int probe_lab(pal_file *f, const uint8_t *h, arc_info *a)
{
    a->kind           = ARC_LAB;
    a->lab_version    = rd32(h + 4);
    a->lab_entries    = rd32(h + 8);
    a->lab_names_size = rd32(h + 12);
    if (a->lab_version != 0x10000) FAIL("LAB version %#x", (unsigned)a->lab_version);
    if (a->lab_entries == 0 || a->lab_entries > (1u << 20)) FAIL("LAB count %u", (unsigned)a->lab_entries);

    const uint64_t names_at = 16 + 16ull * a->lab_entries;
    const uint64_t dir_end  = names_at + a->lab_names_size;
    if (dir_end > a->file_size) FAIL("LAB directory past EOF");

    uint8_t buf[16 * 256];
    for (uint32_t i = 0; i < a->lab_entries; i += 256) {
        uint32_t n = a->lab_entries - i < 256 ? a->lab_entries - i : 256;
        if (pal_file_read_at(f, 16 + 16ull * i, buf, 16u * n) != 16u * n) FAIL("LAB short read");
        for (uint32_t j = 0; j < n; j++) {
            const uint8_t *e = buf + 16 * j;
            uint32_t name_off = rd32(e), off = rd32(e + 4), size = rd32(e + 8);
            if (name_off >= a->lab_names_size) FAIL("LAB entry %u name_off", (unsigned)(i + j));
            if (off < dir_end || (uint64_t)off + size > a->file_size)
                FAIL("LAB entry %u range %u+%u", (unsigned)(i + j), (unsigned)off, (unsigned)size);
            if ((uint64_t)off + size > a->lab_max_end) a->lab_max_end = (uint64_t)off + size;
            if (i + j == 0) { a->lab_first_off = off; a->lab_first_size = size; }

            char t[5] = { (char)e[15], (char)e[14], (char)e[13], (char)e[12], 0 };
            int k = 0;
            while (k < a->lab_ntypes && memcmp(a->lab_type[k], t, 5) != 0) k++;
            if (k == a->lab_ntypes && k < ARC_MAX_TYPES) memcpy(a->lab_type[a->lab_ntypes++], t, 5);
            if (k < ARC_MAX_TYPES) a->lab_type_count[k]++;
        }
    }
    if (pal_file_read_at(f, 16, buf, 16) != 16) FAIL("LAB first entry");
    uint32_t first_name_off = rd32(buf);
    size_t got = pal_file_read_at(f, names_at + first_name_off, a->lab_first_name,
                                  sizeof a->lab_first_name - 1);
    a->lab_first_name[got] = 0;
    return a->ok = 1;
}

/* InstallShield 3 .Z: 255-byte header; layout below verified against the CD's
 * BIG.Z (a single-file archive). Multi-file archives are not needed. */
static int probe_is3z(pal_file *f, const uint8_t *h, arc_info *a)
{
    a->kind         = ARC_IS3Z;
    a->z_files      = rd16(h + 0x0c);
    a->z_arc_size   = rd32(h + 0x12);
    a->z_orig_total = rd32(h + 0x16);
    a->z_dir_off    = rd32(h + 0x29);
    a->z_ftab_off   = rd32(h + 0x33);
    if (a->z_arc_size != a->file_size) FAIL("IS3Z size %u != file %u", (unsigned)a->z_arc_size, (unsigned)a->file_size);
    if (a->z_files != 1) FAIL("IS3Z files=%u (expected 1)", (unsigned)a->z_files);

    uint8_t e[96];
    size_t n = pal_file_read_at(f, a->z_ftab_off, e, sizeof e);
    if (n < 31) FAIL("IS3Z file table short");
    a->z_orig     = rd32(e + 3);
    a->z_comp     = rd32(e + 7);
    a->z_data_off = rd32(e + 11);
    unsigned nlen = e[29];
    if (30 + nlen > n || nlen >= sizeof a->z_name) FAIL("IS3Z name len %u", nlen);
    memcpy(a->z_name, e + 30, nlen);
    a->z_name[nlen] = 0;
    if ((uint64_t)a->z_data_off + a->z_comp > a->file_size) FAIL("IS3Z payload past EOF");

    uint8_t dcl[2];
    if (pal_file_read_at(f, a->z_data_off, dcl, 2) != 2) FAIL("IS3Z payload read");
    a->z_dcl_lit = dcl[0];
    a->z_dcl_dict = dcl[1];
    if (dcl[0] > 1 || dcl[1] < 4 || dcl[1] > 6) FAIL("not PKWARE DCL (%02x %02x)", dcl[0], dcl[1]);
    return a->ok = 1;
}

int arc_probe(const char *path, arc_info *a)
{
    memset(a, 0, sizeof *a);
    pal_file *f = pal_file_open(path);
    if (!f) { a->kind = ARC_MISSING; snprintf(a->err, sizeof a->err, "not found"); return 0; }

    int64_t sz = pal_file_size(f);
    a->file_size = sz < 0 ? 0 : (uint64_t)sz;
    uint8_t h[256];
    size_t n = pal_file_read_at(f, 0, h, sizeof h);
    int ok = 0;
    a->kind = ARC_UNKNOWN;
    if (n >= 16 && memcmp(h, "LABN", 4) == 0)        ok = probe_lab(f, h, a);
    else if (n >= 0x37 && rd32(h) == IS3Z_MAGIC)      ok = probe_is3z(f, h, a);
    else snprintf(a->err, sizeof a->err, "unknown magic %02x %02x %02x %02x",
                  n > 0 ? h[0] : 0, n > 1 ? h[1] : 0, n > 2 ? h[2] : 0, n > 3 ? h[3] : 0);
    pal_file_close(f);
    return ok;
}

void arc_describe(const arc_info *a, char *buf, size_t n)
{
    if (!a->ok) {
        static const char *k[] = { "MISSING", "UNKNOWN", "LAB", "IS3Z" };
        snprintf(buf, n, "%s: %s", k[a->kind], a->err);
        return;
    }
    if (a->kind == ARC_LAB) {
        int w = snprintf(buf, n, "LABN v%x %u entries, names %u B, %llu B, first '%s' @%u+%u, types",
                         (unsigned)a->lab_version, (unsigned)a->lab_entries, (unsigned)a->lab_names_size,
                         (unsigned long long)a->file_size, a->lab_first_name,
                         (unsigned)a->lab_first_off, (unsigned)a->lab_first_size);
        for (int i = 0; i < a->lab_ntypes && w > 0 && (size_t)w < n; i++)
            w += snprintf(buf + w, n - w, " %s:%u", a->lab_type[i], (unsigned)a->lab_type_count[i]);
    } else {
        snprintf(buf, n, "IS3Z '%s' orig %u B, comp %u B @%u, DCL lit=%u dict=%u",
                 a->z_name, (unsigned)a->z_orig, (unsigned)a->z_comp, (unsigned)a->z_data_off,
                 a->z_dcl_lit, a->z_dcl_dict);
    }
}
