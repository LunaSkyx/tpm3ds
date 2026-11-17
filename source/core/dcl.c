/* dcl.c -- decompress a stream from the PKWare Data Compression Library.
 *
 * This file is an ALTERED, DERIVED VERSION of blast.c version 1.3 (24 Aug
 * 2013) from zlib/contrib/blast, written by Mark Adler.  It is plainly marked
 * as such and must not be represented as the original software.
 *
 * Differences from blast.c:
 *   - All decoder state, including the 4096-byte window and the three Huffman
 *     decoding tables, lives in a struct on the stack of dcl_explode().  The
 *     code is therefore reentrant: concurrent calls share no mutable state.
 *     (blast.c builds its tables in writable statics on first use.)
 *   - Errors are reported with explicit codes instead of setjmp/longjmp.
 *     bits() and decode() return sentinels (-1 input error, -2 invalid code)
 *     that the caller propagates.  There is no recursion either.
 *   - Input and output are streamed through callbacks that share one context,
 *     the output callback is never passed more than 4096 bytes, and the number
 *     of compressed bytes consumed is reported on success.
 *   - The overlapping match copy uses integer indices rather than forming a
 *     pointer before the start of the window array, which avoids undefined
 *     behaviour while keeping blast.c's exact wrap semantics.
 *
 * Original copyright notice and licence (from blast.h):
 *
 *   blast.h -- interface for blast.c
 *   Copyright (C) 2003, 2012, 2013 Mark Adler
 *   version 1.3, 24 Aug 2013
 *
 *   This software is provided 'as-is', without any express or implied
 *   warranty.  In no event will the author be held liable for any damages
 *   arising from the use of this software.
 *
 *   Permission is granted to anyone to use this software for any purpose,
 *   including commercial applications, and to alter it and redistribute it
 *   freely, subject to the following restrictions:
 *
 *   1. The origin of this software must not be misrepresented; you must not
 *      claim that you wrote the original software. If you use this software
 *      in a product, an acknowledgment in the product documentation would be
 *      appreciated but is not required.
 *   2. Altered source versions must be plainly marked as such, and must not be
 *      misrepresented as being the original software.
 *   3. This notice may not be removed or altered from any source distribution.
 *
 *   Mark Adler    madler@alumni.caltech.edu
 *
 * Format (see blast.c for the full description): the first byte is 0 if
 * literals are uncoded or 1 if they are Huffman coded, the second is the
 * dictionary size exponent (4, 5 or 6).  Bits are read least significant bit
 * first.  Every symbol is preceded by a 1-bit flag, 0 for a literal and 1 for
 * a length/distance pair; the length code 519 ends the stream.
 */

#include "dcl.h"

#define DCL_MAXBITS 13              /* maximum Huffman code length */
#define DCL_WIN     4096            /* window (and output buffer) size */

struct dcl_huff {
    int nsym;                       /* number of symbol[] entries in use */
    short *symbol;                  /* symbols in canonical order */
    short count[DCL_MAXBITS + 1];   /* number of codes of each length */
};

struct dcl_state {
    /* callbacks */
    dcl_in_fn in;
    dcl_out_fn out;
    void *ctx;

    /* input */
    const uint8_t *inbuf;           /* current input chunk */
    size_t inleft;                  /* bytes left in inbuf */
    size_t in_total;                /* bytes handed to us by the callback */

    /* bit buffer, filled least significant bit first */
    int bitbuf;
    int bitcnt;

    /* output */
    unsigned next;                  /* next write index in out[] */
    int first;                      /* true while inside the first 4 KiB */
    uint8_t win[DCL_WIN];           /* output buffer and sliding window */

    /* decoding tables, built once per call */
    struct dcl_huff lit, len, dist;
    short litsym[256];
    short lensym[16];
    short distsym[64];
};

/* Bit lengths of the literal codes, run-length coded (count high nibble + 1,
 * code length low nibble), as fixed by the format. */
static const unsigned char litlen[] = {
    11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
    9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
    7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
    8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
    44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
    44, 173};
static const unsigned char lenlen[] = {2, 35, 36, 53, 38, 23};
static const unsigned char distlen[] = {2, 20, 53, 230, 247, 151, 248};
static const short base[16] = {3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
static const char extra[16] = {0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8};

/*
 * Build the decoding table for one Huffman code from the run-length coded code
 * lengths rep[0..n-1].  This is blast.c's construct(), writing into the arrays
 * supplied by the caller instead of into static storage.  Returns 0 on
 * success, -1 if the description is malformed or over-subscribed.
 */
static int dcl_construct(struct dcl_huff *h, short *symbol, int nsym,
                         const unsigned char *rep, int n)
{
    short offs[DCL_MAXBITS + 1];    /* offsets into symbol[] per length */
    short length[256];              /* code length of each symbol */
    int sym, len, left, i;

    if (nsym <= 0 || nsym > 256 || n <= 0) return -1;
    h->nsym = nsym;
    h->symbol = symbol;

    /* expand the compact repeat counts into the per-symbol length list */
    sym = 0;
    do {
        len = *rep++;
        left = (len >> 4) + 1;
        len &= 15;
        if (len > DCL_MAXBITS) return -1;
        do {
            if (sym >= nsym) return -1;
            length[sym++] = (short)len;
        } while (--left);
    } while (--n);

    /* count the number of codes of each length */
    for (i = 0; i <= DCL_MAXBITS; i++) h->count[i] = 0;
    for (i = 0; i < sym; i++) h->count[length[i]]++;
    if (h->count[0] == sym) return 0;   /* no codes; decode() will fail */

    /* reject an over-subscribed set of lengths */
    left = 1;
    for (len = 1; len <= DCL_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }

    /* offsets into symbol[] for each length, then sort by length */
    offs[1] = 0;
    for (len = 1; len < DCL_MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (i = 0; i < sym; i++)
        if (length[i] != 0) {
            int at = offs[length[i]]++;
            if (at < 0 || at >= nsym) return -1;
            symbol[at] = (short)i;
        }
    return 0;
}

/*
 * Fetch the next input chunk.  Returns 0 on success, -1 if the callback
 * reported end of input (or violated its contract by handing back no buffer).
 */
static int dcl_refill(struct dcl_state *s)
{
    const uint8_t *buf = NULL;
    size_t n = s->in(s->ctx, &buf);

    if (n == 0 || buf == NULL) return -1;
    s->inbuf = buf;
    s->inleft = n;
    s->in_total += n;
    return 0;
}

/*
 * Return the next need bits (0..8) from the stream, or -1 if the input ended.
 * Bits are pulled from the low end of the bit buffer; new bytes are appended
 * above.  Fewer than eight bits remain buffered afterwards.
 */
static int dcl_bits(struct dcl_state *s, int need)
{
    int val = s->bitbuf;

    while (s->bitcnt < need) {
        if (s->inleft == 0 && dcl_refill(s) != 0) return -1;
        val |= (int)(*s->inbuf++) << s->bitcnt;
        s->inleft--;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1 << need) - 1);
}

/*
 * Decode one symbol with table h.  Returns the symbol, -1 if the input ended,
 * or -2 for a code that is not in the table (after at most DCL_MAXBITS bits).
 * The code is accumulated with inverted bits, as in blast.c, because the
 * stored codes are bit-reversed with respect to the canonical ordering.
 */
static int dcl_decode(struct dcl_state *s, const struct dcl_huff *h)
{
    int len, code, first, count, index, bitbuf, left, r;
    const short *next;

    bitbuf = s->bitbuf;
    left = s->bitcnt;
    code = first = index = 0;
    len = 1;
    next = h->count + 1;
    for (;;) {
        while (left != 0) {
            left--;
            code |= (bitbuf & 1) ^ 1;       /* invert the code bit */
            bitbuf >>= 1;
            count = *next++;
            if (code < first + count) {     /* code of length len? */
                s->bitbuf = bitbuf;
                s->bitcnt = (s->bitcnt - len) & 7;
                r = index + (code - first);
                if (r < 0 || r >= h->nsym) return -2;
                return h->symbol[r];
            }
            index += count;                 /* advance to the next length */
            first += count;
            first <<= 1;
            code <<= 1;
            len++;
        }
        left = (DCL_MAXBITS + 1) - len;
        if (left == 0) break;               /* ran out of codes */
        if (s->inleft == 0 && dcl_refill(s) != 0) return -1;
        bitbuf = *s->inbuf++;
        s->inleft--;
        if (left > 8) left = 8;
    }
    return -2;
}

int dcl_explode(dcl_in_fn in, dcl_out_fn out, void *ctx, size_t *in_used)
{
    struct dcl_state s;
    int lit, dict, symbol, len, shift, extra_bits;
    unsigned dist;

    if (in == NULL || out == NULL) return DCL_ERR_INPUT;

    s.in = in;
    s.out = out;
    s.ctx = ctx;
    s.inbuf = NULL;
    s.inleft = 0;
    s.in_total = 0;
    s.bitbuf = 0;
    s.bitcnt = 0;
    s.next = 0;
    s.first = 1;

    if (dcl_construct(&s.lit, s.litsym, 256, litlen, (int)sizeof litlen) != 0 ||
        dcl_construct(&s.len, s.lensym, 16, lenlen, (int)sizeof lenlen) != 0 ||
        dcl_construct(&s.dist, s.distsym, 64, distlen, (int)sizeof distlen) != 0)
        return DCL_ERR_CODE;

    /* header: literal mode, then dictionary size exponent */
    lit = dcl_bits(&s, 8);
    if (lit < 0) return DCL_ERR_INPUT;
    if (lit > 1) return DCL_ERR_LITMODE;

    dict = dcl_bits(&s, 8);
    if (dict < 0) return DCL_ERR_INPUT;
    if (dict < 4 || dict > 6) return DCL_ERR_DICT;

    for (;;) {
        int flag = dcl_bits(&s, 1);
        if (flag < 0) return DCL_ERR_INPUT;

        if (flag == 0) {                    /* literal */
            if (lit) {
                symbol = dcl_decode(&s, &s.lit);
                if (symbol < 0) return symbol == -1 ? DCL_ERR_INPUT : DCL_ERR_CODE;
            } else {
                symbol = dcl_bits(&s, 8);
                if (symbol < 0) return DCL_ERR_INPUT;
            }
            s.win[s.next++] = (uint8_t)symbol;
            if (s.next == DCL_WIN) {
                if (out(ctx, s.win, s.next)) return DCL_ERR_ABORT;
                s.next = 0;
                s.first = 0;
            }
            continue;
        }

        /* length code */
        symbol = dcl_decode(&s, &s.len);
        if (symbol < 0) return symbol == -1 ? DCL_ERR_INPUT : DCL_ERR_CODE;
        if (symbol > 15) return DCL_ERR_CODE;
        extra_bits = dcl_bits(&s, extra[symbol]);
        if (extra_bits < 0) return DCL_ERR_INPUT;
        len = base[symbol] + extra_bits;
        if (len == 519) break;              /* end of stream */

        /* distance code */
        shift = (len == 2) ? 2 : dict;
        symbol = dcl_decode(&s, &s.dist);
        if (symbol < 0) return symbol == -1 ? DCL_ERR_INPUT : DCL_ERR_CODE;
        if (symbol > 63) return DCL_ERR_CODE;
        dist = (unsigned)symbol << shift;
        extra_bits = dcl_bits(&s, shift);
        if (extra_bits < 0) return DCL_ERR_INPUT;
        dist += (unsigned)extra_bits;
        dist++;
        if (s.first && dist > s.next) return DCL_ERR_DIST;

        /* copy len bytes from dist bytes back; overlap is allowed */
        for (;;) {
            unsigned from, copy;

            if (s.next >= dist) {           /* source is contiguous */
                from = s.next - dist;
                copy = DCL_WIN - s.next;
            } else {                        /* source wraps at the window end */
                from = s.next + DCL_WIN - dist;
                copy = dist - s.next;
            }
            if (copy > (unsigned)len) copy = (unsigned)len;
            len -= (int)copy;
            while (copy != 0) {
                s.win[s.next] = s.win[from];
                s.next++;
                from++;
                copy--;
            }
            if (s.next == DCL_WIN) {
                if (out(ctx, s.win, s.next)) return DCL_ERR_ABORT;
                s.next = 0;
                s.first = 0;
            }
            if (len == 0) break;
        }
    }

    /* flush the tail: a successful end flushes what is left in the window (a
     * failed stream flushes nothing); only here is *in_used written */
    if (s.next != 0 && out(ctx, s.win, s.next)) return DCL_ERR_ABORT;
    if (in_used != NULL) *in_used = s.in_total - s.inleft;
    return DCL_OK;
}

const char *dcl_strerror(int err)
{
    switch (err) {
    case DCL_OK:          return "success";
    case DCL_ERR_INPUT:   return "unexpected end of input";
    case DCL_ERR_LITMODE: return "invalid literal mode";
    case DCL_ERR_DICT:    return "invalid dictionary size";
    case DCL_ERR_DIST:    return "match distance before start of output";
    case DCL_ERR_CODE:    return "invalid Huffman code";
    case DCL_ERR_ABORT:   return "output callback aborted";
    default:              return "unknown error";
    }
}

