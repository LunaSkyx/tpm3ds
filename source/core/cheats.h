/* In-game cheats, driven by the bottom-screen UI (pal/3ds/ui.c). They only read/write guest memory once per input pump,
 * the same way camera_look.c does; the addresses come from the game's own built-in cheat console (FUN_0042fc90):
 *   0x86d57c -> player stats { int health; ...; int cur_weapon at +8; int ammo[weapon] at +0x10 + 4*weapon }
 *   0x882288 slowmo toggle, 0x88228c "perfection" toggle (perfect aim)
 *   0x4b5220 -> { ... +0xc player 3D object (pos +0x18..0x20, heading +0x40, +0xa0 -> physics actor) }
 */
#ifndef CHEATS_H
#define CHEATS_H
#include <stdint.h>

typedef struct { int god, ammo, fly, slowmo, aim; } cheat_flags;
extern cheat_flags g_cheat;

void cheats_tick(void);                 /* once per input pump */
void cheats_set(int *flag, int on);     /* flag points into g_cheat */
void cheats_heal(void);
void cheats_all_weapons(void);
void cheats_next_hero(void);
int  cheats_in_level(void);             /* player stats exist (a level is running) */
int  cheats_health(void);               /* -1 outside a level */
const char *cheats_hero_name(void);
void cheats_pre_camera(void);        /* before updateCam: feed the camera the Ghost position */
int  cheats_can_load(void);          /* in a level, not paused, no cutscene: the game's own pause-key condition */
int  cheats_load_slot(int slot);      /* the game's own load of ZanziNN.sav, then the level restarts from it */
int  cheats_quick_save(void);          /* the game's own checkpoint save into slot 90; 0 outside a level */
int  cheats_brightness_get(void);       /* the game's own brightness (gamma) option, 0..100 */
void cheats_brightness_set(int pct);     /* set through the options-menu function */
#endif
