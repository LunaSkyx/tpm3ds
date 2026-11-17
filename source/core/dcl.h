#ifndef DCL_H
#define DCL_H
#include <stddef.h>
#include <stdint.h>

/* Input callback: return the next chunk of compressed data and set *buf to
 * point at it, or return 0 at end of input. The chunk must stay valid until
 * the next call. */
typedef size_t (*dcl_in_fn)(void *ctx, const uint8_t **buf);

/* Output callback: consume n decompressed bytes (n <= 4096).
 * Return 0 to continue, nonzero to abort (e.g. user quit, write error). */
typedef int (*dcl_out_fn)(void *ctx, const uint8_t *data, size_t n);

enum {
    DCL_OK          =  0,
    DCL_ERR_INPUT   = -1,  /* ran out of input before the end-of-stream code */
    DCL_ERR_LITMODE = -2,  /* header byte 0 not 0 or 1 */
    DCL_ERR_DICT    = -3,  /* header byte 1 not 4, 5 or 6 */
    DCL_ERR_DIST    = -4,  /* match distance reaches before the start of output */
    DCL_ERR_CODE    = -5,  /* invalid Huffman code */
    DCL_ERR_ABORT   = -6   /* out callback returned nonzero */
};

/* Decompress one complete DCL stream. On success *in_used (if non-NULL)
 * receives the number of compressed bytes consumed from the input. ctx is
 * passed to both callbacks. Returns DCL_OK or a negative DCL_ERR_* code. */
int dcl_explode(dcl_in_fn in, dcl_out_fn out, void *ctx, size_t *in_used);

const char *dcl_strerror(int err);
#endif
