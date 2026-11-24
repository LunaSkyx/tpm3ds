/* Native 0x00405cf0: visible-object collection (void __cdecl f(byte *view); once per frame, 16 % of the frame in the fights).
 *
 * Walks the portal graph the renderer found for the view: (1) the three sector ids at view+0xd, (2) the view's own
 * sector entries (view+0x38, 0x40 apart) that are flagged visible, then (3)/(4) two lists of loose objects that are handed
 * to 0x406300. For every object of a sector it applies the visibility filter and, if it passes, links the object into the
 * depth bucket lists (the body of native 0x406300, inlined here: the sector loops reach it ~100-300 times per frame).
 *
 * Transcribed from the x86 listing (Ghidra FUN_00405cf0). The branch with the global at 0x59df60 != 0 (distance fade through
 * x87 and __ftol) is not transcribed: the whole call then runs the lifted body, decided before anything is written.
 * Contract: EBX/EBP/ESI/EDI preserved (the original saves them), EAX/ECX/EDX scratch, ESP = entry + 4. No guest calls.
 * Host check: TPM_VERIFY_CO=1 runs native and lifted reference from the same memory image and compares the whole arena
 * (except the reference's own stack scratch just below ESP) and the registers. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_00405cf0_ref(cpu_t *c);
extern int g_native_off;

#define G_VIS      0x008bfaa0u      /* per-sector visit state: 0 new, 1 in progress, 2 done */
#define G_VIS2     0x008bc920u      /* per-object "already collected" flags */
#define G_HEADS    0x008bc880u      /* depth bucket list heads */
#define G_RECS     0x008a4880u      /* bucket records, 12 bytes: object, next, - */
#define G_NREC     0x0059debcu
#define G_MAXBUCK  0x0059df88u
#define G_MASK44   0x0059df44u
#define G_MASK54   0x0059df54u
#define G_FADE     0x0059df60u
#define G_SKIPCNT  0x0059df98u
#define G_ROOMS    0x0059df58u

#define R8(a)   gmem_rd8_l(gm, gsz, (a))
#define R16(a)  gmem_rd16_l(gm, gsz, (a))
#define R32(a)  gmem_rd32_l(gm, gsz, (a))
#define W8(a, v)  gmem_wr8_l(gm, gsz, (a), (v))
#define W32(a, v) gmem_wr32_l(gm, gsz, (a), (v))

/* the tail shared by every path: colour byte, bucket choice, record link (G_FADE == 0) */
static inline void co_link(uint8_t *gm, uint32_t gsz, gptr o)
{
    const uint32_t bl = R8(o + 0x2bu);
    uint32_t al = 0, edx, edi;

    if (bl) { const uint32_t t = bl & 0x1fu; al = ((t >> 2) + t * 8u) & 0xffu; }
    W8(o + 0xeu, (uint8_t)al);
    edi = R32(G_MAXBUCK);
    edx = R8(o + 0x24u);
    if ((int32_t)edx >= (int32_t)edi) return;
    if (bl || al) {
        edx = edi;
        if (!al) { const uint32_t t = bl & 0x1fu; W8(o + 0xeu, (uint8_t)(((t << 3) & 0xffu) + (t >> 2))); }
    }
    {
        const uint32_t cnt = R32(G_NREC);
        const uint32_t head = R32(G_HEADS + edx * 4u);
        const uint32_t rec = cnt * 12u + G_RECS;
        W32(rec + 4u, head);
        W32(G_HEADS + edx * 4u, rec);
        W32(G_NREC, cnt + 1u);
        W32(rec, o);
    }
    if (R8(o + 0x2du) & 1u) W8(G_VIS2 + R16(o + 0x30u), 1u);
}

/* one object of a sector list (loops 1 and 2) */
static inline void co_sector_obj(uint8_t *gm, uint32_t gsz, gptr o, uint32_t m44, uint32_t m54)
{
    const uint32_t al = R8(o + 0x28u);
    if (!(al & 4u)) return;
    if (R8(o + 0x38u) & m44) return;
    if (m54 & R8(o + 0xcu)) return;
    if (al & 0x10u) return;
    if (R8(G_VIS + R8(o + 0x2au)) == 2u) return;
    if ((R8(o + 0x2du) & 1u) && R8(G_VIS2 + R16(o + 0x30u)) != 0u) return;
    co_link(gm, gsz, o);
}

/* 0x406300 for the loose-object lists (loops 3 and 4) */
static inline void co_loose_obj(uint8_t *gm, uint32_t gsz, gptr o)
{
    const uint32_t al = R8(o + 0x28u);
    if (R8(o + 0xcu) & R8(G_MASK54)) return;
    if (al & 0x10u) return;
    if ((al & 4u) && R8(G_VIS + R8(o + 0x2au)) == 2u) return;
    if ((R8(o + 0x2du) & 1u) && R8(G_VIS2 + R16(o + 0x30u)) != 0u) return;
    if (R8(o + 0x38u) & R8(G_MASK44)) { W32(G_SKIPCNT, R32(G_SKIPCNT) + 1u); return; }
    co_link(gm, gsz, o);
}

static void co_sector_list(uint8_t *gm, uint32_t gsz, gptr room, uint32_t m44, uint32_t m54)
{
    int32_t nsec = (int32_t)R32(room + 0x24u), k;
    gptr s = room + 0xd0u;
    for (k = 0; k < nsec; k++, s += 0x9cu) {
        int32_t n = (int32_t)R32(s), j;
        if (n > 0) {
            const gptr list = R32(s + 0x4cu);
            for (j = 0; j < n; j++) co_sector_obj(gm, gsz, R32(list + 4u * (uint32_t)j), m44, m54);
        }
    }
}

static void co_native(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const gptr E = cg->r[ESP];
    const gptr p = R32(E + 4u);
    const uint32_t m44 = R8(G_MASK44), m54 = R8(G_MASK54);
    uint32_t i, cnt;

    if (R8(p + 0xcu)) {                                   /* loop 1: the three sector ids at view+0xd */
        for (i = 0; i < 3; i++) {
            const uint32_t id = R8(p + 0xdu + i);
            if (id != 0 && R8(G_VIS + id) == 0) {
                const gptr room = R32(R32(G_ROOMS) + 0x624u + id * 4u);
                co_sector_list(gm, gsz, room, m44, m54);
                W8(G_VIS + id, 2);
            }
        }
    }
    cnt = R8(p);
    if ((R8(p + 1u) & 1u) && cnt != 0) {                  /* loop 2: the view's own sector entries */
        gptr e = p + 0x38u;
        for (i = 0; i < R8(p); i++, e += 0x40u) {
            uint32_t id;
            if (R16(e - 0x28u) == 0xffffu) continue;
            id = R8(e + 2u);
            if ((R8(e) & 0xcu) == 0) continue;
            {
                const gptr tab = R32(G_ROOMS);
                if (id >= R32(tab + 0x620u)) continue;
                if (R8(G_VIS + id) != 0) continue;
                W8(G_VIS + id, 1);
                {
                    const gptr room = R32(tab + 0x624u + id * 4u);
                    if (room == 0 || R32(room) == 0) continue;
                    W32(room + 0x64u, 1);
                    co_sector_list(gm, gsz, room, m44, m54);
                    W8(G_VIS + id, 2);
                }
            }
        }
    }
    if (R8(p + 1u) & 0x40u) {                             /* loop 3: loose objects of the sector-less entries */
        gptr e = p + 0x10u;
        for (i = 0; i < R8(p); i++, e += 0x40u) {
            uint32_t n, k;
            if (R16(e) != 0xffffu) continue;
            n = R8(e + 2u);
            if (n > 5u) continue;
            for (k = 0; k < R8(e + 2u); k++) {
                const gptr q = e + 8u + 0xcu * k;
                const gptr o = R32(q);
                if (R8(G_VIS2 + R32(q + 4u)) == 0 && o != 0 && R8(o + 0x25u) != 0) co_loose_obj(gm, gsz, o);
            }
        }
    }
    if (R8(p + 1u) & 0x80u) {                             /* loop 4: entries that are objects themselves */
        gptr e = p + 0x10u;
        for (i = 0; i < R8(p); i++, e += 0x40u)
            if (R16(e) != 0xffffu && (R8(e + 0x2cu) & 0x80u)) co_loose_obj(gm, gsz, e);
    }
    cg->r[ESP] = E + 4u;
}

#ifndef __3DS__
static int cv_verify = -1;
static uint64_t cv_checked, cv_differed;
#define CV_W1 0x008a4880u
#define CV_W1N 0x00028780u           /* records, heads, vis2, vis (up to 0x8cd000) */
#define CV_W2 0x0059de00u
#define CV_W2N 0x00000300u
static uint8_t cv_a1[CV_W1N], cv_a2[CV_W2N], cv_b1[CV_W1N], cv_b2[CV_W2N];
static uint8_t cv_col[2][8192];
static uint32_t cv_ncol[2];
static void cv_at_exit(void)
{
    fprintf(stderr, "visible-collect verify: %llu calls checked, %llu differed\n",
            (unsigned long long)cv_checked, (unsigned long long)cv_differed);
}
/* colour bytes of every object linked so far */
static uint32_t cv_cols(uint8_t *dst)
{
    const uint32_t n = gmem_rd32(G_NREC);
    uint32_t i, m = 0;
    for (i = 0; i < n && m < 8192u / 5u; i++) {
        const gptr o = gmem_rd32(G_RECS + 12u * i);
        memcpy(dst + 5u * m, &o, 4);
        dst[5u * m + 4u] = gmem_rd8(o + 0xeu);
        m++;
    }
    return m;
}
static void co_shadow(cpu_t *cg)
{
    const cpu_t c0 = *cg;
    cpu_t cn;
    uint8_t *const w1 = gmem_span(CV_W1, CV_W1N, 1), *const w2 = gmem_span(CV_W2, CV_W2N, 1);
    int bad = 0;
    if (!w1 || !w2) { co_native(cg); return; }
    {   /* native first, then the reference from the same window state */
        static uint8_t s1[CV_W1N], s2[CV_W2N];
        memcpy(s1, w1, CV_W1N); memcpy(s2, w2, CV_W2N);
        co_native(cg);
        cn = *cg;
        memcpy(cv_a1, w1, CV_W1N); memcpy(cv_a2, w2, CV_W2N);
        cv_ncol[0] = cv_cols(cv_col[0]);
        memcpy(w1, s1, CV_W1N); memcpy(w2, s2, CV_W2N);
        *cg = c0;
        wmain_00405cf0_ref(cg);
        memcpy(cv_b1, w1, CV_W1N); memcpy(cv_b2, w2, CV_W2N);
        cv_ncol[1] = cv_cols(cv_col[1]);
    }
    if (memcmp(cv_a1, cv_b1, CV_W1N) || memcmp(cv_a2, cv_b2, CV_W2N)) {
        uint32_t k;
        for (k = 0; k < CV_W1N; k++) if (cv_a1[k] != cv_b1[k]) { if (cv_differed < 6) fprintf(stderr, "co verify: window1 +%x native %02x ref %02x\n", k, cv_a1[k], cv_b1[k]); break; }
        for (k = 0; k < CV_W2N; k++) if (cv_a2[k] != cv_b2[k]) { if (cv_differed < 6) fprintf(stderr, "co verify: window2 +%x native %02x ref %02x\n", k, cv_a2[k], cv_b2[k]); break; }
        bad = 1;
    }
    if (cv_ncol[0] != cv_ncol[1] || memcmp(cv_col[0], cv_col[1], 5u * cv_ncol[0])) {
        bad = 1;
        if (cv_differed < 6) fprintf(stderr, "co verify: linked objects / colour bytes differ (%u vs %u)\n", cv_ncol[0], cv_ncol[1]);
    }
    if (cn.r[EBX] != cg->r[EBX] || cn.r[ESI] != cg->r[ESI] || cn.r[EDI] != cg->r[EDI] || cn.r[EBP] != cg->r[EBP] || cn.r[ESP] != cg->r[ESP]) {
        bad = 1;
        if (cv_differed < 6) fprintf(stderr, "co verify: regs differ esp %08x/%08x\n", cn.r[ESP], cg->r[ESP]);
    }
    cv_checked++;
    if (bad) cv_differed++;
}
#endif

void wmain_00405cf0(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
#ifndef __3DS__
    if (cv_verify < 0) {
        const char *e = getenv("TPM_VERIFY_CO");
        cv_verify = (e && *e == '1');
        if (cv_verify) atexit(cv_at_exit);
    }
    if (getenv("TPM_AX_OFF")) { wmain_00405cf0_ref(cg); return; }
#endif
    if (g_native_off || R32(G_FADE) != 0u) { wmain_00405cf0_ref(cg); return; }
#ifndef __3DS__
    if (cv_verify) { co_shadow(cg); return; }
#endif
    co_native(cg);
}
