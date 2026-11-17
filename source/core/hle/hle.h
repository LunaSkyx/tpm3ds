/* High-level emulation of the imports (Win32, DirectX, Miles, Bink, iMUSE).
 * Every import gets a thunk address written into its IAT slot. A guest call
 * to a thunk lands in hle_dispatch(), which runs the C implementation.
 * Calling convention (all imports used here are stdcall): on entry the guest
 * return address is at [esp], args at [esp+4], [esp+8], ...; the
 * implementation sets eax and pops return address + args (hle_ret). */
#ifndef HLE_H
#define HLE_H

#include <stdint.h>
#include "core/cpu.h"

#define HLE_THUNK_BASE 0xfff00000u     /* outside the arena: never readable code */

typedef void (*hle_fn)(cpu_t *c);
typedef struct { const char *dll, *name; hle_fn fn; } hle_def;

#define ARG(n) gmem_rd32(c->r[ESP] + 4u + 4u * (uint32_t)(n))
static inline void hle_ret(cpu_t *c, uint32_t v, int nargs)
{
    c->r[EAX] = v;
    c->r[ESP] += 4u + 4u * (uint32_t)nargs;
}

void     hle_register(const hle_def *defs, int n);         /* called by each hle_*.c init */
uint32_t hle_thunk_for(const char *dll, const char *name);  /* also for not-yet-implemented imports */
int      hle_is_thunk(uint32_t addr);
void     hle_dispatch(cpu_t *c, uint32_t addr);
const char *hle_name(uint32_t addr);
void     hle_init_all(void);                              /* before IAT patching */
void     hle_process_init(void);                          /* after the guest heap exists */
uint32_t hle_missing_count(void);
void hle_report_top(uint32_t frames);

/* string helpers: guest char* <-> host */
uint32_t hle_str_in(char *dst, uint32_t n, gptr src);       /* copies, returns length */
void     hle_str_out(gptr dst, uint32_t n, const char *src); /* NUL-terminated, truncated to n */

/* thunk address of the HLE function running now (0 = guest code); for the host profiler */
extern volatile uint32_t g_hle_cur;

#endif
