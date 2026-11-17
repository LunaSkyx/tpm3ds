#include <string.h>
#include "core/gheap.h"
#include "pal/pal.h"

/* Block header (16 bytes) in guest memory:
 *   +0 size of the whole block incl. header (multiple of 16)
 *   +4 size of the previous block (0 for the first)
 *   +8 magic | used flag
 *   +12 requested size
 * The heap is a list of arenas: arena 0 is the fixed heap range, more arenas (256 KB each) are carved out of the VirtualAlloc
 * region whenever every arena is full, so the unused part of that region serves small blocks too. */
#define HDR       16u
#define MAGIC     0x48ea9000u
#define USED      1u
#define VMAGIC    0x48ea9f00u   /* big block (64 KB and up) that lives alone in the VirtualAlloc region (heap was full) */
#define ARENA_MAX 64
#define ARENA_GROW 0x40000u     /* size of an extra arena */
#define VIRT_DIRECT 0x10000u    /* blocks of at least this size are VirtualAlloc regions of their own */

typedef struct { gptr lo, hi, rover; uint32_t nofit; } arena;   /* nofit: no free block of this size or more exists (0xffffffff: unknown) */
static arena g_ar[ARENA_MAX];
static int g_nar;
static uint32_t g_free, g_total, g_peak;

static uint32_t bsize(gptr b) { return gmem_rd32(b); }
static int      bused(gptr b) { return (gmem_rd32(b + 8) & USED) != 0; }
static void     bset(gptr b, uint32_t size, uint32_t prev, int used, uint32_t req)
{
    gmem_wr32(b, size);
    gmem_wr32(b + 4, prev);
    gmem_wr32(b + 8, MAGIC | (used ? USED : 0));
    gmem_wr32(b + 12, req);
}
static int valid(const arena *a, gptr b)
{
    return b >= a->lo && b + HDR <= a->hi && (gmem_rd32(b + 8) & ~USED) == MAGIC &&
           bsize(b) >= HDR && b + bsize(b) <= a->hi;
}
static arena *arena_of(gptr b)
{
    for (int i = 0; i < g_nar; i++) if (b >= g_ar[i].lo && b < g_ar[i].hi) return &g_ar[i];
    return NULL;
}

static int vblock(gptr b)
{
    return gvirt_contains(b, HDR) && gmem_rd32(b + 8) == VMAGIC;
}

void gheap_init(gptr lo, gptr hi)
{
    g_ar[0].lo = g_ar[0].rover = lo;
    g_ar[0].hi = hi;
    g_ar[0].nofit = 0xffffffffu;
    g_nar = 1;
    bset(lo, hi - lo, 0, 0, 0);
    g_free = g_total = hi - lo;
    g_peak = 0;
    pal_log("gheap: %08lx-%08lx (%lu KB)", (unsigned long)lo, (unsigned long)hi, (unsigned long)((hi - lo) >> 10));
}

static gptr try_from(const arena *a, gptr start, gptr end, uint32_t need)
{
    for (gptr b = start; b < end; b += bsize(b)) {
        if (!valid(a, b)) { pal_log("gheap: CORRUPT block at %08lx", (unsigned long)b); return 0; }
        if (!bused(b) && bsize(b) >= need) return b;
    }
    return 0;
}

static gptr carve(arena *a, gptr b, uint32_t need, uint32_t n)
{
    uint32_t total = bsize(b), prev = gmem_rd32(b + 4);
    if (total - need >= HDR + 16u) {            /* split */
        gptr rest = b + need;
        bset(rest, total - need, need, 0, 0);
        if (rest + (total - need) < a->hi) gmem_wr32(rest + (total - need) + 4, total - need);
        total = need;
    }
    bset(b, total, prev, 1, n);
    g_free -= total;
    if (g_total - g_free > g_peak) g_peak = g_total - g_free;
    a->rover = b;
    uint8_t *p = gmem_span(b + HDR, total - HDR, 1);
    if (p) memset(p, 0, total - HDR);
    return b + HDR;
}

static gptr alloc_in(arena *a, uint32_t need, uint32_t n)
{
    if (need >= a->nofit) return 0;                  /* a full arena is not scanned again until something at least this big is freed */
    gptr b = try_from(a, a->rover, a->hi, need);
    if (!b) b = try_from(a, a->lo, a->rover, need);
    if (!b) { if (need < a->nofit) a->nofit = need; return 0; }
    return carve(a, b, need, n);
}

gptr gheap_alloc(uint32_t n)
{
    if (n > 0x3ffffff0u) { pal_log("gheap: alloc %lu FAILED (too large)", (unsigned long)n); return 0; }
    uint32_t need = (n + HDR + 15u) & ~15u;
    for (int i = 0; i < g_nar; i++) {
        gptr p = alloc_in(&g_ar[i], need, n);
        if (p) return p;
    }
    /* Every arena is full (the queen and espa levels fill the 27 MB heap): large blocks (800x480 surfaces, Bink buffers) become
     * VirtualAlloc regions of their own, smaller ones go into a new 256 KB arena taken from the same region. A NULL here used to
     * end in "call to unknown address 0" a minute later in the game. */
    if (n >= VIRT_DIRECT) {
        gptr v = gvirt_reserve(0, n + HDR);
        if (v) {
            gmem_wr32(v, 0); gmem_wr32(v + 4, 0); gmem_wr32(v + 8, VMAGIC); gmem_wr32(v + 12, n);
            return v + HDR;
        }
    } else if (g_nar < ARENA_MAX && need <= ARENA_GROW) {
        gptr lo = gvirt_reserve(0, ARENA_GROW);
        if (lo) {
            arena *a = &g_ar[g_nar++];
            a->lo = a->rover = lo;
            a->hi = lo + ARENA_GROW;
            a->nofit = 0xffffffffu;
            bset(lo, ARENA_GROW, 0, 0, 0);
            g_free += ARENA_GROW;
            g_total += ARENA_GROW;
            pal_log("gheap: heap full, new %lu KB arena %d at %08lx (free in VirtualAlloc %lu KB)", (unsigned long)(ARENA_GROW >> 10),
                    g_nar - 1, (unsigned long)lo, (unsigned long)(gvirt_free_bytes() >> 10));
            gptr p = alloc_in(a, need, n);
            if (p) return p;
        }
    }
    pal_log("gheap: alloc %lu FAILED (free %lu KB)", (unsigned long)n, (unsigned long)(g_free >> 10));
    return 0;
}

void gheap_free(gptr p)
{
    if (!p) return;
    gptr b = p - HDR;
    arena *a = arena_of(b);
    if (!a) {
        if (vblock(b)) { gmem_wr32(b + 8, 0); gvirt_release(b); return; }
        pal_log("gheap: free of bad pointer %08lx", (unsigned long)p);
        return;
    }
    if (!valid(a, b) || !bused(b)) { pal_log("gheap: free of bad pointer %08lx", (unsigned long)p); return; }
    uint32_t size = bsize(b), prev = gmem_rd32(b + 4);
    g_free += size;
    gptr next = b + size;
    if (next < a->hi && valid(a, next) && !bused(next)) size += bsize(next);   /* merge forward */
    if (prev && valid(a, b - prev) && !bused(b - prev)) {                     /* merge backward */
        b -= prev;
        size += bsize(b);
        prev = gmem_rd32(b + 4);
    }
    bset(b, size, prev, 0, 0);
    if (size >= a->nofit) a->nofit = 0xffffffffu;
    if (b + size < a->hi) gmem_wr32(b + size + 4, size);
    if (a->rover > b && a->rover < b + size) a->rover = b;
}

uint32_t gheap_size(gptr p)
{
    gptr b = p - HDR;
    if (!p) return 0;
    arena *a = arena_of(b);
    if (!a) return vblock(b) ? gmem_rd32(b + 12) : 0;
    return (valid(a, b) && bused(b)) ? gmem_rd32(b + 12) : 0;
}

gptr gheap_realloc(gptr p, uint32_t n)
{
    if (!p) return gheap_alloc(n);
    uint32_t old = gheap_size(p);
    if (!old && p) { pal_log("gheap: realloc of bad pointer %08lx", (unsigned long)p); return 0; }
    gptr q = gheap_alloc(n);
    if (!q) return 0;
    uint8_t *dst = gmem_span(q, n, 1), *src = gmem_span(p, old, 0);
    if (dst && src) memcpy(dst, src, old < n ? old : n);
    gheap_free(p);
    return q;
}

uint32_t gheap_free_bytes(void) { return g_free; }
uint32_t gheap_peak_bytes(void) { return g_peak; }

/* ---- VirtualAlloc regions: simple table, first-fit in a 64 KB grid ---- */
#define VGRAN   0x10000u
#define MAXV    512
static gptr     v_lo, v_hi;
static struct { gptr a; uint32_t n; } v_reg[MAXV];
static int      v_count;

void gvirt_init(gptr lo, gptr hi) { v_lo = lo; v_hi = hi; v_count = 0; }

static int overlaps(gptr a, uint32_t n)
{
    for (int i = 0; i < v_count; i++)
        if (a < v_reg[i].a + v_reg[i].n && v_reg[i].a < a + n) return 1;
    return 0;
}

gptr gvirt_reserve(gptr want, uint32_t n)
{
    n = (n + VGRAN - 1) & ~(VGRAN - 1);
    if (v_count >= MAXV || !n) return 0;
    gptr a = 0;
    if (want) {
        want &= ~(VGRAN - 1);
        if (want >= v_lo && want + n <= v_hi && !overlaps(want, n)) a = want;
    } else {
        for (gptr t = v_lo; t + n <= v_hi; t += VGRAN)
            if (!overlaps(t, n)) { a = t; break; }
    }
    if (!a) { pal_log("gvirt: reserve %lu KB FAILED", (unsigned long)(n >> 10)); return 0; }
    v_reg[v_count].a = a;
    v_reg[v_count].n = n;
    v_count++;
    uint8_t *p = gmem_span(a, n, 1);
    if (p) memset(p, 0, n);
    return a;
}

int gvirt_release(gptr p)
{
    for (int i = 0; i < v_count; i++)
        if (v_reg[i].a == p) { v_reg[i] = v_reg[--v_count]; return 1; }
    return 0;
}

int gvirt_contains(gptr p, uint32_t n)
{
    for (int i = 0; i < v_count; i++)
        if (p >= v_reg[i].a && p + n <= v_reg[i].a + v_reg[i].n) return 1;
    return 0;
}

uint32_t gvirt_free_bytes(void)
{
    uint32_t used = 0;
    for (int i = 0; i < v_count; i++) used += v_reg[i].n;
    return (v_hi - v_lo) - used;
}
