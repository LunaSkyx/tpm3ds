/* Native 0x00418544: updateCam (once per game frame, no arguments). The player's own update clamps a flying Ghost back into
 * the walkable area and hands that position to the camera follow target; so the Ghost position is written there again
 * right before the camera code runs, and the game's own easing then follows it smoothly. */
#include "core/cheats.h"
#include "core/cpu.h"

void wmain_00418544_ref(cpu_t *c);

void wmain_00418544(cpu_t *cg)
{
    cheats_pre_camera();
    wmain_00418544_ref(cg);
}
