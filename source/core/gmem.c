#include <stdlib.h>
#include "core/gmem.h"
#include "pal/pal.h"

uint8_t *g_gmem;
uint32_t g_gmem_size;
uint32_t g_gmem_faults;
void (*g_gmem_fault_hook)(gptr a, uint32_t n, int write);

int gmem_init(uint32_t size)
{
    gmem_fini();
#ifdef TPM_GMEM_MASKED
    if (size != GMEM_MASKED_SIZE) {
        pal_log("gmem: masked build needs a %lu KB arena, not %lu KB", (unsigned long)(GMEM_MASKED_SIZE >> 10),
                (unsigned long)(size >> 10));
        return 0;
    }
#endif
    g_gmem = calloc(1, size + 16u);          /* tail pad: masked 2/4-byte accesses at the very end */
    if (!g_gmem) { pal_log("gmem: FAILED to allocate %lu KB arena", (unsigned long)(size >> 10)); return 0; }
    g_gmem_size = size;
    g_gmem_faults = 0;
    pal_log("gmem: arena %lu KB", (unsigned long)(size >> 10));
    return 1;
}

void gmem_fini(void)
{
    free(g_gmem);
    g_gmem = NULL;
    g_gmem_size = 0;
}

void gmem_fault(gptr a, uint32_t n, int write)
{
    /* First faults are logged in full; afterwards only counted, so a runaway
     * loop cannot flood the SD log. The caller decides whether to stop. */
    if (g_gmem_faults++ < 16)
        pal_log("gmem: FAULT %s %lu bytes at %08lx", write ? "write" : "read",
                (unsigned long)n, (unsigned long)a);
    if (g_gmem_fault_hook) g_gmem_fault_hook(a, n, write);
}

uint32_t gmem_strcpy_out(char *dst, uint32_t n, gptr src)
{
    uint32_t i = 0;
    if (n == 0) return 0;
    for (; i + 1 < n; i++) {
        uint8_t c = gmem_rd8(src + i);
        if (!c) break;
        dst[i] = (char)c;
    }
    dst[i] = 0;
    return i;
}
