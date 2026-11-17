/* Differential test for the hand-written guest functions in
 * source/core/native_overrides.c.
 *
 * The lifter emits the original lifted body of every overridden address as
 * wmain_<va>_ref (Makefile.host defines TPM_OVERRIDE_REF for source/gen only).
 * For each function this test builds a randomized cpu_t and guest memory from
 * one fixed state, runs the reference and the hand-written function on separate
 * copies, and compares everything the guest can observe:
 *   all registers, eflags and the lazy-flag fields, the x87 stack/TOP/status/
 *   control words, and the touched guest memory (argument slots and buffers).
 * Edge cases (NaN, +-0, +-inf, denormals, huge/negative ints, unaligned
 * pointers, DF set) are mixed into every function's inputs.
 *
 * Float values compare bit for bit, except that a NaN produced on both sides
 * counts as equal: with two NaN operands the sign/payload chosen by the host
 * compiler is unspecified (the guest x87 would pick one, the double-based model
 * cannot promise which), so requiring identical NaN bits would test the host
 * compiler's operand order, not the guest semantics.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/gheap.h"
#include "core/guest.h"
#include "core/gmem.h"
#include "pal/pal.h"
#include "gen/wmain_gen.h"

#define DATA_SZ  0x2000u
#define STACK_SZ 0x10000u
#define WIN_SZ   0x100u

static uint32_t g_pass, g_fail;
static uint8_t  g_save_data[DATA_SZ], g_save_stack[STACK_SZ];
static uint8_t  g_res_data[DATA_SZ], g_res_stack[STACK_SZ];
static uint32_t g_data_addr, g_stack_addr, g_esp;
static uint8_t *g_dv, *g_sv;         /* host view of the two guest blocks */
static gptr     g_out_ptr;           /* float results of the current function */
static uint32_t g_out_n;

/* Image windows for functions whose writes are indexed into the guest image.
 * Only enabled by the setups that need them (g_use_wins). */
#define NWIN 5
static const uint32_t g_win_addr[NWIN] = { 0x59de00u, 0x8a4880u, 0x8bc880u, 0x8bc920u, 0x8bfaa0u };
static const uint32_t g_win_len[NWIN]  = { 0x200u,    0x400u,    0x400u,    0x4000u,   0x100u };
static uint8_t g_wsave[0x5000], g_wres[0x5000];
static int g_use_wins;
static uint32_t g_f32_regs;      /* registers that hold float bit patterns */

static void win_copy(uint8_t *dst)
{
    uint32_t o = 0;
    for (int k = 0; k < NWIN; k++) {
        memcpy(dst + o, gmem_span(g_win_addr[k], g_win_len[k], 1), g_win_len[k]);
        o += g_win_len[k];
    }
}

static void win_restore(uint8_t *src)
{
    uint32_t o = 0;
    for (int k = 0; k < NWIN; k++) {
        memcpy(gmem_span(g_win_addr[k], g_win_len[k], 1), src + o, g_win_len[k]);
        o += g_win_len[k];
    }
}

static int win_diff(const char *name, unsigned it)
{
    uint32_t o = 0;
    for (int k = 0; k < NWIN; k++) {
        const uint8_t *cur = gmem_span(g_win_addr[k], g_win_len[k], 0);
        for (uint32_t j = 0; j < g_win_len[k]; j++)
            if (cur[j] != g_wres[o + j]) {
                printf("FAIL %s it %u: image %08x+%x %02x/%02x\n", name, it,
                       g_win_addr[k], j, cur[j], g_wres[o + j]);
                return 1;
            }
        o += g_win_len[k];
    }
    return 0;
}

static const uint32_t g_fbits[] = {
    0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u,
    0x7fc00000u, 0x7fa00000u, 0xffc00000u, 0x00000001u, 0x80000001u, 0x007fffffu,
    0x00800000u, 0x7f7fffffu, 0xff7fffffu, 0x4f000000u, 0xcf000000u, 0x4f800000u,
    0x501502f9u, 0xd01502f9u, 0x7f000000u, 0x00000002u, 0x3f000000u, 0x40490fdbu,
    0x3eaaaaabu, 0xbeaaaaabu, 0x41200000u, 0xc1200000u,
};

static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x ? x : 0x12345678u;
    return *s;
}

static uint32_t rndf(uint32_t *s)
{
    uint32_t r = rnd(s);
    if ((r & 3u) == 0u) return g_fbits[(r >> 2) % (sizeof g_fbits / sizeof g_fbits[0])];
    if ((r & 3u) == 1u) return ((r >> 2) & 0x807fffffu) | 0x3f000000u;   /* 0.5..2 */
    return r;                                                          /* anything */
}

static void put_f(gptr a, uint32_t bits) { gmem_wr32(a, bits); }

/* random 0..3-byte offset: unaligned guest pointers */
static uint32_t ua(uint32_t *s) { return rnd(s) & 3u; }

static void save_mem(void)
{
    memcpy(g_save_data, g_dv, DATA_SZ);
    memcpy(g_save_stack, g_sv, STACK_SZ);
}

static void restore_mem(void)
{
    memcpy(g_dv, g_save_data, DATA_SZ);
    memcpy(g_sv, g_save_stack, STACK_SZ);
}

static void load_mem_results(void)
{
    memcpy(g_res_data, g_dv, DATA_SZ);
    memcpy(g_res_stack, g_sv, STACK_SZ);
}

static int bits_f32_eq(uint32_t ua, uint32_t ub)
{
    float fa, fb;
    if (ua == ub) return 1;
    memcpy(&fa, &ua, 4);
    memcpy(&fb, &ub, 4);
    return isnan(fa) && isnan(fb);
}

static int bits_f64_eq(uint64_t ua, uint64_t ub)
{
    double da, db;
    if (ua == ub) return 1;
    memcpy(&da, &ua, 8);
    memcpy(&db, &ub, 8);
    return isnan(da) && isnan(db);
}

static int mem_diff(const char *name, unsigned it)
{
    uint32_t off = g_esp - g_stack_addr;
    uint32_t oo = (uint32_t)(g_out_ptr - g_data_addr);
    uint32_t on = 4u * g_out_n;

    for (uint32_t k = 0; k < DATA_SZ; k++) {
        if (on && k >= oo && k < oo + on) continue;
        if (g_dv[k] != g_res_data[k]) {
            printf("FAIL %s it %u: data[%04x] %02x/%02x\n", name, it, k, g_dv[k], g_res_data[k]);
            return 1;
        }
    }
    for (uint32_t k = 0; k < on; k += 4u) {
        uint32_t ua, ub;
        memcpy(&ua, g_dv + oo + k, 4);
        memcpy(&ub, g_res_data + oo + k, 4);
        if (!bits_f32_eq(ua, ub)) {
            printf("FAIL %s it %u: out float [%08x] %08x/%08x\n", name, it, g_out_ptr + k, ua, ub);
            return 1;
        }
    }
    for (uint32_t k = 0; k < WIN_SZ; k++)
        if (g_sv[off + k] != g_res_stack[off + k]) {
            printf("FAIL %s it %u: stack[%02x] %02x/%02x\n", name, it, k, g_sv[off + k], g_res_stack[off + k]);
            return 1;
        }
    return 0;
}

static int cpu_diff(const char *name, unsigned it, const cpu_t *x, const cpu_t *y)
{
    for (int k = 0; k < 8; k++) {
        int same = (x->r[k] == y->r[k]);
        if (!same && ((g_f32_regs >> k) & 1u))
            same = bits_f32_eq(x->r[k], y->r[k]);   /* float bits: NaN payload varies */
        if (!same) {
            printf("FAIL %s it %u: r[%d] %08x/%08x\n", name, it, k, x->r[k], y->r[k]);
            return 1;
        }
    }
    if (x->fa != y->fa || x->fb != y->fb || x->fr != y->fr || x->fcf != y->fcf) {
        printf("FAIL %s it %u: lazy flags fa/fb/fr/fcf %08x,%08x,%08x,%08x vs %08x,%08x,%08x,%08x\n",
               name, it, x->fa, x->fb, x->fr, x->fcf, y->fa, y->fb, y->fr, y->fcf);
        return 1;
    }
    if (x->fop != y->fop || x->fbits != y->fbits) {
        printf("FAIL %s it %u: fop/fbits %u,%u vs %u,%u\n", name, it, x->fop, x->fbits, y->fop, y->fbits);
        return 1;
    }
    if (x->eflags != y->eflags) {
        printf("FAIL %s it %u: eflags %08x/%08x\n", name, it, x->eflags, y->eflags);
        return 1;
    }
    if (x->fs_base != y->fs_base) {
        printf("FAIL %s it %u: fs_base %08x/%08x\n", name, it, x->fs_base, y->fs_base);
        return 1;
    }
    if (x->ftop != y->ftop) {
        printf("FAIL %s it %u: ftop %u/%u\n", name, it, x->ftop, y->ftop);
        return 1;
    }
    if (x->fsw != y->fsw) {
        printf("FAIL %s it %u: fsw %04x/%04x\n", name, it, x->fsw, y->fsw);
        return 1;
    }
    if (x->fcw != y->fcw) {
        printf("FAIL %s it %u: fcw %04x/%04x\n", name, it, x->fcw, y->fcw);
        return 1;
    }
    for (int k = 0; k < 8; k++) {
        uint64_t ua, ub;
        memcpy(&ua, &x->st[k], 8);
        memcpy(&ub, &y->st[k], 8);
        if (!bits_f64_eq(ua, ub)) {
            printf("FAIL %s it %u: st[%d] %016llx/%016llx\n", name, it, k,
                   (unsigned long long)ua, (unsigned long long)ub);
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- state setup */
static void build_state(cpu_t *st, uint32_t *s, uint32_t esp)
{
    memset(st, 0, sizeof *st);
    for (int k = 0; k < 8; k++) st->r[k] = rnd(s);
    st->r[ESP] = esp;
    st->eflags = rnd(s) | (rnd(s) & 1u ? F_DF : 0u);
    st->fop = (uint8_t)(rnd(s) % 13u);
    st->fbits = (uint8_t)((rnd(s) & 1u) ? 8u : (rnd(s) & 1u) ? 16u : 32u);
    st->fa = rnd(s); st->fb = rnd(s); st->fr = rnd(s); st->fcf = rnd(s);
    st->ftop = rnd(s) & 7u;
    st->fsw = (uint16_t)rnd(s);
    switch (rnd(s) % 5u) {
    case 0: st->fcw = 0x037fu; break;
    case 1: st->fcw = 0x027fu; break;
    case 2: st->fcw = (uint16_t)(0x007fu | (rnd(s) & 0x0f00u)); break;
    case 3: st->fcw = (uint16_t)(0x037fu ^ (rnd(s) & 0x0c00u)); break;
    default: st->fcw = (uint16_t)rnd(s); break;
    }
    for (int k = 0; k < 8; k++) {
        uint32_t lo = rndf(s), hi = rnd(s);
        uint64_t u = (uint64_t)lo | (uint64_t)hi << 32;
        memcpy(&st->st[k], &u, 8);
        if ((rnd(s) & 7u) == 0u) {           /* common "nice" values */
            double v = (double)(int32_t)rnd(s) * 0.25;
            st->st[k] = v;
        }
    }
}

/* ------------------------------------------------------------- per-function */
typedef void (*gfn)(cpu_t *);

static void setup_004064b0(uint32_t *s)
{
    gptr recs = g_data_addr + 0x400u + ua(s);
    uint32_t i, k, count;

    for (i = 0; i < 0x200u; i += 4u) gmem_wr32(0x59de00u + i, rnd(s));
    gmem_wr32(0x59debcu, (rnd(s) & 7u) ? (rnd(s) % 0x100u) : 0x2000u + (rnd(s) & 0xffu));
    for (i = 0; i < 12u; i++) put_f(0x59dec8u + 4u * i, rndf(s));

    count = (rnd(s) & 3u) ? (uint8_t)(1u + rnd(s) % 6u) : 0u;
    gmem_wr8(recs, (uint8_t)count);
    for (i = 0; i < count; i++) {
        gptr r = recs + 0x10u + 0x40u * i;
        uint32_t f;
        for (k = 0; k < 0x40u; k += 4u) gmem_wr32(r + k, rnd(s));
        gmem_wr16(r, (rnd(s) & 7u) ? (uint16_t)rnd(s) : 0xffffu);
        gmem_wr8(r + 0x0cu, (uint8_t)rnd(s));
        gmem_wr8(r + 0x1cu, (uint8_t)rnd(s));
        gmem_wr8(r + 0x1du, (uint8_t)(rnd(s) % 5u));
        gmem_wr8(r + 0x1eu, (uint8_t)(rnd(s) % 5u));
        gmem_wr8(r + 0x1fu, (uint8_t)rnd(s));
        gmem_wr8(r + 0x24u, (uint8_t)rnd(s));
        f = rnd(s);
        if (rnd(s) & 1u) f |= 0x04u;
        if (rnd(s) & 1u) f |= 0x10u;
        gmem_wr8(r + 0x28u, (uint8_t)f);
        gmem_wr8(r + 0x2au, (uint8_t)rnd(s));
        gmem_wr8(r + 0x2bu, (uint8_t)rnd(s));
        f = rnd(s) & 0xffffu;
        if (rnd(s) & 1u) f |= 0x0080u;
        if (rnd(s) & 1u) f |= 0x0100u;
        gmem_wr16(r + 0x2cu, (uint16_t)f);
        gmem_wr16(r + 0x30u, (uint16_t)(rnd(s) % 0x3000u));
        gmem_wr8(r + 0x38u, (uint8_t)rnd(s));
    }

    for (i = 0; i < 0x100u; i++) gmem_wr32(0x8bc880u + 4u * i, g_data_addr + 0x1000u + (rnd(s) & 0xffu));
    for (i = 0; i < 0x3000u; i++) gmem_wr8(0x8bc920u + i, (uint8_t)(rnd(s) & 3u));
    for (i = 0; i < 0x100u; i++) gmem_wr8(0x8bfaa0u + i, (uint8_t)(rnd(s) & 3u));
    for (i = 0; i < 0x400u; i += 4u) gmem_wr32(0x8a4880u + i, rnd(s));

    gmem_wr32(g_esp + 4u, recs);
    gmem_wr32(g_esp + 8u, rndf(s));
    gmem_wr32(g_esp + 12u, rndf(s));
    g_out_n = 0;
    g_use_wins = 1;
}

static void setup_0047e40a(uint32_t *s)
{
    gptr out = g_data_addr + 0x100u + ua(s);
    gptr mp  = g_data_addr + 0x200u + ua(s);
    for (int k = 0; k < 3; k++) put_f(out + 4u * (uint32_t)k, rndf(s));
    for (int k = 0; k < 12; k++) put_f(mp + 4u * (uint32_t)k, rndf(s));
    gmem_wr32(g_esp + 4u, out);
    gmem_wr32(g_esp + 8u, mp);
    g_out_ptr = out; g_out_n = 3;
}


static void setup_0047e263(uint32_t *s)
{
    gptr out = g_data_addr + 0x100u + ua(s);
    gptr mp  = g_data_addr + 0x200u + ua(s);
    for (int k = 0; k < 3; k++) put_f(out + 4u * (uint32_t)k, rndf(s));
    for (int k = 0; k < 9; k++) put_f(mp + 4u * (uint32_t)k, rndf(s));
    gmem_wr32(g_esp + 4u, out);
    gmem_wr32(g_esp + 8u, mp);
    g_out_ptr = out; g_out_n = 3;
    g_f32_regs = (1u << ECX) | (1u << EDX);   /* ECX/EDX = out[2]/out[1] float bits */
}

static void setup_0047ceb9(uint32_t *s)
{
    gptr dst = g_data_addr + 0x100u + ua(s);
    gptr src = g_data_addr + 0x200u + ua(s);
    for (int k = 0; k < 12; k++) gmem_wr32(src + 4u * (uint32_t)k, rndf(s));
    for (int k = 0; k < 12; k++) gmem_wr32(dst + 4u * (uint32_t)k, rnd(s));
    gmem_wr32(g_esp + 4u, dst);
    gmem_wr32(g_esp + 8u, src);
    g_out_ptr = dst + 0x24u; g_out_n = 3;
}

static void setup_0047e34d(uint32_t *s)
{
    gptr out = g_data_addr + ua(s);
    gptr rp  = g_data_addr + 0x100u + ua(s);
    gptr mp  = g_data_addr + 0x200u + ua(s);
    for (int k = 0; k < 3; k++) put_f(rp + 4u * (uint32_t)k, rndf(s));
    for (int k = 0; k < 12; k++) put_f(mp + 4u * (uint32_t)k, rndf(s));
    gmem_wr32(g_esp + 4u, out);
    gmem_wr32(g_esp + 8u, rp);
    gmem_wr32(g_esp + 12u, mp);
    g_out_ptr = out; g_out_n = 3;
}

static void setup_004075c6(uint32_t *s)
{
    gptr a = g_data_addr + 0x100u + ua(s);
    gptr out = g_data_addr + ua(s);
    static const uint32_t recbase[3] = { 0x59dffcu, 0x5a5ffcu, 0x5adffcu };
    static const uint32_t seloff[3]  = { 0x1cu, 0x1du, 0x0cu };
    static const uint32_t cmpoff[3]  = { 0x1eu, 0x1fu, 0x1eu };
    int k;

    for (k = 0; k < 0x20; k++) gmem_wr8(a + (uint32_t)k, (uint8_t)rnd(s));
    put_f(a + 0x10u, rndf(s));
    put_f(a + 0x14u, rndf(s));
    put_f(a + 0x18u, rndf(s));
    for (k = 0; k < 3; k++) {
        uint32_t sel, base, r;
        if ((rnd(s) & 3u) == 0u) gmem_wr8(a + seloff[k], 0u);      /* exercise the skip path */
        sel = gmem_rd8(a + seloff[k]);
        base = recbase[k] + sel * 128u;
        put_f(base, rndf(s));
        put_f(base + 4u, rndf(s));
        put_f(base + 8u, rndf(s));
        put_f(base + 0x40u, rndf(s));
        r = rnd(s);
        gmem_wr32(base + 0x38u, (r & 1u) ? 1u : r);                /* often == 1 */
        gmem_wr32(base + 0x30u, (rnd(s) & 1u) ? (uint32_t)gmem_rd8(a + cmpoff[k]) : rnd(s));
    }
    gmem_wr32(g_esp + 4u, a);
    gmem_wr32(g_esp + 8u, out);
    gmem_wr32(g_esp + 12u, rndf(s));
    g_out_ptr = out; g_out_n = 4;
}

static void setup_004074d4(uint32_t *s)
{
    gptr p = g_data_addr + 0x100u + ua(s);
    for (int k = 0; k < 3; k++) put_f(p + 4u * (uint32_t)k, rndf(s));
    gmem_wr32(g_esp + 4u, p);
    g_out_n = 0;
}

static void setup_0040e96b(uint32_t *s)
{
    gptr src = g_data_addr + 0x100u + ua(s);
    gptr dst = g_data_addr + ua(s);
    uint32_t m = rnd(s);
    if (rnd(s) & 1u) m &= (rnd(s) & 1u) ? 0x7fu : 0xffu;
    for (int k = 0; k < 32; k++) gmem_wr32(src + 4u * (uint32_t)k, rnd(s));
    gmem_wr32(g_esp + 4u, src);
    gmem_wr32(g_esp + 8u, dst);
    gmem_wr32(g_esp + 12u, m);
    g_out_n = 0;                      /* raw 8-byte copies, not floats */
}

static void setup_0047dad6(uint32_t *s)
{
    gptr dst = g_data_addr + ua(s);
    gptr ap  = g_data_addr + 0x100u + ua(s);
    gptr bp  = g_data_addr + 0x200u + ua(s);
    for (int k = 0; k < 12; k++) put_f(ap + 4u * (uint32_t)k, rndf(s));
    for (int k = 0; k < 12; k++) put_f(bp + 4u * (uint32_t)k, rndf(s));
    gmem_wr32(g_esp + 4u, dst);
    gmem_wr32(g_esp + 8u, ap);
    gmem_wr32(g_esp + 12u, bp);
    g_out_ptr = dst; g_out_n = 12;
}

static void setup_0047dee9(uint32_t *s)
{
    gptr dst = g_data_addr + 0x100u + ua(s);
    gptr mp  = g_data_addr + 0x200u + ua(s);
    for (int k = 0; k < 12; k++) put_f(dst + 4u * (uint32_t)k, rndf(s));
    for (int k = 0; k < 12; k++) put_f(mp + 4u * (uint32_t)k, rndf(s));
    gmem_wr32(g_esp + 4u, dst);
    gmem_wr32(g_esp + 8u, mp);
    g_out_ptr = dst; g_out_n = 12;
}

static void setup_00486245(uint32_t *s)
{
    gptr v = g_data_addr + 0x100u + ua(s);
    for (int k = 0; k < 3; k++) {
        uint32_t r = rnd(s);
        put_f(v + 4u * (uint32_t)k, (r & 7u) ? (rndf(s) & 0xbf7fffffu) : rndf(s));
    }
    gmem_wr32(g_esp + 4u, v);
    gmem_wr32(0x8621f8u, 1u);           /* CRT sqrt: no FP-exception handler */
    g_out_ptr = v; g_out_n = 3;
}

static void setup_00487f90(uint32_t *s)
{
    gptr a = g_data_addr + 0x100u + ua(s);
    gptr b = g_data_addr + 0x200u + ua(s);
    for (int k = 0; k < 6; k++) put_f(a + 4u * (uint32_t)k, rndf(s));
    for (int k = 0; k < 6; k++) put_f(b + 4u * (uint32_t)k, rndf(s));
    gmem_wr32(a + 8u, rnd(s) & ((rnd(s) & 1u) ? 0x00100000u : 0xffffffffu));
    gmem_wr32(b + 8u, rnd(s) & ((rnd(s) & 1u) ? 0x00100000u : 0xffffffffu));
    gmem_wr32(a + 0x14u, rnd(s) & 3u);
    gmem_wr32(b + 0x14u, rnd(s) & 3u);
    gmem_wr32(g_esp + 4u, a);
    gmem_wr32(g_esp + 8u, b);
    g_out_n = 0;
}

static const uint32_t g_af_gf[] = { 0x5bf9e8u, 0x6bfa1cu, 0x5bb580u, 0x5bb614u,
                                    0x5bb5fcu, 0x5bb60cu, 0x5bb578u, 0x5bf9d4u,
                                    0x5bb600u, 0x5bf9d8u, 0x5bb604u, 0x5bb5f8u,
                                    0x5bb5b0u };
static const uint32_t g_af_gu[] = { 0x5bb5bcu, 0x6bfa14u, 0x866fc8u };

static void setup_0041af30(uint32_t *s)
{
    gptr src = g_data_addr + 0x100u + ua(s);
    gptr dst = g_data_addr + 0x200u + ua(s);
    for (int k = 0; k < 3; k++) put_f(src + 4u * (uint32_t)k, rndf(s));
    for (int k = 0; k < 8; k++) put_f(dst + 4u * (uint32_t)k, rndf(s));
    gmem_wr32(g_esp + 4u, src);
    gmem_wr32(g_esp + 8u, dst);
    for (unsigned k = 0; k < sizeof g_af_gf / sizeof g_af_gf[0]; k++) put_f(g_af_gf[k], rndf(s));
    for (unsigned k = 0; k < sizeof g_af_gu / sizeof g_af_gu[0]; k++) {
        uint32_t r = rnd(s);
        gmem_wr32(g_af_gu[k], (r & 3u) == 0u ? (r >> 2) & 1u : r);
    }
    g_out_ptr = dst; g_out_n = 8;
}

/* 0041ae50 animated-texture frame selector: world record, poly record, texture table. */
static uint32_t g_ae50[8];
static void dump_ae50(void)
{
    printf("  ae50: nframes=%u idx=%d ntex=%u period=%d time=%u flags=%02x frame=%u\n",
           g_ae50[0], (int)g_ae50[1], g_ae50[2], (int)g_ae50[3], g_ae50[4], g_ae50[5], g_ae50[6]);
}

static void setup_0041ae50(uint32_t *s)
{
    gptr w = g_data_addr + 0x100u;
    gptr p = g_data_addr + 0x400u;
    gptr tab = g_data_addr + 0x800u;
    uint32_t ntex = 1u + rnd(s) % 6u, i;
    int32_t nf, idx, per;

    g_ae50[2] = ntex;
    g_ae50[4] = rnd(s);
    gmem_wr32(w + 0x50u, g_ae50[4]);
    gmem_wr32(w + 0xb0u, ntex);
    gmem_wr32(w + 0xb8u, tab);
    for (i = 0; i < 8u; i++) gmem_wr32(tab + 0x28u * i + 0x20u, 1u + rnd(s) % 3u);

    per = (int32_t)(rnd(s) % 9u) - 4;
    if ((rnd(s) & 7u) == 0u) per = 0;                 /* period 0 -> forced to 1 */
    gmem_wr16(p + 0x32u, (uint16_t)(int16_t)per);
    g_ae50[3] = (uint32_t)per;
    nf = (int32_t)(2u + rnd(s) % 7u);
    if ((rnd(s) & 7u) == 0u) nf = (int32_t)(rnd(s) & 1u);   /* jbe early exit */
    gmem_wr8(p + 0x34u, (uint8_t)nf);
    g_ae50[0] = (uint32_t)nf;
    idx = (int32_t)(rnd(s) % (ntex + 3u)) - 1;
    if ((rnd(s) & 3u) == 0u) idx = -(int32_t)(rnd(s) % 3u);  /* jl early exit */
    gmem_wr16(p + 8u, (uint16_t)(int16_t)idx);
    g_ae50[1] = (uint32_t)idx;
    g_ae50[5] = (rnd(s) & 0xfu) | ((rnd(s) & 1u) ? 0x10u : 0u);
    gmem_wr8(p + 0x2cu, (uint8_t)g_ae50[5]);
    g_ae50[6] = rnd(s) & 0xffu;
    gmem_wr8(p + 0x35u, (uint8_t)g_ae50[6]);
    gmem_wr32(g_esp + 4u, w);
    gmem_wr32(g_esp + 8u, p);
}

static void setup_00401eb8(uint32_t *s)
{
    gptr o = g_data_addr + 0x200u;
    gptr obj = g_data_addr + 0x300u;
    uint32_t key = 1u + rnd(s) % 3u;
    uint32_t cnt = rnd(s) % 4u, k;

    for (k = 0; k < 4u; k++) {
        gmem_wr32(0x005956f8u + 12u * k, 1u + rnd(s) % 4u);
        gmem_wr32(0x005956f8u + 12u * k + 4u, 0u);
        gmem_wr32(0x005956f8u + 12u * k + 8u, 0u);
    }
    for (k = 0; k < cnt; k++) {                       /* existing chains */
        gptr head = g_data_addr + 0x400u + 8u * k;
        gmem_wr32(0x005956f8u + 12u * k + 4u, head);
        gmem_wr32(0x005956f8u + 12u * k + 8u, head);
        gmem_wr32(head, 0u);
    }
    gmem_wr32(0x005959f8u, cnt);
    gmem_wr32(0x004dd6c0u, (rnd(s) & 1u) && cnt ? 0x005956f8u + 12u * (rnd(s) % cnt) : 0u);
    gmem_wr32(0x004dd6e0u, rnd(s) % 7u);
    gmem_wr8(0x008bfba0u, (uint8_t)rnd(s));
    gmem_wr32(o, rnd(s));
    gmem_wr32(o + 4u, obj);
    gmem_wr8(o + 0x17u, (uint8_t)rnd(s));
    gmem_wr32(obj + 0x20u, key);
    gmem_wr32(g_esp + 4u, o);
}

/* Angle helpers 0x4940ab / 0x49418b / 0x4941c0: float arguments from a pool that
 * exercises both wrap paths (|x| >= 360), the exact 360 edge and NaNs/infs. */
static uint32_t ang_f32(uint32_t *s)
{
    /* In-range only: the reference hands |angle| >= 360 to the CRT floor, which
     * raises FP exceptions for the harness's randomized control word (the game runs
     * with inexact masked), and the native delegates that case to the reference
     * verbatim, so there is nothing to compare there. */
    static const float pool[] = { 0.0f, -0.0f, 1.0f, -1.0f, 179.5f, 180.0f, 359.5f, 359.99f,
                                  -359.5f, -359.99f, 90.0f, -90.0f };
    uint32_t u;
    switch (rnd(s) & 3u) {
    case 0: { float f = (float)((int32_t)(rnd(s) % 100000u) - 50000) * 0.0071f; memcpy(&u, &f, 4); break; }
    case 1: { float f = pool[rnd(s) % (sizeof pool / sizeof pool[0])]; memcpy(&u, &f, 4); break; }
    default: { float f = (float)((int32_t)(rnd(s) % 2000u) - 1000) * 0.37f; memcpy(&u, &f, 4); break; }
    }
    return u;
}
static void setup_004940ab(uint32_t *s) { gmem_wr32(g_esp + 4u, ang_f32(s)); }
static void setup_0049418b(uint32_t *s) { gmem_wr32(g_esp + 4u, ang_f32(s)); }
static void setup_004941c0(uint32_t *s) { gmem_wr32(g_esp + 4u, ang_f32(s)); gmem_wr32(g_esp + 8u, ang_f32(s)); }

/* 0x40f170: object with a part array; OR/AND a byte at a random offset. */
static void setup_0040f170(uint32_t *s)
{
    gptr obj = g_data_addr + 0x100u;
    gptr arr = g_data_addr + 0x200u;
    uint32_t n = rnd(s) % 5u, i;
    for (i = 0; i < n; i++) {
        gptr part = g_data_addr + 0x300u + 0x20u * i;
        gmem_wr32(arr + 4u * i, part);
        for (uint32_t k = 0; k < 8u; k++) gmem_wr8(part + k, (uint8_t)rnd(s));
    }
    gmem_wr32(obj + 0x14u, n);
    gmem_wr32(obj + 0x18u, arr);
    gmem_wr32(g_esp + 4u, obj);
    gmem_wr32(g_esp + 8u, rnd(s) % 16u);
}

/* 0x47cac0: rotation matrix from three angles; needs the game's x87 control word
 * for the native sincos fast path in both runs. */
static void setup_0047cac0(uint32_t *s)
{
    gptr out = g_data_addr + 0x100u;
    gptr ang = g_data_addr + 0x200u;
    gptr pos = g_data_addr + 0x300u;
    for (int k = 0; k < 3; k++) {
        float f = (float)((int32_t)(rnd(s) % 7200u) - 3600) * 0.1f;
        memcpy(g_dv + (ang - g_data_addr) + 4u * (uint32_t)k, &f, 4);
        put_f(pos + 4u * (uint32_t)k, rndf(s));
    }
    gmem_wr32(g_esp + 4u, out);
    gmem_wr32(g_esp + 8u, ang);
    gmem_wr32(g_esp + 12u, pos);
    g_out_ptr = out; g_out_n = 3;      /* 12 floats: compare all as floats */
}

static void setup_00494272(uint32_t *s) { gmem_wr32(g_esp + 4u, ang_f32(s)); }

/* 0x422130, the actor AI sweep, walks the 64 actor slots at 0x6bfa50 (stride 0x124).
 * Its single caller (0x0041fcbe) reloads EAX/ECX/EDX/ESP/EBP out of cg->r after the
 * call, so the native has to reproduce every integer register write of the listing and
 * the x87 slot writeback, not just the memory effects. Two shapes cover that contract:
 *   g_ai_mode 0: every slot inactive -> only the loop and the epilogue execute.
 *   g_ai_mode 1: every slot active through the `fl & 0x10` branch (0x42234d), which
 *     calls 0x41f700 and 0x422427; 0x20000 stays clear so 0x422427 returns at once and
 *     0x40000 stays set so 0x420db2 (the animation walk) is skipped. Both callees only
 *     touch the slots, the data block and 0x8a0200, all of which are compared or
 *     restored here.
 * The deeper branches (0x41f910 / 0x41f80b / 0x41fa37 / 0x447d18 / the sqrt /
 * 0x47e34d / 0x47840e) walk engine structures through pointers stored in the slot
 * fields, and there is no random input for those that both bodies survive - the same
 * reason the original override shipped without a per-function test. They are covered
 * by the 2400-frame golden A/B; the register and x87 effects at those call sites are
 * transcribed from docs/asm/00422130_listing.txt like the rest. */
static int g_ai_mode;

static void setup_00422130(uint32_t *s)
{
    uint32_t i, k;

    gmem_wr32(0x4abee0u, g_data_addr);       /* destination of 0x41f700's copy */
    gmem_wr32(0x8a0200u, rnd(s) % 3u);       /* the sweep's range result */
    gmem_wr32(0x6bfa48u, rnd(s));            /* stamped into [slot+0x118] */
    g_out_ptr = 0; g_out_n = 0;              /* compare the whole data block */

    for (i = 0; i < 0x40u; i++) {
        gptr slot = 0x6bfa50u + 0x124u * i;
        for (k = 0; k < 0x124u; k += 4u) gmem_wr32(slot + k, rnd(s));
        if (g_ai_mode == 0) {
            gmem_wr32(slot + 0x10u, 0u);                          /* inactive */
        } else {
            gmem_wr32(slot + 0x10u, 1u + (rnd(s) & 0xffffu));     /* active */
            /* 0x10 set -> 0x42234d; 0x10000 clear -> not the 0x4214bf branch;
             * 0x20000 clear -> 0x422427 returns early; 0x40000 set -> no 0x420db2 */
            gmem_wr32(slot + 0x14u, (rnd(s) & ~0x30000u) | 0x10u | 0x40000u);
        }
    }
}

/* Globals the functions read (randomized per iteration, compared after). */
static const uint32_t g_globals[] = {
    0x8621f8u,
    0x5bf9e8u, 0x6bfa1cu, 0x5bb580u, 0x5bb614u, 0x5bb5fcu, 0x5bb60cu, 0x5bb578u,
    0x5bf9d4u, 0x5bb600u, 0x5bf9d8u, 0x5bb604u, 0x5bb5f8u, 0x5bb5b0u,
    0x5bb5bcu, 0x6bfa14u, 0x866fc8u,
};

/* Compared regions: guest memory a test's function MAY modify. Saved before the
 * reference run, restored before the native run and compared afterwards (unlike
 * g_globals, which must come out unchanged and only guards inputs). */
typedef struct { uint32_t addr, len; } zr_region;

static const zr_region g_zr[] = {
    { 0x004dd6c0u, 4u }, { 0x004dd6e0u, 4u }, { 0x005959f8u, 4u },
    { 0x005956f8u, 0x30u }, { 0x008bfba0u, 4u },
};

/* Only the 0x422130 test needs these (the 64-slot actor array and the two globals the
 * sweep writes); keeping them out of g_zr avoids an 18 KB copy in every other case. */
static const zr_region g_zr_ai[] = {
    { 0x006bfa50u, 0x4900u }, { 0x008a0200u, 4u }, { 0x006bfa48u, 4u },
};

static int g_zr_ai_on;
static const zr_region *const g_zr_tab[2] = { g_zr, g_zr_ai };
static const unsigned g_zr_cnt[2] = {
    sizeof g_zr / sizeof g_zr[0], sizeof g_zr_ai / sizeof g_zr_ai[0]
};
#define ZR_SZ (0x40u + 0x4908u)
static uint8_t g_zsave[ZR_SZ], g_zres[ZR_SZ];

static unsigned zr_nregions(void) { return g_zr_ai_on ? 2u : 1u; }

static void zr_copy(uint8_t *dst)
{
    uint32_t o = 0;
    for (unsigned t = 0; t < zr_nregions(); t++)
        for (unsigned r = 0; r < g_zr_cnt[t]; r++)
            for (uint32_t k = 0; k < g_zr_tab[t][r].len; k++)
                dst[o++] = gmem_rd8(g_zr_tab[t][r].addr + k);
}
static void zr_restore(const uint8_t *src)
{
    uint32_t o = 0;
    for (unsigned t = 0; t < zr_nregions(); t++)
        for (unsigned r = 0; r < g_zr_cnt[t]; r++)
            for (uint32_t k = 0; k < g_zr_tab[t][r].len; k++)
                gmem_wr8(g_zr_tab[t][r].addr + k, src[o++]);
}
static int zr_diff(const char *name, unsigned it)
{
    uint32_t o = 0;
    for (unsigned t = 0; t < zr_nregions(); t++)
        for (unsigned r = 0; r < g_zr_cnt[t]; r++)
            for (uint32_t k = 0; k < g_zr_tab[t][r].len; k++, o++)
                if (gmem_rd8(g_zr_tab[t][r].addr + k) != g_zres[o]) {
                    printf("FAIL %s it %u: region %08x byte %u %02x/%02x\n", name, it,
                           g_zr_tab[t][r].addr + k, k, gmem_rd8(g_zr_tab[t][r].addr + k), g_zres[o]);
                    return 1;
                }
    return 0;
}
static uint32_t g_save_glob[sizeof g_globals / sizeof g_globals[0]];

static void save_globals(void)
{
    for (unsigned k = 0; k < sizeof g_globals / sizeof g_globals[0]; k++)
        g_save_glob[k] = gmem_rd32(g_globals[k]);
}

static void restore_globals(void)
{
    for (unsigned k = 0; k < sizeof g_globals / sizeof g_globals[0]; k++)
        gmem_wr32(g_globals[k], g_save_glob[k]);
}

static int glob_diff(const char *name, unsigned it)
{
    for (unsigned k = 0; k < sizeof g_globals / sizeof g_globals[0]; k++) {
        uint32_t v = gmem_rd32(g_globals[k]);
        if (v != g_save_glob[k]) {
            printf("FAIL %s it %u: global %08x %08x/%08x\n", name, it, g_globals[k], v, g_save_glob[k]);
            return 1;
        }
    }
    return 0;
}

/* Optional: the next check() runs with this x87 control word instead of the
 * randomized one. Functions that do x87 arithmetic need the game's 0x037f; with an
 * unmasked inexact/invalid exception the guest raises and stops. */
static uint16_t g_next_fcw;
/* Optional: the next check() also saves/restores/compares the actor-slot array
 * (g_zr_ai, ~18 KB) - only worth the copying for 0x422130. */
static int g_next_zr_ai;

static void check(const char *name, gfn ref, gfn nat, unsigned iters, void (*setup)(uint32_t *))
{
    const uint16_t forced_fcw = g_next_fcw;
    const int zr_ai = g_next_zr_ai;
    uint32_t seed = 0x9e3779b9u ^ (uint32_t)(uintptr_t)ref;
    unsigned pass = 0, fail = 0;
    g_use_wins = 0;
    g_f32_regs = 0u;
    g_next_fcw = 0;
    g_next_zr_ai = 0;
    g_zr_ai_on = zr_ai;

    for (unsigned it = 0; it < iters; it++) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t s = seed ? seed : 1u;

        for (uint32_t k = 0; k < DATA_SZ; k++) g_dv[k] = (uint8_t)rnd(&s);
        for (uint32_t k = 0; k < STACK_SZ; k++) g_sv[k] = (uint8_t)rnd(&s);
        gmem_wr32(g_esp, 0x00404040u + (rnd(&s) & 0xffu));    /* return address slot */
        setup(&s);
        save_globals();

        cpu_t base;
        build_state(&base, &s, g_esp);
        if (forced_fcw) base.fcw = forced_fcw;
        cpu_t a = base, b = base;

        save_mem();
        zr_copy(g_zsave);
        if (g_use_wins) win_copy(g_wsave);
        ref(&a);
        load_mem_results();
        zr_copy(g_zres);
        zr_restore(g_zsave);
        if (g_use_wins) win_copy(g_wres);
        cpu_t ra = a;

        restore_mem();
        restore_globals();
        if (g_use_wins) win_restore(g_wsave);
        nat(&b);

        if (cpu_diff(name, it, &ra, &b) || mem_diff(name, it) || glob_diff(name, it) || zr_diff(name, it) ||
            (g_use_wins && win_diff(name, it))) {
            if (!strcmp(name, "0041ae50")) dump_ae50();
            fail++;
            if (fail >= 5) { printf("FAIL %s: stopping after 5 failures\n", name); break; }
        } else {
            pass++;
        }
    }
    g_pass += pass;
    g_fail += fail;
    g_zr_ai_on = 0;
    printf("%s %s: %u passed, %u failed\n", fail ? "FAIL" : "PASS", name, pass, fail);
}

/* Optional micro-benchmark (TPM_OVR_BENCH=1): lifted vs hand-written ns/call. */
static void bench(const char *name, gfn ref, gfn nat, unsigned iters, void (*setup)(uint32_t *), uint32_t reset_a)
{
    uint32_t s = 0x1234abcd;
    for (uint32_t k = 0; k < DATA_SZ; k++) g_dv[k] = (uint8_t)rnd(&s);
    for (uint32_t k = 0; k < STACK_SZ; k++) g_sv[k] = (uint8_t)rnd(&s);
    setup(&s);
    cpu_t base;
    build_state(&base, &s, g_esp);
    cpu_t c;
    uint64_t t0, t1;
    unsigned i;

    c = base;
    t0 = pal_time_us();
    for (i = 0; i < iters; i++) { if (reset_a) { gmem_wr32(reset_a, 0u); gmem_wr8(gmem_rd32(g_esp + 4u), 4u); } c.r[ESP] = g_esp; ref(&c); }
    t1 = pal_time_us();
    double rns = (double)(t1 - t0) * 1000.0 / (double)iters;

    c = base;
    t0 = pal_time_us();
    for (i = 0; i < iters; i++) { if (reset_a) { gmem_wr32(reset_a, 0u); gmem_wr8(gmem_rd32(g_esp + 4u), 4u); } c.r[ESP] = g_esp; nat(&c); }
    t1 = pal_time_us();
    double nns = (double)(t1 - t0) * 1000.0 / (double)iters;
    printf("bench %s: lifted %.1f ns, native %.1f ns (%.2fx)\n", name, rns, nns, rns / nns);
}

int main(int argc, char **argv)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/GAMEDATA", argc > 1 ? argv[1] : "game_files");
    guest_config cfg = {0};
    cfg.data_dir = dir;
    cfg.save_dir = "build-host/save-test";
    cfg.max_ms = 5000;
    if (!guest_init(&cfg)) { printf("SKIP test_override: WMAIN.EXE not found under %s\n", dir); return 0; }

    g_data_addr = gheap_alloc(DATA_SZ);
    g_stack_addr = gheap_alloc(STACK_SZ);
    if (!g_data_addr || !g_stack_addr) { printf("FAIL: out of guest heap\n"); return 1; }
    g_dv = gmem_span(g_data_addr, DATA_SZ, 1);
    g_sv = gmem_span(g_stack_addr, STACK_SZ, 1);
    if (!g_dv || !g_sv) { printf("FAIL: guest blocks not mapped\n"); return 1; }
    g_esp = g_stack_addr + 0x8000u;

    check("004064b0", wmain_004064b0_ref, wmain_004064b0, 5000, setup_004064b0);
    check("0047e34d", wmain_0047e34d_ref, wmain_0047e34d, 5000, setup_0047e34d);
    check("004074d4", wmain_004074d4_ref, wmain_004074d4, 5000, setup_004074d4);
    check("004075c6", wmain_004075c6_ref, wmain_004075c6, 5000, setup_004075c6);
    check("0040e96b", wmain_0040e96b_ref, wmain_0040e96b, 5000, setup_0040e96b);
    check("0047e40a", wmain_0047e40a_ref, wmain_0047e40a, 5000, setup_0047e40a);
    check("0047e263", wmain_0047e263_ref, wmain_0047e263, 5000, setup_0047e263);
    check("0047ceb9", wmain_0047ceb9_ref, wmain_0047ceb9, 5000, setup_0047ceb9);
    check("0047dad6", wmain_0047dad6_ref, wmain_0047dad6, 5000, setup_0047dad6);
    check("0047dee9", wmain_0047dee9_ref, wmain_0047dee9, 5000, setup_0047dee9);
    check("00487f90", wmain_00487f90_ref, wmain_00487f90, 5000, setup_00487f90);
    check("0041af30", wmain_0041af30_ref, wmain_0041af30, 5000, setup_0041af30);
    check("00486245", wmain_00486245_ref, wmain_00486245, 1000, setup_00486245);
    check("0041ae50", wmain_0041ae50_ref, wmain_0041ae50, 5000, setup_0041ae50);
    check("00401eb8", wmain_00401eb8_ref, wmain_00401eb8, 5000, setup_00401eb8);
    { const char *only = getenv("TPM_ANG"); unsigned it = getenv("TPM_ANG_IT") ? (unsigned)atoi(getenv("TPM_ANG_IT")) : 5000;
      if (!only || !strcmp(only, "ab")) { g_next_fcw = 0x037f; check("004940ab", wmain_004940ab_ref, wmain_004940ab, it, setup_004940ab); }
      if (!only || !strcmp(only, "w"))  { g_next_fcw = 0x037f; check("0049418b", wmain_0049418b_ref, wmain_0049418b, it, setup_0049418b); }
      if (!only || !strcmp(only, "d"))  { g_next_fcw = 0x037f; check("004941c0", wmain_004941c0_ref, wmain_004941c0, it, setup_004941c0); }
      if (!only || !strcmp(only, "agg")) check("0040f170", wmain_0040f170_ref, wmain_0040f170, it, setup_0040f170);
      if (!only || !strcmp(only, "rot")) { g_next_fcw = 0x027f; check("0047cac0", wmain_0047cac0_ref, wmain_0047cac0, it, setup_0047cac0); }
      if (!only || !strcmp(only, "sind")) { g_next_fcw = 0x027f; check("00494272", wmain_00494272_ref, wmain_00494272, it, setup_00494272); }
      /* 0x422130: the register/x87 writeback contract at its single call site.
       * iters is small on purpose: the active shape runs two guest calls per actor. */
      if (!only || !strcmp(only, "ai")) {
          unsigned ai_it = getenv("TPM_AI_IT") ? (unsigned)atoi(getenv("TPM_AI_IT")) : 1500;
          g_next_zr_ai = 1; g_ai_mode = 0;
          check("00422130/inactive", wmain_00422130_ref, wmain_00422130, ai_it, setup_00422130);
          g_next_zr_ai = 1; g_ai_mode = 1;
          check("00422130/active", wmain_00422130_ref, wmain_00422130, ai_it, setup_00422130);
          g_ai_mode = 0;
      } }

    if (getenv("TPM_OVR_BENCH")) {
        bench("0047e34d", wmain_0047e34d_ref, wmain_0047e34d, 200000, setup_0047e34d, 0u);
        bench("004074d4", wmain_004074d4_ref, wmain_004074d4, 200000, setup_004074d4, 0u);
        bench("004075c6", wmain_004075c6_ref, wmain_004075c6, 200000, setup_004075c6, 0u);
        bench("004064b0", wmain_004064b0_ref, wmain_004064b0, 100000, setup_004064b0, 0x59debcu);
        bench("0040e96b", wmain_0040e96b_ref, wmain_0040e96b, 200000, setup_0040e96b, 0u);
        bench("0047e40a", wmain_0047e40a_ref, wmain_0047e40a, 200000, setup_0047e40a, 0u);
        bench("0047e263", wmain_0047e263_ref, wmain_0047e263, 200000, setup_0047e263, 0u);
        bench("0047ceb9", wmain_0047ceb9_ref, wmain_0047ceb9, 200000, setup_0047ceb9, 0u);
        bench("0047dad6", wmain_0047dad6_ref, wmain_0047dad6, 100000, setup_0047dad6, 0u);
        bench("0047dee9", wmain_0047dee9_ref, wmain_0047dee9, 100000, setup_0047dee9, 0u);
        bench("00487f90", wmain_00487f90_ref, wmain_00487f90, 200000, setup_00487f90, 0u);
        bench("0041af30", wmain_0041af30_ref, wmain_0041af30, 100000, setup_0041af30, 0u);
        bench("00486245", wmain_00486245_ref, wmain_00486245, 50000, setup_00486245, 0u);
    }

    guest_fini();
    printf("%s test_override: %u passed, %u failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail != 0;
}