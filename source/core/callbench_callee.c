/* Perf plan A1: callees for the call-marshalling micro benchmark (core/callbench.c).
 * Separate file so the compiler cannot see across the call, like most lifted calls.
 * The bodies copy the shape of generated code (tools/lifter/lift.py output):
 * live-in copy at entry, guest memory through GMEM locals, copy-out at ret. */
#include <stdint.h>
#include "core/cpu.h"
#include "core/gmem.h"

extern uint8_t g_cb_mem[];
#define CB_LOCAL uint8_t *const gm_base = g_cb_mem; const uint32_t gm_size = 0x10000u

/* Typical small leaf as generated today: mov eax,[esp+4]; mov ecx,[eax]; add eax,ecx; ret.
 * Copy-in of the read set, copy-out of the set the lifter writes back at ret (7 regs + eflags,
 * the most common size in wmain_gen: 5..9 entries). */
void cb_callee_full(cpu_t *cg)
{
    cpu_t lc, *const c = &lc;
    CB_LOCAL;
    lc.r[EAX] = cg->r[EAX]; lc.r[ECX] = cg->r[ECX]; lc.r[EDX] = cg->r[EDX]; lc.r[ESP] = cg->r[ESP]; lc.r[EBP] = cg->r[EBP];
    { uint32_t ea = (uint32_t)(R(ESP) + 0x4u); R(EAX) = GRD32(ea); }
    { uint32_t ea = (uint32_t)(R(EAX)); R(ECX) = GRD32(ea); }
    { uint32_t a = R(EAX), b = R(ECX), r = a + b; R(EAX) = r; lc.eflags = r; }
    { R(ESP) += 4u; cg->r[EAX] = lc.r[EAX]; cg->r[ECX] = lc.r[ECX]; cg->r[EDX] = lc.r[EDX]; cg->r[ESP] = lc.r[ESP]; cg->r[EBP] = lc.r[EBP]; cg->eflags = lc.eflags; return; }
}

/* The same function with a lean ABI (perf plan A2 target): only ESP in, EAX/ECX/ESP out
 * (ECX is clobbered by the body, EDX/EBP untouched, eflags dead after the call). */
void cb_callee_lean(cpu_t *cg)
{
    cpu_t lc, *const c = &lc;
    CB_LOCAL;
    lc.r[ESP] = cg->r[ESP];
    { uint32_t ea = (uint32_t)(R(ESP) + 0x4u); R(EAX) = GRD32(ea); }
    { uint32_t ea = (uint32_t)(R(EAX)); R(ECX) = GRD32(ea); }
    { uint32_t a = R(EAX), b = R(ECX), r = a + b; R(EAX) = r; }
    { R(ESP) += 4u; cg->r[EAX] = lc.r[EAX]; cg->r[ECX] = lc.r[ECX]; cg->r[ESP] = lc.r[ESP]; return; }
}

/* Empty callee (only the C call and ret) for the baseline. */
void cb_callee_empty(cpu_t *cg) { (void)cg; }
