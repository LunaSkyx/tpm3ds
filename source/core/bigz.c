#include <stdio.h>
#include <string.h>
#include "core/arcprobe.h"
#include "core/bigz.h"
#include "core/dcl.h"
#include "pal/pal.h"

#define IN_CHUNK (32 * 1024)

typedef struct {
    pal_file      *src;
    uint64_t       pos, end;         /* compressed byte range in src */
    uint8_t        inbuf[IN_CHUNK];
    pal_wfile     *dst;
    uint32_t       out_bytes, out_limit;
    int            write_failed, read_failed, aborted;
    bigz_progress *prog;
} unpack_ctx;

static size_t in_cb(void *p, const uint8_t **buf)
{
    unpack_ctx *c = p;
    if (c->pos >= c->end) return 0;
    size_t n = c->end - c->pos < IN_CHUNK ? (size_t)(c->end - c->pos) : IN_CHUNK;
    size_t got = pal_file_read_at(c->src, c->pos, c->inbuf, n);
    if (got == 0) { c->read_failed = 1; return 0; }
    c->pos += got;
    *buf = c->inbuf;
    return got;
}

static int out_cb(void *p, const uint8_t *data, size_t n)
{
    unpack_ctx *c = p;
    if (c->out_bytes + n > c->out_limit) return 1;           /* more than the header says */
    if (!pal_wfile_write(c->dst, data, n)) { c->write_failed = 1; return 1; }
    c->out_bytes += (uint32_t)n;
    if (c->prog) {
        c->prog->done_bytes = c->out_bytes;
        if (c->prog->should_abort && c->prog->should_abort()) { c->aborted = 1; return 1; }
    }
    return 0;
}

#define FAIL(code, ...) do { snprintf(err, err_size, __VA_ARGS__); rc = (code); goto out; } while (0)

int bigz_unpack(const char *src_z, const char *dst_lab, bigz_progress *prog,
                char *err, unsigned err_size)
{
    static unpack_ctx c;             /* 32 KB buffer: keep it off the thread stack */
    char tmp[512];
    int rc = 0;
    arc_info z;

    err[0] = 0;
    memset(&c, 0, sizeof c);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst_lab);

    if (!arc_probe(src_z, &z) || z.kind != ARC_IS3Z) {
        snprintf(err, err_size, "source: %s", z.err[0] ? z.err : "not an IS3 .Z file");
        return -1;
    }
    if (prog) { prog->done_bytes = 0; prog->total_bytes = z.z_orig; }

    c.src = pal_file_open(src_z);
    c.dst = pal_wfile_create(tmp);
    c.pos = z.z_data_off;
    c.end = (uint64_t)z.z_data_off + z.z_comp;
    c.out_limit = z.z_orig;
    c.prog = prog;
    if (!c.src) FAIL(-2, "cannot reopen %s", src_z);
    if (!c.dst) FAIL(-3, "cannot create %s", tmp);

    size_t used = 0;
    int d = dcl_explode(in_cb, out_cb, &c, &used);
    if (c.aborted)       FAIL(-4, "aborted at %u of %u bytes", (unsigned)c.out_bytes, (unsigned)z.z_orig);
    if (c.write_failed)  FAIL(-5, "write failed at %u bytes (SD full?)", (unsigned)c.out_bytes);
    if (c.read_failed)   FAIL(-6, "read failed at offset %llu", (unsigned long long)c.pos);
    if (d != DCL_OK)     FAIL(-7, "DCL: %s at %u bytes out", dcl_strerror(d), (unsigned)c.out_bytes);
    if (c.out_bytes != z.z_orig) FAIL(-8, "size %u != expected %u", (unsigned)c.out_bytes, (unsigned)z.z_orig);
    if (used != z.z_comp) FAIL(-9, "consumed %u of %u compressed bytes", (unsigned)used, (unsigned)z.z_comp);

    int ok = pal_wfile_close(c.dst);
    c.dst = NULL;
    if (!ok) FAIL(-5, "write/flush failed on close (SD full?)");

    arc_info lab;
    if (!arc_probe(tmp, &lab) || lab.kind != ARC_LAB || lab.file_size != z.z_orig)
        FAIL(-10, "unpacked file invalid: %s", lab.err);
    if (!pal_file_rename(tmp, dst_lab)) FAIL(-11, "rename %s -> %s failed", tmp, dst_lab);

out:
    if (c.dst) pal_wfile_close(c.dst);
    if (c.src) pal_file_close(c.src);
    if (rc != 0) pal_file_remove(tmp);
    return rc;
}
