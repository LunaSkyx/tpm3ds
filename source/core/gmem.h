/* Guest memory: one arena; a guest pointer is a 32-bit offset into it
 * (guest address == arena offset, so the exe maps at its own image base).
 * All access goes through these helpers: bounds-checked, alignment-safe
 * (memcpy), little-endian guest on a little-endian host (3DS ARM11, x86). */
#ifndef GMEM_H
#define GMEM_H

#include <stdint.h>
#include <string.h>

typedef uint32_t gptr;

/* Hot helpers must inline even in the huge lifted functions, where GCC's
 * growth limits otherwise leave them as out-of-line calls. */
#define GMEM_AI static inline __attribute__((always_inline))
#define GMEM_NULL_GUARD 0x10000u   /* guest addresses below this always fault */

extern uint8_t *g_gmem;
extern uint32_t g_gmem_size;
extern uint32_t g_gmem_faults;
extern void (*g_gmem_fault_hook)(gptr a, uint32_t n, int write);   /* after logging */

int  gmem_init(uint32_t size);      /* zero-filled; returns 1 on success */
void gmem_fini(void);
__attribute__((cold, noinline)) void gmem_fault(gptr a, uint32_t n, int write);  /* logs; access is dropped */

static inline int gmem_ok(gptr a, uint32_t n)
{
    return a >= GMEM_NULL_GUARD && n <= g_gmem_size && a <= g_gmem_size - n;
}

#define GMEM_RD(bits)                                                        \
    static inline uint##bits##_t gmem_rd##bits(gptr a)                      \
    {                                                                        \
        uint##bits##_t v = 0;                                                \
        if (gmem_ok(a, bits / 8)) memcpy(&v, g_gmem + a, bits / 8);          \
        else gmem_fault(a, bits / 8, 0);                                     \
        return v;                                                            \
    }
#define GMEM_WR(bits)                                                        \
    static inline void gmem_wr##bits(gptr a, uint##bits##_t v)              \
    {                                                                        \
        if (gmem_ok(a, bits / 8)) memcpy(g_gmem + a, &v, bits / 8);          \
        else gmem_fault(a, bits / 8, 1);                                     \
    }
GMEM_RD(8) GMEM_RD(16) GMEM_RD(32)
GMEM_WR(8) GMEM_WR(16) GMEM_WR(32)
#undef GMEM_RD
#undef GMEM_WR

/* The same with the arena base and size passed in. Lifted functions copy both into
 * locals once (GMEM_LOCAL): guest stores are byte stores to the C compiler and
 * would otherwise force a reload of the globals after every one. One unsigned
 * compare covers both bounds (addresses below the guard wrap to large values);
 * the arena is always larger than the guard. */
#define GMEM_LOCAL uint8_t *const gm_base = g_gmem; const uint32_t gm_size = g_gmem_size
#define GMEM_RDL(bits)                                                               GMEM_AI uint##bits##_t gmem_rd##bits##_l(uint8_t *b, uint32_t sz, gptr a)     {                                                                                    uint##bits##_t v = 0;                                                            if ((uint32_t)(a - GMEM_NULL_GUARD) <= sz - (GMEM_NULL_GUARD + bits / 8)) memcpy(&v, b + a, bits / 8);         else gmem_fault(a, bits / 8, 0);                                                 return v;                                                                    }
#define GMEM_WRL(bits)                                                               GMEM_AI void gmem_wr##bits##_l(uint8_t *b, uint32_t sz, gptr a, uint##bits##_t v)     {                                                                                    if ((uint32_t)(a - GMEM_NULL_GUARD) <= sz - (GMEM_NULL_GUARD + bits / 8)) memcpy(b + a, &v, bits / 8);         else gmem_fault(a, bits / 8, 1);                                             }
#ifdef TPM_GMEM_MASKED
/* Release builds (3DS): the arena is exactly GMEM_MASKED_SIZE bytes (plus a
 * small tail pad) and lifted accesses wrap into it with a constant mask instead
 * of a checked branch: one AND per access, and constant guest addresses fold
 * into a plain base+offset load. A stray access lands somewhere in the arena
 * instead of being logged and dropped (the original exe would have crashed on
 * it; runs with the checked build report 0 faults). gmem_init enforces the size. */
#define GMEM_MASKED_SIZE (64u << 20)
#undef GMEM_RDL
#undef GMEM_WRL
#define GMEM_RDL(bits)                                                                   GMEM_AI uint##bits##_t gmem_rd##bits##_l(uint8_t *b, uint32_t sz, gptr a)         {                                                                                        uint##bits##_t v; (void)sz;                                                          memcpy(&v, b + (a & (GMEM_MASKED_SIZE - 1u)), bits / 8);                             return v;                                                                        }
#define GMEM_WRL(bits)                                                                   GMEM_AI void gmem_wr##bits##_l(uint8_t *b, uint32_t sz, gptr a, uint##bits##_t v)     {                                                                                        (void)sz;                                                                            memcpy(b + (a & (GMEM_MASKED_SIZE - 1u)), &v, bits / 8);                         }
#endif
GMEM_RDL(8) GMEM_RDL(16) GMEM_RDL(32)
GMEM_WRL(8) GMEM_WRL(16) GMEM_WRL(32)
#undef GMEM_RDL
#undef GMEM_WRL
#define GRD8(a)      gmem_rd8_l(gm_base, gm_size, (a))
#define GRD16(a)     gmem_rd16_l(gm_base, gm_size, (a))
#define GRD32(a)     gmem_rd32_l(gm_base, gm_size, (a))
#define GWR8(a, v)   gmem_wr8_l(gm_base, gm_size, (a), (v))
#define GWR16(a, v)  gmem_wr16_l(gm_base, gm_size, (a), (v))
#define GWR32(a, v)  gmem_wr32_l(gm_base, gm_size, (a), (v))

/* Host view of [a, a+n) for bulk byte copies; NULL (and a fault) if out of range. */
static inline uint8_t *gmem_span(gptr a, uint32_t n, int write)
{
    if (gmem_ok(a, n)) return g_gmem + a;
    gmem_fault(a, n, write);
    return NULL;
}

/* Copy a NUL-terminated guest string (at most n-1 chars); returns length. */
uint32_t gmem_strcpy_out(char *dst, uint32_t n, gptr src);

#endif
