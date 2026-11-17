/* Guest heap inside the arena (HeapAlloc/GlobalAlloc/LocalAlloc/malloc land
 * here via HLE) and a 64 KB-granular region allocator for VirtualAlloc.
 * Blocks are 16-byte aligned and zero-filled (deterministic runs). */
#ifndef GHEAP_H
#define GHEAP_H

#include <stdint.h>
#include "core/gmem.h"

void     gheap_init(gptr lo, gptr hi);          /* [lo, hi) must be 16-aligned */
gptr     gheap_alloc(uint32_t n);               /* 0 on failure (logged with size) */
void     gheap_free(gptr p);                    /* 0 is ignored; bad pointers are logged */
gptr     gheap_realloc(gptr p, uint32_t n);
uint32_t gheap_size(gptr p);                    /* usable size, 0 if not a live block */
uint32_t gheap_free_bytes(void);
uint32_t gheap_peak_bytes(void);               /* most bytes in use at once */

void     gvirt_init(gptr lo, gptr hi);
gptr     gvirt_reserve(gptr want, uint32_t n);  /* want == 0: anywhere; 64 KB granular */
int      gvirt_release(gptr p);
int      gvirt_contains(gptr p, uint32_t n);    /* inside a reserved region? */
uint32_t gvirt_free_bytes(void);

#endif
