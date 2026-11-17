/* Native float angle helpers: guest 0x004940ab (wrap to [0,360)), 0x0049418b
 * (wrap to (-180,180]) and 0x004941c0 (shortest signed difference).
 *
 * These three run ~700 times per frame each (host fnhist, frames 1800-2400) and
 * together execute ~4.9% of all lifted instructions (tools/icount). Almost all of
 * that is call-site state sync around a handful of x87 compares.
 *
 * Semantics are transcribed from source/gen/wmain_gen_026.c (the lifted bodies,
 * compiled in release builds as wmain_004xxxxx_ref, are the reference):
 *   - x87 values are C doubles, exactly like the lifter (fld dword = float->double,
 *     fstp dword rounds to float).
 *   - every `fcomp` becomes FCOM(), so the status-word condition bits - and thus
 *     cg->fsw, which the reference writes back - come out identical.
 *   - the result is left in ST(0): the x87 top is decremented and st[ftop-1] holds
 *     the value, as the reference's epilogue does.
 *   - registers: EAX ends as the last fnstsw (16 bits, upper half kept), ECX/EDX/
 *     EBX/ESI/EDI untouched, EBP and ESP as usual. Lazy flags and eflags are not
 *     written back by the reference, so the native leaves them alone.
 *
 * Fallback: the reference calls the CRT floor (0x49a480) whenever |angle| >= 360,
 * and that function fiddles with the control word and can raise FP exceptions. The
 * native handles only |angle| < 360 (the game's normal range) and hands the whole
 * call to the lifted body otherwise, before touching any state.
 *
 * Verification: tests/test_override.c runs native and reference on the same
 * randomized finite float inputs (0, negatives, 360 boundaries, large values) and
 * compares all registers, the lazy flag fields, fsw, fcw, ftop and the x87 stack.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "core/cpu.h"
#include "core/gmem.h"

void wmain_004940ab_ref(cpu_t *c);
void wmain_0049418b_ref(cpu_t *c);
void wmain_004941c0_ref(cpu_t *c);

/* A/B switch (same policy as the other natives): nonative.txt on the 3DS, or
 * TPM_AX_OFF in the host environment, runs the lifted bodies instead. */
static int na_ang_off(void)
{
    extern int g_native_off;
#ifdef __3DS__
    return g_native_off;
#else
    static int off = -1;
    if (off < 0) off = (g_native_off != 0) || getenv("TPM_AX_OFF") != NULL;
    return off;
#endif
}

#define GLD32(a) gmem_rd32_l(gm, gsz, (a))
/* Guest float at a guest address, as an x87 value (float -> double). */
static double gldf(const uint8_t *gm, uint32_t gsz, uint32_t a)
{
    uint32_t u = gmem_rd32_l(gm, gsz, a);
    float f;
    memcpy(&f, &u, 4);
    return (double)f;
}

enum { AW_NEG_SMALL, AW_POS_SMALL, AW_FALLBACK };

/* Decides the reference's path for x before any state is touched:
 *   0 -> 0 > x > -360   (the `360 - (-x)` branch, no CRT floor)
 *   1 -> 0 <= x < 360   (return x unchanged)
 *   2 -> |x| >= 360 or NaN (the CRT floor path -> run the lifted body) */
static int aw_path(double x)
{
    if (x > -360.0 && x < 0.0) return AW_NEG_SMALL;
    if (x >= 0.0 && x < 360.0) return AW_POS_SMALL;
    return AW_FALLBACK;
}

/* Shared body of 0x4940ab for the two floor-free paths. Keeps fsw and *eax (the
 * last `fnstsw ax`) identical to the reference and returns the st(0) value. */
static double angle_wrap_inrange(cpu_t *cg, uint32_t *eax, double x, int path)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    const double k0 = gldf(gm, gsz, 0x004a8e80u);       /* 0.0 */
    const double k360 = gldf(gm, gsz, 0x004a8e84u);     /* 360.0 */
    float res;

    FCOM(cg, x, k0);                                    /* 004940b4 */
    *eax = (*eax & 0xffff0000u) | (cpu_fsw_at(cg, cg->ftop) & 0xffffu);
    if (path == AW_NEG_SMALL) {
        double n = -x;                                  /* 004940c4: fchs */
        FCOM(cg, n, k360);                              /* 004940c9 */
        *eax = (*eax & 0xffff0000u) | (cpu_fsw_at(cg, cg->ftop) & 0xffffu);
        res = (float)(k360 - n);                        /* 004940d6: fld 360; fsub; fstp */
        if (gmem_u32_f(res) == 0x43b40000u) res = 0.0f; /* 00494153: exactly 360 -> 0 */
    } else {
        FCOM(cg, x, k360);                              /* 00494118 */
        *eax = (*eax & 0xffff0000u) | (cpu_fsw_at(cg, cg->ftop) & 0xffffu);
        return x;                                       /* 00494125: fld arg; return it */
    }
    return (double)res;
}

/* 0x004940ab */
void wmain_004940ab(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    uint32_t E, eax;
    double x;
    int path;

    if (na_ang_off()) { wmain_004940ab_ref(cg); return; }
    E = cg->r[ESP];
    eax = cg->r[EAX];
    x = gldf(gm, gsz, E + 4u);
    path = aw_path(x);
    if (path == AW_FALLBACK) { wmain_004940ab_ref(cg); return; }
    {
        double r = angle_wrap_inrange(cg, &eax, x, path);   /* stack still at the entry depth here */
        cg->ftop = (cg->ftop - 1u) & 7u;
        cg->st[cg->ftop] = r;
    }
    cg->r[EAX] = eax;
    cg->r[ESP] = E + 4u;
}

/* 0x0049418b: wrap to (-180, 180] with the reference's sign fix-up. */
void wmain_0049418b(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    uint32_t E, eax;
    double x, f;
    float r;
    int path;

    if (na_ang_off()) { wmain_0049418b_ref(cg); return; }
    E = cg->r[ESP];
    eax = GLD32(E + 4u);                                /* 0049418f: mov eax,[ebp+8]; push eax */
    x = gldf(gm, gsz, E + 4u);
    path = aw_path(x);
    if (path == AW_FALLBACK) { wmain_0049418b_ref(cg); return; }
    f = angle_wrap_inrange(cg, &eax, x, path);
    r = (float)f;                                       /* 0049419b: fst dword [ebp-4] */
    FCOM(cg, f, gldf(gm, gsz, 0x004a8e88u));            /* 0049419e */
    eax = (eax & 0xffff0000u) | (cpu_fsw_at(cg, cg->ftop) & 0xffffu);
    if (!(cg->fsw & 0x4100u))                           /* not (f < K3 or unordered) */
        r = (float)(-(gldf(gm, gsz, 0x004a8e84u) - (double)r));   /* 004941ab */
    cg->ftop = (cg->ftop - 1u) & 7u;
    cg->st[cg->ftop] = (double)r;
    cg->r[EAX] = eax;
    cg->r[ESP] = E + 4u;
}

/* 0x004941c0(a, b): shortest signed difference. */
void wmain_004941c0(cpu_t *cg)
{
    uint8_t *const gm = g_gmem;
    const uint32_t gsz = g_gmem_size;
    uint32_t E, eax, ecx, a1, b1;
    float fa, fb;
    double d, r;

    if (na_ang_off()) { wmain_004941c0_ref(cg); return; }
    E = cg->r[ESP];
    a1 = GLD32(E + 4u);
    eax = GLD32(E + 8u);                                /* 004941c6: mov eax,[ebp+0xc]; push eax */
    b1 = GLD32(E + 8u);
    memcpy(&fa, &a1, 4);
    memcpy(&fb, &b1, 4);
    /* both arguments must take a floor-free path, else run the lifted body */
    if (aw_path((double)fb) == AW_FALLBACK || aw_path((double)fa) == AW_FALLBACK) {
        wmain_004941c0_ref(cg);
        return;
    }
    fb = (float)angle_wrap_inrange(cg, &eax, (double)fb, aw_path((double)fb));   /* 004941ca */
    ecx = a1;                                           /* 004941d5: mov ecx,[ebp+8] */
    d = (double)fb - angle_wrap_inrange(cg, &eax, (double)fa, aw_path((double)fa));   /* 004941d9, fsubr */
    r = (double)(float)d;                               /* 004941e4: fst dword [ebp-4] */
    FCOM(cg, d, gldf(gm, gsz, 0x004a8e80u));            /* 004941e7 */
    eax = (eax & 0xffff0000u) | (cpu_fsw_at(cg, cg->ftop) & 0xffffu);
    if (cg->fsw & 0x0100u) {                            /* d < 0 */
        FCOM(cg, r, gldf(gm, gsz, 0x004a8e8cu));        /* 004941f7 */
        eax = (eax & 0xffff0000u) | (cpu_fsw_at(cg, cg->ftop) & 0xffffu);
        if (cg->fsw & 0x0100u) r = gldf(gm, gsz, 0x004a8e84u) + r;   /* 00494209 */
    } else {
        FCOM(cg, r, gldf(gm, gsz, 0x004a8e88u));        /* 00494217 */
        eax = (eax & 0xffff0000u) | (cpu_fsw_at(cg, cg->ftop) & 0xffffu);
        if (!(cg->fsw & 0x4100u)) r = -(gldf(gm, gsz, 0x004a8e84u) - r);   /* 00494229 */
    }
    cg->ftop = (cg->ftop - 1u) & 7u;
    cg->st[cg->ftop] = r;
    cg->r[EAX] = eax;
    cg->r[ECX] = ecx;
    cg->r[ESP] = E + 4u;
}
