/* Tests for the DCL (PKWare Compression Library) decompressor.
 *
 * Build and run:
 *   gcc -std=gnu11 -Wall -Wextra -O2 -Isource -o test_dcl tests/test_dcl.c source/core/dcl.c
 * Exit status 0 on success.
 *
 * All non-fuzz vectors were produced independently of dcl.c and verified with
 * the reference decoder in zlib/contrib/blast/blast.c (Mark Adler).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/dcl.h"

#define MAX_N    4096u           /* largest chunk the output callback allows */
#define OUT_CAP  (1u << 20)      /* fuzz output cap: 1 MiB */

static int g_checks, g_pass, g_fail;

#define CHECK(cond) do {                                                    \
        g_checks++;                                                         \
        if (cond) g_pass++;                                                 \
        else {                                                              \
            g_fail++;                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                   \
    } while (0)

/* One shared context, as the dcl.h interface requires. */
typedef struct {
    /* input */
    const uint8_t *data;
    size_t len;
    size_t pos;
    size_t chunk;               /* 0 = hand out all remaining bytes at once */
    int in_calls;

    /* output */
    uint8_t *dst;               /* optional copy destination */
    size_t dst_cap;
    size_t out_len;
    size_t max_n;               /* largest n ever received */
    int out_calls;
    int fail_on_call;           /* 1-based call that aborts, 0 = never */
    int oversize;               /* received n > 4096 */
    int aborted;
    int after_abort;            /* output calls made after an abort */
    size_t sizes[8];            /* sizes of the first calls */
    int nsizes;
    size_t limit;               /* total output cap, 0 = none */
} stream_ctx;

static size_t in_cb(void *ctx, const uint8_t **buf)
{
    stream_ctx *c = (stream_ctx *)ctx;
    size_t n;

    if (c->pos >= c->len) return 0;
    n = c->len - c->pos;
    if (c->chunk != 0 && n > c->chunk) n = c->chunk;
    *buf = c->data + c->pos;
    c->pos += n;
    c->in_calls++;
    return n;
}

static int out_cb(void *ctx, const uint8_t *data, size_t n)
{
    stream_ctx *c = (stream_ctx *)ctx;

    c->out_calls++;
    if (c->aborted) {           /* must never happen: decoder keeps going */
        c->after_abort++;
        return 1;
    }
    if (n > c->max_n) c->max_n = n;
    if (n > MAX_N) {
        c->oversize = 1;
        return 1;
    }
    if (c->nsizes < (int)(sizeof c->sizes / sizeof c->sizes[0]))
        c->sizes[c->nsizes++] = n;
    if (c->fail_on_call != 0 && c->out_calls == c->fail_on_call) {
        c->aborted = 1;
        return 1;
    }
    if (c->limit != 0 && c->out_len + n > c->limit) {
        c->aborted = 1;
        return 1;
    }
    if (c->dst != NULL && c->out_len + n <= c->dst_cap)
        memcpy(c->dst + c->out_len, data, n);
    c->out_len += n;
    return 0;
}

/* Run one stream; returns the dcl_explode() result.  *in_used is only
 * meaningful for DCL_OK. */
static int run(stream_ctx *c, const uint8_t *data, size_t len, size_t chunk,
               uint8_t *dst, size_t cap, size_t limit, int fail_on_call,
               size_t *in_used)
{
    size_t used = 0;
    int ret;

    memset(c, 0, sizeof *c);
    c->data = data;
    c->len = len;
    c->chunk = chunk;
    c->dst = dst;
    c->dst_cap = cap;
    c->limit = limit;
    c->fail_on_call = fail_on_call;

    ret = dcl_explode(in_cb, out_cb, c, &used);
    if (ret == DCL_OK) *in_used = used;
    return ret;
}

/* Expect a specific error code; also checks the output-callback invariants. */
static void expect_error(const uint8_t *vec, size_t len, int want,
                         const char *label)
{
    stream_ctx c;
    uint8_t buf[16];
    size_t used = 0;
    int ret;

    memset(buf, 0, sizeof buf);
    ret = run(&c, vec, len, 1, buf, sizeof buf, 0, 0, &used);
    g_checks++;
    if (ret == want) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL %s: got %d (%s), want %d (%s)\n", label,
               ret, dcl_strerror(ret), want, dcl_strerror(want));
    }
    CHECK(c.max_n <= MAX_N);
    CHECK(c.oversize == 0);
}

/* 1. Reference vector from blast.c: "AIAIAIAIAIAIA", chunked several ways. */
static void test_reference_vector(void)
{
    static const uint8_t vec[] = {0x00, 0x04, 0x82, 0x24, 0x25, 0x8F, 0x80, 0x7F};
    static const char want[] = "AIAIAIAIAIAIA";
    static const size_t chunks[] = {1, 2, 3, 8};
    size_t i;

    printf("  reference vector, chunk sizes 1/2/3/8\n");
    for (i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        stream_ctx c;
        uint8_t buf[64];
        size_t used = 0;
        int ret;

        memset(buf, 0, sizeof buf);
        ret = run(&c, vec, sizeof vec, chunks[i], buf, sizeof buf, 0, 0, &used);
        CHECK(ret == DCL_OK);
        CHECK(used == sizeof vec);
        CHECK(c.out_len == sizeof want - 1);
        CHECK(c.out_len <= sizeof buf);
        CHECK(memcmp(buf, want, sizeof want - 1) == 0);
        CHECK(c.max_n <= MAX_N);
        CHECK(c.max_n == sizeof want - 1);
        CHECK(c.oversize == 0);
    }
}

/* 2. Raw-literal (mode 0) vector: "A".  Verified with reference blast.c. */
static void test_raw_literal(void)
{
    static const uint8_t vec[] = {0x00, 0x04, 0x82, 0x02, 0xFE, 0x01};
    static const size_t chunks[] = {1, 6};
    size_t i;

    printf("  raw-literal vector (mode 0)\n");
    for (i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        stream_ctx c;
        uint8_t buf[16];
        size_t used = 0;
        int ret;

        memset(buf, 0, sizeof buf);
        ret = run(&c, vec, sizeof vec, chunks[i], buf, sizeof buf, 0, 0, &used);
        CHECK(ret == DCL_OK);
        CHECK(used == sizeof vec);
        CHECK(c.out_len == 1);
        CHECK(buf[0] == 'A');
        CHECK(c.max_n == 1);
    }
}

/* 3. Huffman-coded literal (mode 1) vectors, generated with the reference
 *    tables and verified by decoding with the reference blast.c:
 *      01 04 E2 80 7F                   -> "A"        (5 bytes)
 *      01 04 E2 0F 01 FF                -> "AAAA"     (6 bytes)
 *      01 04 50 6C D3 D4 03 FE 01       -> "Hello"    (9 bytes)
 */
static void test_coded_literals(void)
{
    static const uint8_t v1[] = {0x01, 0x04, 0xE2, 0x80, 0x7F};
    static const uint8_t v2[] = {0x01, 0x04, 0xE2, 0x0F, 0x01, 0xFF};
    static const uint8_t v3[] = {0x01, 0x04, 0x50, 0x6C, 0xD3, 0xD4, 0x03, 0xFE, 0x01};
    static const struct { const uint8_t *vec; size_t len; size_t chunk;
                          const char *want; } cases[] = {
        {v1, sizeof v1, 1, "A"},
        {v1, sizeof v1, 5, "A"},
        {v2, sizeof v2, 2, "AAAA"},
        {v3, sizeof v3, 3, "Hello"},
        {v3, sizeof v3, 9, "Hello"}
    };
    size_t i;

    printf("  coded-literal vectors (mode 1)\n");
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        stream_ctx c;
        uint8_t buf[32];
        size_t used = 0;
        size_t want = strlen(cases[i].want);
        int ret;

        memset(buf, 0, sizeof buf);
        ret = run(&c, cases[i].vec, cases[i].len, cases[i].chunk,
                  buf, sizeof buf, 0, 0, &used);
        CHECK(ret == DCL_OK);
        CHECK(used == cases[i].len);
        CHECK(c.out_len == want);
        CHECK(c.out_len <= sizeof buf);
        CHECK(memcmp(buf, cases[i].want, want) == 0);
        CHECK(c.max_n == want);
    }
}

/* 4. Truncated input must be DCL_ERR_INPUT. */
static void test_truncated(void)
{
    static const uint8_t e0[] = {0x00};
    static const uint8_t e1[] = {0x00, 0x04};
    static const uint8_t e2[] = {0x00, 0x04, 0x82, 0x02, 0xFE};

    printf("  truncated input\n");
    expect_error(NULL, 0, DCL_ERR_INPUT, "empty input");
    expect_error(e0, sizeof e0, DCL_ERR_INPUT, "{00}");
    expect_error(e1, sizeof e1, DCL_ERR_INPUT, "{00,04}");
    expect_error(e2, sizeof e2, DCL_ERR_INPUT, "00 04 82 02 FE");
}

/* 5. Bad header bytes. */
static void test_bad_headers(void)
{
    static const uint8_t a[] = {0x02};
    static const uint8_t b[] = {0x02, 0x04};
    static const uint8_t c[] = {0x00, 0x03};
    static const uint8_t d[] = {0x00, 0x07};
    static const uint8_t e[] = {0x01, 0x03};

    printf("  bad headers\n");
    expect_error(a, sizeof a, DCL_ERR_LITMODE, "{02}");
    expect_error(b, sizeof b, DCL_ERR_LITMODE, "{02,04}");
    expect_error(c, sizeof c, DCL_ERR_DICT, "{00,03}");
    expect_error(d, sizeof d, DCL_ERR_DICT, "{00,07}");
    expect_error(e, sizeof e, DCL_ERR_DICT, "{01,03}");
}

/* 6. Distance reaching before the start of the output.  blast.c returns -3
 *    for 00 04 1F 00; we map that to DCL_ERR_DIST. */
static void test_distance(void)
{
    static const uint8_t vec[] = {0x00, 0x04, 0x1F, 0x00};

    printf("  distance before start of output\n");
    expect_error(vec, sizeof vec, DCL_ERR_DIST, "00 04 1F 00");
}

/* 7. Output callback abort: reported at once, and never called again. */
static void test_abort(void)
{
    static const uint8_t ref[] = {0x00, 0x04, 0x82, 0x24, 0x25, 0x8F, 0x80, 0x7F};
    static const uint8_t longvec[] = {
        0x00, 0x04, 0x82, 0x08, 0x19, 0x42, 0x14, 0xE0, 0xFF, 0x04, 0xF8, 0x3F,
        0x01, 0xFE, 0x4F, 0x80, 0xFF, 0x13, 0xE0, 0xFF, 0x04, 0xF8, 0x3F, 0x01,
        0xFE, 0x4F, 0x80, 0xFF, 0x13, 0xE0, 0xFF, 0x04, 0xF8, 0x3F, 0x01, 0xFE,
        0x4F, 0x80, 0xFF, 0x13, 0xE0, 0xFF, 0x04, 0xF8, 0x3F, 0x01, 0xFE, 0x4F,
        0x80, 0xFF, 0x13, 0xF0, 0x0F};
    stream_ctx c;
    uint8_t buf[64];
    size_t used = 0;
    int ret;

    printf("  output abort\n");
    memset(buf, 0, sizeof buf);
    ret = run(&c, ref, sizeof ref, 8, buf, sizeof buf, 0, 1, &used);
    CHECK(ret == DCL_ERR_ABORT);
    CHECK(c.out_calls == 1);
    CHECK(c.after_abort == 0);
    CHECK(c.out_len == 0);

    /* abort on the first 4096-byte window flush of a long stream */
    memset(buf, 0, sizeof buf);
    ret = run(&c, longvec, sizeof longvec, 8, buf, sizeof buf, 0, 1, &used);
    CHECK(ret == DCL_ERR_ABORT);
    CHECK(c.out_calls == 1);
    CHECK(c.after_abort == 0);
    CHECK(c.out_len == 0);
    CHECK(c.sizes[0] == 4096);
}

/* 8. Long stream: 8292 bytes of "ABCD" repeated, verified with blast.c.
 *    Exercises window wrap, multiple flushes and a 100-byte tail. */
static void test_long_output(void)
{
    static const uint8_t vec[] = {
        0x00, 0x04, 0x82, 0x08, 0x19, 0x42, 0x14, 0xE0, 0xFF, 0x04, 0xF8, 0x3F,
        0x01, 0xFE, 0x4F, 0x80, 0xFF, 0x13, 0xE0, 0xFF, 0x04, 0xF8, 0x3F, 0x01,
        0xFE, 0x4F, 0x80, 0xFF, 0x13, 0xE0, 0xFF, 0x04, 0xF8, 0x3F, 0x01, 0xFE,
        0x4F, 0x80, 0xFF, 0x13, 0xE0, 0xFF, 0x04, 0xF8, 0x3F, 0x01, 0xFE, 0x4F,
        0x80, 0xFF, 0x13, 0xF0, 0x0F};
    static const char want[] = "ABCD";
    static uint8_t buf[9000];
    stream_ctx c;
    size_t used = 0;
    int ret;
    size_t i;

    printf("  long output (window wrap)\n");
    ret = run(&c, vec, sizeof vec, 5, buf, sizeof buf, 0, 0, &used);
    CHECK(ret == DCL_OK);
    CHECK(used == sizeof vec);
    CHECK(c.out_len == 8292);
    CHECK(c.max_n == 4096);
    CHECK(c.nsizes == 3 && c.sizes[0] == 4096 && c.sizes[1] == 4096 &&
          c.sizes[2] == 100);
    for (i = 0; i < c.out_len && i < sizeof buf; i++)
        if (buf[i] != (uint8_t)want[i % 4]) break;
    CHECK(c.out_len == 8292 && i == 8292);
}

/* 9. Deterministic fuzz: well-formed headers, random payload, chunked input. */
static uint32_t g_rnd;

static uint32_t xorshift32(void)
{
    uint32_t x = g_rnd;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rnd = x;
    return x;
}

static void test_fuzz(void)
{
    enum { CASES = 10000 };
    static const int good[] = {
        DCL_OK, DCL_ERR_INPUT, DCL_ERR_LITMODE, DCL_ERR_DICT,
        DCL_ERR_DIST, DCL_ERR_CODE, DCL_ERR_ABORT
    };
    uint8_t in[512];
    int n_ok = 0, n_input = 0, n_code = 0, n_abort = 0, n_bad = 0;
    int n_dist = 0, n_hdr = 0;
    unsigned long out_total = 0;
    int i;

    printf("  fuzz: %d cases\n", (int)CASES);
    g_rnd = 0x12345678u;
    for (i = 0; i < CASES; i++) {
        uint32_t r = xorshift32();
        size_t len = 2 + (size_t)(r % 511);        /* 2..512 */
        size_t chunk, j;
        stream_ctx c;
        size_t used = 0;
        int ret, known = 0;

        in[0] = (uint8_t)(r & 1);                  /* valid literal mode */
        in[1] = (uint8_t)(4 + r % 3);              /* valid dictionary */
        for (j = 2; j < len; j++) in[j] = (uint8_t)(xorshift32() >> 24);
        chunk = 1 + (size_t)(xorshift32() % len);

        ret = run(&c, in, len, chunk, NULL, 0, OUT_CAP, 0, &used);

        for (j = 0; j < sizeof good / sizeof good[0]; j++)
            if (ret == good[j]) known = 1;
        if (!known) {
            n_bad++;
            printf("FAIL fuzz case %d: undefined return code %d\n", i, ret);
        }
        if (c.max_n > MAX_N) {
            n_bad++;
            printf("FAIL fuzz case %d: output chunk %lu > 4096\n", i,
                   (unsigned long)c.max_n);
        }
        if (c.oversize) {
            n_bad++;
            printf("FAIL fuzz case %d: oversized output chunk flagged\n", i);
        }
        switch (ret) {
        case DCL_OK:        n_ok++; break;
        case DCL_ERR_INPUT: n_input++; break;
        case DCL_ERR_CODE:  n_code++; break;
        case DCL_ERR_ABORT: n_abort++; break;
        case DCL_ERR_DIST:  n_dist++; break;
        default:            n_hdr++; break;   /* literal mode / dictionary */
        }
        out_total += (unsigned long)c.out_len;
    }
    CHECK(n_bad == 0);
    printf("  fuzz summary: ok=%d input=%d dist=%d code=%d abort=%d hdr=%d,"
           " out=%lu bytes\n", n_ok, n_input, n_dist, n_code, n_abort, n_hdr,
           out_total);
}

int main(void)
{
    printf("test_dcl: PKWare DCL decompressor\n");

    test_reference_vector();
    test_raw_literal();
    test_coded_literals();
    test_truncated();
    test_bad_headers();
    test_distance();
    test_abort();
    test_long_output();
    test_fuzz();

    printf("\n%d checks: %d passed, %d failed\n", g_checks, g_pass, g_fail);
    printf("%s\n", g_fail == 0 ? "PASS test_dcl" : "FAIL test_dcl");
    return g_fail == 0 ? 0 : 1;
}

