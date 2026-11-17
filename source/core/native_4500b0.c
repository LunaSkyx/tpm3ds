/* Native 0x004500b0: the player's death routine (called with the death kind for kills, falls and leaving the level).
 * While God mode or Ghost (noclip) is on from the bottom-screen UI the player cannot die: the call returns at once.
 * Otherwise the lifted body runs. cdecl void f(int kind): the caller pops the argument. */
#include "core/cheats.h"
#include "core/cpu.h"

void wmain_004500b0_ref(cpu_t *c);

void wmain_004500b0(cpu_t *cg)
{
    if (g_cheat.god || g_cheat.fly) { cg->r[ESP] += 4u; return; }
    wmain_004500b0_ref(cg);
}
