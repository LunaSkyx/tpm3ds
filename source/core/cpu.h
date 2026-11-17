/* Guest x86 CPU state + helpers used by lifted code (platform-free core).
 *
 * Calling model: every guest callable (lifted function or HLE import) is a
 * host function void f(cpu_t *). The caller pushes the guest return address
 * on the guest stack exactly like x86 does; the callee pops it at `ret`.
 * Indirect calls go through cpu_call(), which maps a guest address to a
 * lifted function or an import thunk.
 *
 * Flags are lazy: flag-writing instructions record (op, size, a, b, result)
 * and conditions are evaluated on demand. The lifter skips the record when
 * liveness proves nobody reads it, and fuses cmp/test+jcc where it can. */
#ifndef CPU_H
#define CPU_H

#include <math.h>
#include <stdint.h>
#include "core/gmem.h"

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

enum {                     /* lazy flag producers */
    FL_RAW,                /* flags are in c->eflags */
    FL_ADD, FL_SUB, FL_ADC, FL_SBB, FL_LOGIC, FL_INC, FL_DEC, FL_NEG,
    FL_SHL, FL_SHR, FL_SAR, FL_MUL /* CF=OF=fcf */
};

#define F_CF 0x001u
#define F_PF 0x004u
#define F_AF 0x010u
#define F_ZF 0x040u
#define F_SF 0x080u
#define F_DF 0x400u
#define F_OF 0x800u

typedef struct cpu {
    uint32_t r[8];
    /* lazy flags */
    uint32_t fa, fb, fr, fcf;
    uint8_t  fop, fbits;
    uint32_t eflags;       /* valid when fop == FL_RAW (plus DF always here) */
    uint32_t fs_base;      /* guest address of the TIB */
    /* x87 (doubles; see docs) */
    double   st[8];
    uint32_t ftop;
    uint16_t fsw, fcw;
} cpu_t;

#define R(i) (c->r[i])

/* ---- partial registers ---- */
GMEM_AI void set16(cpu_t *c, int i, uint32_t v) { c->r[i] = (c->r[i] & 0xffff0000u) | (v & 0xffffu); }
GMEM_AI void set8l(cpu_t *c, int i, uint32_t v) { c->r[i] = (c->r[i] & 0xffffff00u) | (v & 0xffu); }
GMEM_AI void set8h(cpu_t *c, int i, uint32_t v) { c->r[i] = (c->r[i] & 0xffff00ffu) | ((v & 0xffu) << 8); }

/* ---- guest stack ---- */
GMEM_AI void PUSH(cpu_t *c, uint32_t v) { c->r[ESP] -= 4; gmem_wr32(c->r[ESP], v); }
GMEM_AI uint32_t POP(cpu_t *c) { uint32_t v = gmem_rd32(c->r[ESP]); c->r[ESP] += 4; return v; }
/* lifted code (GMEM_LOCAL in scope) */
#define LPUSH(c, v) do { uint32_t v_ = (v); (c)->r[ESP] -= 4; GWR32((c)->r[ESP], v_); } while (0)
#define LPOP(c)     ((c)->r[ESP] += 4, GRD32((c)->r[ESP] - 4))

/* ---- lazy flags ---- */
GMEM_AI void FLAGS(cpu_t *c, uint8_t op, uint8_t bits, uint32_t a, uint32_t b, uint32_t r)
{
    c->fop = op; c->fbits = bits; c->fa = a; c->fb = b; c->fr = r;
}
GMEM_AI uint32_t fmask(const cpu_t *c) { return c->fbits == 32 ? 0xffffffffu : (1u << c->fbits) - 1; }
GMEM_AI uint32_t fsign(const cpu_t *c) { return 1u << (c->fbits - 1); }

GMEM_AI int cpu_cf(const cpu_t *c)
{
    uint32_t m = fmask(c), a = c->fa & m, b = c->fb & m, r = c->fr & m;
    switch (c->fop) {
    case FL_ADD:   return r < a;
    case FL_SUB:   return a < b;
    case FL_ADC:   return c->fcf ? r <= a : r < a;
    case FL_SBB:   return c->fcf ? a <= b : a < b;
    case FL_INC: case FL_DEC: case FL_MUL: return (int)c->fcf;
    case FL_NEG:   return a != 0;
    case FL_SHL:   return b <= c->fbits ? (int)((a >> (c->fbits - b)) & 1) : 0;
    case FL_SHR:   return (int)((a >> (b - 1)) & 1);
    case FL_SAR: {
        int32_t s = (int32_t)(a << (32 - c->fbits)) >> (32 - c->fbits);
        return (int)(((uint32_t)(s >> (b > 31 ? 31 : b - 1))) & 1);
    }
    case FL_LOGIC: return 0;
    default:       return (int)(c->eflags & F_CF);
    }
}
GMEM_AI int cpu_zf(const cpu_t *c) { return c->fop == FL_RAW ? !!(c->eflags & F_ZF) : (c->fr & fmask(c)) == 0; }
GMEM_AI int cpu_sf(const cpu_t *c) { return c->fop == FL_RAW ? !!(c->eflags & F_SF) : !!(c->fr & fsign(c)); }
GMEM_AI int cpu_pf(const cpu_t *c)
{
    if (c->fop == FL_RAW) return !!(c->eflags & F_PF);
    return !__builtin_parity(c->fr & 0xffu);
}
GMEM_AI int cpu_of(const cpu_t *c)
{
    uint32_t s = fsign(c), a = c->fa, b = c->fb, r = c->fr;
    switch (c->fop) {
    case FL_ADD: case FL_ADC: return !!(((a ^ r) & (b ^ r)) & s);
    case FL_SUB: case FL_SBB: return !!(((a ^ b) & (a ^ r)) & s);
    case FL_INC:   return (r & fmask(c)) == s;
    case FL_DEC:   return (r & fmask(c)) == s - 1;
    case FL_NEG:   return (r & fmask(c)) == s;
    case FL_SHL:   return !!((r & s) != 0) ^ cpu_cf(c);
    case FL_SHR:   return !!(a & s);
    case FL_SAR: case FL_LOGIC: return 0;
    case FL_MUL:   return (int)c->fcf;
    default:       return !!(c->eflags & F_OF);
    }
}
/* Collapse lazy state into c->eflags (for pushf/lahf/partial writers). */
static inline uint32_t cpu_flags(cpu_t *c)
{
    if (c->fop != FL_RAW) {
        uint32_t f = c->eflags & ~(F_CF | F_PF | F_ZF | F_SF | F_OF | F_AF);
        f |= cpu_cf(c) ? F_CF : 0; f |= cpu_pf(c) ? F_PF : 0; f |= cpu_zf(c) ? F_ZF : 0;
        f |= cpu_sf(c) ? F_SF : 0; f |= cpu_of(c) ? F_OF : 0;
        c->eflags = f;
        c->fop = FL_RAW;
    }
    return c->eflags | 0x2u;
}

/* condition codes, named after the jcc suffix */
#define CC_o(c)  (cpu_of(c))
#define CC_no(c) (!cpu_of(c))
#define CC_b(c)  (cpu_cf(c))
#define CC_ae(c) (!cpu_cf(c))
#define CC_e(c)  (cpu_zf(c))
#define CC_ne(c) (!cpu_zf(c))
#define CC_be(c) (cpu_cf(c) || cpu_zf(c))
#define CC_a(c)  (!cpu_cf(c) && !cpu_zf(c))
#define CC_s(c)  (cpu_sf(c))
#define CC_ns(c) (!cpu_sf(c))
#define CC_p(c)  (cpu_pf(c))
#define CC_np(c) (!cpu_pf(c))
#define CC_l(c)  (cpu_sf(c) != cpu_of(c))
#define CC_ge(c) (cpu_sf(c) == cpu_of(c))
#define CC_le(c) (cpu_zf(c) || cpu_sf(c) != cpu_of(c))
#define CC_g(c)  (!cpu_zf(c) && cpu_sf(c) == cpu_of(c))

/* ---- x87: values are doubles (rules: "x87 maths stays double");
 * precision control is ignored, rounding control is honoured for fist/frndint. */
#define ST(i) (c->st[(c->ftop + (i)) & 7u])
#define FSW_C0 0x0100u
#define FSW_C1 0x0200u
#define FSW_C2 0x0400u
#define FSW_C3 0x4000u

GMEM_AI void FPUSH(cpu_t *c, double v) { c->ftop = (c->ftop - 1) & 7u; c->st[c->ftop] = v; }
GMEM_AI void FPOP(cpu_t *c) { c->ftop = (c->ftop + 1) & 7u; }
GMEM_AI double frd32(gptr a) { uint32_t u = gmem_rd32(a); float f; memcpy(&f, &u, 4); return f; }
GMEM_AI double frd64(gptr a)
{
    uint64_t u = (uint64_t)gmem_rd32(a) | (uint64_t)gmem_rd32(a + 4) << 32;
    double d; memcpy(&d, &u, 8); return d;
}
GMEM_AI void fwr32(gptr a, double v) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); gmem_wr32(a, u); }
GMEM_AI void fwr64(gptr a, double v)
{
    uint64_t u; memcpy(&u, &v, 8); gmem_wr32(a, (uint32_t)u); gmem_wr32(a + 4, (uint32_t)(u >> 32));
}
/* lifted code (GMEM_LOCAL in scope): the same through the cached arena locals */
GMEM_AI float gmem_f32_l(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
GMEM_AI double gmem_f64_l(uint32_t lo, uint32_t hi) { uint64_t u = (uint64_t)lo | (uint64_t)hi << 32; double d; memcpy(&d, &u, 8); return d; }
GMEM_AI uint32_t gmem_u32_f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
GMEM_AI uint64_t gmem_u64_d(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
#define LFRD32(a)    ((double)gmem_f32_l(GRD32(a)))
#define LFRD64(a)    gmem_f64_l(GRD32(a), GRD32((a) + 4))
#define LFWR32(a, v) GWR32((a), gmem_u32_f((float)(v)))
#define LFWR64(a, v) do { uint64_t u_ = gmem_u64_d(v); gptr a_ = (a); GWR32(a_, (uint32_t)u_); GWR32(a_ + 4, (uint32_t)(u_ >> 32)); } while (0)
#define LGRD64S(a)   ((int64_t)((uint64_t)GRD32(a) | (uint64_t)GRD32((a) + 4) << 32))
#define LGWR64(a, v) do { uint64_t u_ = (v); gptr a_ = (a); GWR32(a_, (uint32_t)u_); GWR32(a_ + 4, (uint32_t)(u_ >> 32)); } while (0)
GMEM_AI int64_t grd64s(gptr a) { return (int64_t)((uint64_t)gmem_rd32(a) | (uint64_t)gmem_rd32(a + 4) << 32); }
GMEM_AI void gwr64(gptr a, uint64_t v) { gmem_wr32(a, (uint32_t)v); gmem_wr32(a + 4, (uint32_t)(v >> 32)); }

GMEM_AI void FCOM(cpu_t *c, double a, double b)
{
    uint16_t f = c->fsw & (uint16_t)~(FSW_C0 | FSW_C2 | FSW_C3);
    if (a != a || b != b) f |= FSW_C0 | FSW_C2 | FSW_C3;
    else if (a < b)       f |= FSW_C0;
    else if (a == b)      f |= FSW_C3;
    c->fsw = f;
}
GMEM_AI uint32_t cpu_fsw(const cpu_t *c) { return (c->fsw & ~0x3800u) | ((c->ftop & 7u) << 11); }
/* x87 in static mode (tools/lifter/fpu_static.py): slots are C locals, TOP is known at lift time */
#define FPUSH_S(slot, v) ((slot) = (v))
GMEM_AI uint32_t cpu_fsw_at(const cpu_t *c, uint32_t top) { return (c->fsw & ~0x3800u) | ((top & 7u) << 11); }


/* Rounding per the FCW rounding-control field. They take the control word, not
 * the CPU state: passing &lc out of line would force a lifted function's
 * registers to memory. */
double   cpu_fround(uint16_t fcw, double v);
uint32_t cpu_fist32_slow(uint16_t fcw, double v);
uint32_t cpu_fist16(uint16_t fcw, double v);
uint64_t cpu_fist64(uint16_t fcw, double v);
/* fist m32: round-to-nearest (the default control word) in range takes one VFP
 * convert on ARM; other modes, out of range and NaN take the slow path. */
GMEM_AI uint32_t cpu_fist32(uint16_t fcw, double v)
{
    if (!(fcw & 0x0c00u) && v > -2147483648.5 && v < 2147483647.5) {
#if defined(__arm__) && defined(__VFP_FP__) && !defined(__SOFTFP__)
        float t;
        uint32_t r;
        __asm__("vcvtr.s32.f64 %0, %P1" : "=t"(t) : "w"(v));   /* FPSCR: round to nearest */
        memcpy(&r, &t, 4);
        return r;
#else
        return (uint32_t)(int32_t)nearbyint(v);
#endif
    }
    return cpu_fist32_slow(fcw, v);
}
double   cpu_f80_rd(gptr a);
void     cpu_f80_wr(gptr a, double v);
void     cpu_fxam(cpu_t *c);
void     cpu_fsave(cpu_t *c, gptr a);                   /* fnsave (and re-init) */
void     cpu_frstor(cpu_t *c, gptr a);
void     cpu_finit(cpu_t *c);

/* ---- debug: ring of recently entered guest functions (host debug builds) ---- */
#ifdef CPU_FN_TRACE
extern uint32_t g_fn_ring[64];
extern uint32_t g_fn_ring_pos;
extern uint32_t g_fn_hist[0x100000];
#define CPU_FN_ENTER(c, a) (g_fn_ring[g_fn_ring_pos++ & 63u] = (a), g_fn_hist[((a) - 0x400000u) & 0xfffffu]++)
#elif defined(TPM_CALL_COUNT)
/* perf plan A1: lifted function entries, logged per frame with the r3d scene line */
extern uint32_t g_fn_calls;
#define CPU_FN_ENTER(c, a) (g_fn_calls++)
#else
#define CPU_FN_ENTER(c, a) ((void)0)
#endif
void cpu_log_fn_ring(int n);                  /* logs the last n entries (no-op without CPU_FN_TRACE) */

/* ---- diagnostics: inclusive timers around selected lifted functions
 * (tools/lifter FN_TIMERS, built with -DTPM_FN_TIMERS; core/prof.c) ---- */
uint64_t fn_timer_begin(void);
void     fn_timer_end(int idx, uint64_t t0);

/* ---- lifted functions run on a local copy (lc) of the shared state (cg) ---- */
#define CPU_SPILL  (*cg = lc)
#define CPU_RELOAD (lc = *cg)

/* ---- runtime (cpu.c) ---- */
typedef void (*guest_fn)(cpu_t *c);
typedef struct { uint32_t addr; guest_fn fn; } guest_fn_entry;

void cpu_call(cpu_t *c, uint32_t target);                 /* indirect call/jmp */
void cpu_unimpl(cpu_t *c, uint32_t pc, const char *what); /* instruction not lifted yet */
void cpu_trap(cpu_t *c, uint32_t pc, const char *what);   /* int3, bad switch index, ... */
void cpu_set_dispatch(const guest_fn_entry *tab, uint32_t n);
/* ext: called for targets not in the table (HLE thunks); trap: after a trap is logged
 * (guest runtime stops the guest; must not return if the code cannot continue). */
typedef void (*cpu_extern_fn)(cpu_t *c, uint32_t addr);
void cpu_set_hooks(cpu_extern_fn ext, cpu_extern_fn trap);
void     cpu_cpuid(cpu_t *c);                             /* a Pentium with FPU, TSC, CX8; no MMX */
uint64_t cpu_rdtsc(cpu_t *c);                            /* monotonic, 400 MHz-equivalent ticks */

#endif
