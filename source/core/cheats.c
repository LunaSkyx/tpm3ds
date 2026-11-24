#include <math.h>
#include <stdio.h>
#include <string.h>
#include "core/cheats.h"
#include "core/gmem.h"
#include "core/render/r3d.h"
#include "core/gheap.h"
#include "core/guest.h"
#include "core/input.h"
#include "pal/pal.h"

cheat_flags g_cheat;
#define G_FLY_SPEED 4.0f
static float g_view[16]; static int g_view_ok;
static gptr g_hits[24];
static int  g_nhits;
static int g_fly_armed;

#define G_STATS      0x0086d57cu
#define G_CHAR       0x0086d580u     /* playable character class (2 changes the weapon set in the game's own cheat) */
#define G_SLOWMO     0x00882288u
#define G_PERFECT    0x0088228cu
#define G_PLAYERROOT 0x004b5220u
#define G_NOPLAYER   0x004b5448u

extern int input_key_down(uint32_t dik);

static gptr valid(gptr p) { return (p >= 0x10000u && p < g_gmem_size - 0x400u) ? p : 0; }
static gptr stats(void)  { return valid(gmem_rd32(G_STATS)); }
static float rdf(gptr a) { uint32_t u = gmem_rd32(a); float f; memcpy(&f, &u, 4); return f; }
static void  wrf(gptr a, float f) { uint32_t u; memcpy(&u, &f, 4); gmem_wr32(a, u); }

/* The controlled object, or 0 (menus, cutscenes: the game points +0x60 at a dummy then). */
static gptr player_obj(void)
{
    gptr root = valid(gmem_rd32(G_PLAYERROOT));
    if (!root || gmem_rd32(root + 0x60) == G_NOPLAYER) return 0;
    return valid(gmem_rd32(root + 0xc));
}

int cheats_in_level(void) { return stats() != 0 && player_obj() != 0; }
int cheats_health(void)   { gptr s = stats(); return s ? (int32_t)gmem_rd32(s) : -1; }

void cheats_set(int *flag, int on)
{
    *flag = on != 0;
    g_fly_armed = 0;
    if (flag == &g_cheat.slowmo) gmem_wr32(G_SLOWMO, (uint32_t)(on != 0));
    if (flag == &g_cheat.aim)    gmem_wr32(G_PERFECT, (uint32_t)(on != 0));
}

void cheats_heal(void)
{
    gptr s = stats();
    if (s) gmem_wr32(s, 100u);
}

/* The weapon amounts of the game's own "i like to cheat" code (ammo of weapons 2..7, plus 10/11 for class 2). */
static void give_all(int top_up_only)
{
    gptr s = stats();
    if (!s) return;
    static const struct { int w; int amt; } k[] = { {2, 500}, {3, 500}, {4, 5}, {5, 1}, {6, 10}, {7, 10}, {10, 400}, {11, 400} };
    const int cls2 = gmem_rd32(G_CHAR) == 2u;
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        const int w = k[i].w;
        if (cls2 ? (w == 2 || w == 4) : (w == 10 || w == 11)) continue;
        gptr a = s + 0x10u + 4u * (uint32_t)w;
        int32_t cur = (int32_t)gmem_rd32(a);
        if (top_up_only ? (cur > 0 && cur < k[i].amt) : cur < k[i].amt) gmem_wr32(a, (uint32_t)k[i].amt);
    }
}
void cheats_all_weapons(void) { give_all(0); }

static int g_hero;
static const char *const k_hero[] = { "Obi-Wan", "Qui-Gon", "Captain Panaka", "Queen Amidala" };
const char *cheats_hero_name(void) { return k_hero[g_hero & 3]; }
/* FUN_004302aa(n): respawn the player as character n at the current position. */
void cheats_next_hero(void)
{
    if (!cheats_in_level()) return;
    g_hero = (g_hero + 1) & 3;
    uint32_t a[1] = { (uint32_t)g_hero };
    guest_callback(guest_cpu(), 0x004302aau, 1, a);
}

/* Ghost: noclip flight. The game's own movement keys are hidden from it (g_input_block_move) and the player object's
 * position (obj+0x18..0x20, previous position +0x54..) is driven here from the D-pad / Circle Pad relative to the camera
 * yaw, B (jump) up and ZR down. With no movement of its own the game has nothing to collide: walls and floors no longer stop it. */
extern int g_input_block_move;
#define G_CAMYAW 0x005bb534u
static float g_p[3];
#define p g_p
static void fly_tick(float dt)
{
    gptr obj = player_obj();
    static int nulls;
    /* The game briefly swaps the player for a dummy state (its out-of-level rescue, loading); only a long gap, such as a
     * real level load, makes the ghost take a fresh position from the game. */
    if (!obj) { if (++nulls > 90) g_fly_armed = 0; return; }
    nulls = 0;
    if (g_fly_armed && !(fabsf(p[0]) < 1e6f && fabsf(p[1]) < 1e6f && fabsf(p[2]) < 1e6f)) g_fly_armed = 0;
    if (!g_fly_armed) {
        p[0] = rdf(obj + 0x18u); p[1] = rdf(obj + 0x1cu); p[2] = rdf(obj + 0x20u); g_fly_armed = 1;
        /* The game keeps more than one copy of the player's position (the object reverts from another one each frame):
         * find every consecutive x,y,z float triple equal to it once, and drive them all. */
        g_nhits = 0;
        if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) { g_fly_armed = 0; return; }   /* no real position yet */
        uint32_t bx = gmem_rd32(obj + 0x18u), by = gmem_rd32(obj + 0x1cu), bz = gmem_rd32(obj + 0x20u);
        for (gptr q = 0x400000u; q < 0x1000000u && q + 12u < g_gmem_size && g_nhits < 24; q += 4u)
            if (gmem_rd32(q) == bx && gmem_rd32(q + 4u) == by && gmem_rd32(q + 8u) == bz && !(q >= 0x5bb000u && q < 0x5bc000u) && !(q >= 0x8a0000u && q < 0x8a1000u)) g_hits[g_nhits++] = q;   /* the camera's own copies (0x5bb4c8.., 0x8a0158) follow the player by themselves, smoothly */
        for (int i = 0; i < g_nhits; i++) pal_log("ghost: position copy at %08x", (unsigned)g_hits[i]);
    }
    float fwd  = (input_key_down(0xc8) ? 1.0f : 0.0f) - (input_key_down(0xd0) ? 1.0f : 0.0f);
    float side = (input_key_down(0xcd) ? 1.0f : 0.0f) - (input_key_down(0xcb) ? 1.0f : 0.0f);
    float up   = (input_key_down(0x2d) ? 1.0f : 0.0f) - (input_key_down(0x38) ? 1.0f : 0.0f);
    /* World axes: x, y horizontal, z up. "Forward" and "right" are the camera's own directions on the ground, taken from
     * the view matrix the game last rendered with (D3D row vectors: columns are the camera's right / up / look in world). */
    float fx = 0.0f, fy = 1.0f, rx = 1.0f, ry = 0.0f;
    if (g_view_ok) {
        float ux = g_view[1] + g_view[2], uy = g_view[5] + g_view[6];      /* ground direction of "screen up" and "look" */
        float ul = sqrtf(ux * ux + uy * uy), rl = sqrtf(g_view[0] * g_view[0] + g_view[4] * g_view[4]);
        if (ul > 1e-3f && rl > 1e-3f) { fx = ux / ul; fy = uy / ul; rx = g_view[0] / rl; ry = g_view[4] / rl; }
    }
    p[0] += (fx * fwd + rx * side) * G_FLY_SPEED * dt;
    p[1] += (fy * fwd + ry * side) * G_FLY_SPEED * dt;
    p[2] += up * G_FLY_SPEED * 0.6f * dt;
    for (int k = 0; k < 3; k++) { wrf(obj + 0x18u + 4u * (uint32_t)k, p[k]); wrf(obj + 0x54u + 4u * (uint32_t)k, p[k]); }
    for (int i = 0; i < g_nhits; i++) for (int k = 0; k < 3; k++) wrf(g_hits[i] + 4u * (uint32_t)k, p[k]);
    /* The camera's follow target (previous / current, read by updateCam FUN_00418544) stops tracking outside the level's
     * camera zones; feeding it the ghost position keeps the camera following, eased by the game's own smoothing. */
    for (int k = 0; k < 3; k++) { wrf(0x005bb4c8u + 4u * (uint32_t)k, p[k]); wrf(0x005bb4d8u + 4u * (uint32_t)k, p[k]); }
    gptr act = valid(gmem_rd32(obj + 0xa0u));
    if (act) { wrf(act + 0x20u, p[0]); wrf(act + 0x24u, p[1]); wrf(act + 0x28u, p[2]); }
}

#undef p
/* Called by r3d_begin_scene, after the game's logic and before it draws: the game clamps the player back into its
 * walkable area during its update, so the ghost position is written again here and the picture shows it. */
void cheats_pre_camera(void)
{
    if (!g_cheat.fly || !g_fly_armed || !player_obj()) return;
    for (int k = 0; k < 3; k++) { wrf(0x005bb4c8u + 4u * (uint32_t)k, g_p[k]); wrf(0x005bb4d8u + 4u * (uint32_t)k, g_p[k]); }
}

void cheats_pre_render(void)
{
    {   /* the last world view the game rendered with (3D scenes only: a rotation, not the identity) */
        const float *v = g_r3d.view;
        int ident = v[0] == 1.0f && v[5] == 1.0f && v[10] == 1.0f && v[1] == 0.0f && v[2] == 0.0f && v[4] == 0.0f;
        if (!ident && isfinite(v[9]) && fabsf(v[9]) > 0.3f) { memcpy(g_view, v, sizeof g_view); g_view_ok = 1; }   /* camera "up" points mostly along world z */
    }
    if (!g_cheat.fly || !g_fly_armed) return;
    gptr obj = player_obj();
    if (!obj) return;
    for (int k = 0; k < 3; k++) wrf(obj + 0x18u + 4u * (uint32_t)k, g_p[k]);
    for (int i = 0; i < g_nhits; i++) for (int k = 0; k < 3; k++) wrf(g_hits[i] + 4u * (uint32_t)k, g_p[k]);
}

/* A load requested from the touch UI. The UI is drawn from inside the present path (ui_frame <- pal_3ds_top_swap <- pal_gpu_present_finish,
 * with the present lock held), so running the loader there re-entered the present code on the same thread: the console froze on the
 * first Lock of the loading screen, later with "too many memory faults". The request is only recorded; cheats_tick() (called from the
 * game's input poll, a safe point on the guest thread) performs it. */
static volatile int g_load_req;   /* slot + 1 */
void cheats_request_load(int slot) { g_load_req = slot + 1; }

void cheats_tick(void)
{
    static uint32_t last;
    uint32_t now = pal_time_ms();
    float dt = last ? (float)(now - last) / 1000.0f : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    cheats_battle_tick();
    if (g_load_req) {
        int slot = g_load_req - 1;
        g_load_req = 0;
        int ok = cheats_load_slot(slot);
        pal_log("ui: load slot %d -> %s", slot, ok ? "ok" : "failed");
    }
    g_input_block_move = g_cheat.fly && player_obj() != 0;
    gptr s = stats();
    if (!s) return;
    { static uint32_t lg; if ((g_cheat.god | g_cheat.fly | g_cheat.ammo) && now - lg > 1000) { lg = now; gptr o = player_obj(); gptr ac = o ? valid(gmem_rd32(o + 0xa0u)) : 0;
        pal_log("cheats: health %d obj %08x pos %.1f %.1f %.1f yaw %.1f act %08x ammo2 %d | root %.1f %.1f %.1f cam %.1f %.1f %.1f", (int)gmem_rd32(s), (unsigned)o, o ? rdf(o + 0x18u) : 0.0f, o ? rdf(o + 0x1cu) : 0.0f, o ? rdf(o + 0x20u) : 0.0f, rdf(G_CAMYAW), (unsigned)ac, (int)gmem_rd32(s + 0x18u), rdf(0x6cf758u), rdf(0x6cf75cu), rdf(0x6cf760u), rdf(0x8a0158u), rdf(0x8a015cu), rdf(0x8a0160u)); } }
    if (g_cheat.god || g_cheat.fly) {
        /* No fall death: the airborne update (FUN_0044f162) starts the fall-death sequence (state +0x358 = 2, forced camera
         * mode 13 = the zoomed-out death camera) once the fall height +0x360 passes 8.0, and kills on landing past 6.0. The death
         * call itself is skipped (native_4500b0.c), but the camera would stay zoomed out: keep the height below both limits and
         * release a death camera that is already running. */
        gptr root = valid(gmem_rd32(G_PLAYERROOT));
        if (root) {
            if (rdf(root + 0x360u) > 4.0f) wrf(root + 0x360u, 4.0f);
            uint32_t st = gmem_rd32(root + 0x358u);
            if (st == 2u || st == 3u) gmem_wr32(root + 0x358u, 0u);
        }
        if (gmem_rd32(0x5bb4e8u) != 0u && gmem_rd32(0x5bb4f0u) == 13u) gmem_wr32(0x5bb4e8u, 0u);
    }
    if (g_cheat.god) {
        int32_t h = (int32_t)gmem_rd32(s);
        if (h < 100) gmem_wr32(s, 100u);   /* also at 0: flying out of the level is a kill zone */
    }
    if (g_cheat.ammo) give_all(1);
    if (g_cheat.slowmo) gmem_wr32(G_SLOWMO, 1u);
    if (g_cheat.aim)    gmem_wr32(G_PERFECT, 1u);
    if (g_cheat.fly)    fly_tick(dt);
}

/* Quick save: the game's own checkpoint save (FUN_00451531(slot, name, thumbnail surface) -> ZanziNN.sav in the save dir:
 * level, player position and heading, inventory), with a black 160x120 thumbnail made by the game's surface creator
 * FUN_0048f226. The slot then shows up in the game's Load screens. Loading goes through those screens (a level
 * restart needs the menu loop), so the UI opens the pause menu for it. */
#define QUICK_SLOT 90
#define BATTLE_SLOT 91   /* benchmark save: scenario `at N savebattle`, restored on boot by battle.txt */
static int quick_save_slot(int slot);
int cheats_quick_save(void) { return quick_save_slot(QUICK_SLOT); }
int cheats_save_battle(void) { return quick_save_slot(BATTLE_SLOT); }

/* battle.txt (sdmc:/3ds/tpm/battle.txt): skip logos and menu, start level 1, and as soon as the game allows a load
 * restore slot 91 (the position saved with the scenario command `at N savebattle`), so a measurement starts in the fight
 * within seconds. Stops the RETURN spam of skip mode afterwards. */
int g_battle_mode;
int g_battle_slot = BATTLE_SLOT;   /* battle.txt may contain another slot number (a hand-made save) */
void cheats_battle_tick(void)
{
    static int done, polls;
    extern int g_skip_level;
    extern uint32_t video_frame_count(void);
    if (g_battle_mode == 2 && !done && cheats_in_level()) {   /* battle_start.txt: normal level start, no load; leave the first fight alone */
        done = 1; g_skip_level = 0; input_key(0x1c, 0);
        pal_log("battle: level start (no load) at frame %lu", (unsigned long)video_frame_count());
        return;
    }
    if (g_battle_mode != 1 || done || !cheats_can_load()) return;
    if (++polls < 20) return;                       /* let the level settle for a few input pumps */
    done = 1;
    g_skip_level = 0;
    input_key(0x1c, 0);
    int ok = cheats_load_slot(g_battle_slot);
    pal_log("battle: slot %d load %s at frame %lu", g_battle_slot, ok ? "ok" : "FAILED (no save? run the scenario with `at N savebattle` first)",
            (unsigned long)video_frame_count());
}

static int quick_save_slot(int slot)
{
    if (!cheats_in_level()) return 0;
    static gptr scratch;
    if (!scratch) scratch = gvirt_reserve(0, 0x10000u);
    if (!scratch) return 0;
    /* surface descriptor as the game's own load screen builds it (FUN_0045ef3f): 160x120, 24 bpp, 8-8-8 */
    static const uint32_t desc[19] = { 160u, 120u, 0u, 0u, 0u, 1u, 0x18u, 8u, 8u, 8u, 0u, 8u, 0x10u, 0u, 0u, 0u, 0u, 0u, 0u };
    for (int i = 0; i < 19; i++) gmem_wr32(scratch + 4u * (uint32_t)i, desc[i]);
    const char *nm = "Quick save";
    for (int i = 0; i < 11; i++) gmem_wr8(scratch + 0x80u + (uint32_t)i, (uint8_t)nm[i]);
    uint32_t a1[4] = { scratch, 0, 0, 0 };
    uint32_t surf = guest_callback(guest_cpu(), 0x0048f226u, 4, a1);
    if (!surf) { pal_log("quick save: no thumbnail surface"); return 0; }
    gptr px = gmem_rd32(surf + 0x5cu);
    if (px) for (uint32_t i = 0; i < 0xe100u; i += 4u) gmem_wr32(px + i, 0u);
    uint32_t a2[3] = { slot, scratch + 0x80u, surf };
    guest_callback(guest_cpu(), 0x00451531u, 3, a2);
    uint32_t a3[1] = { surf };
    guest_callback(guest_cpu(), 0x0048f6f3u, 1, a3);
    pal_log("quick save: slot %d written", slot);
    return 1;
}

/* The game's own "in a level, not paused, not in a cutscene" test for its pause key (FUN_0043f603): state 0x881368 == 2 and the
 * player controller's flag at +0x394 clear, and no menu open (0x6ccfd8 / 0x6ccfe0). False on the main menu and in movies. */
int cheats_can_load(void)
{
    if (!cheats_in_level()) return 0;
    if (gmem_rd32(0x00881368u) != 2u || gmem_rd32(0x006ccfd8u) != 0u || gmem_rd32(0x006ccfe0u) != 0u) return 0;
    uint32_t root = gmem_rd32(0x004b5220u);
    if (root && gmem_rd32(root + 0x394u) != 0u) return 0;
    return 1;
}

/* Direct load, the sequence of the game's pause menu (FUN_0043fab5 / FUN_00445b25 / FUN_0045db7a) without its modal screen:
 * pause message 8, the loader FUN_00451936(slot) (reads ZanziNN.sav and loads the saved level itself), message 0x18 (loaded)
 * and resume message 9. */
int cheats_load_slot(int slot)
{
    if (!cheats_can_load()) return 0;
    cpu_t *c = guest_cpu();
    uint32_t pause_arg = gmem_rd32(0x00868714u);
    gmem_wr32(0x006ccfe0u, 1u);
    uint32_t m8[3] = { 0u, 8u, pause_arg };
    guest_callback(c, 0x0046f4a9u, 3, m8);
    gmem_wr32(0x006ccfd8u, 1u);
    uint32_t a[1] = { (uint32_t)slot };
    uint32_t r = guest_callback(c, 0x00451936u, 1, a);
    pal_log("load slot %d: loader returned %u, state %u", slot, (unsigned)r, (unsigned)gmem_rd32(0x00881368u));
    if (r == 0) { uint32_t b[2] = { 0u, 0x18u }; guest_callback(c, 0x0046f3c3u, 2, b); }
    gmem_wr32(0x006ccfd8u, 0u);
    uint32_t m9[3] = { 0u, 9u, pause_arg };
    guest_callback(c, 0x0046f4a9u, 3, m9);
    gmem_wr32(0x006ccfe0u, 0u);
    return r == 0;
}

/* Brightness: the game's own option. FUN_0046c000(p), p in 0..1, is what its options slider calls: gamma = 1.7 p + 0.3, kept
 * in 0x4b77f8 (float) and saved by the game in its config ("gamma", percent), so the game restores it by itself next start.
 * The default gamma 1.0 is p = 0.41. */
int cheats_brightness_get(void)
{
    uint32_t b = gmem_rd32(0x004b77f8u);
    float g; memcpy(&g, &b, sizeof g);
    if (!(g >= 0.3f && g <= 2.0f)) return 41;
    int pct = (int)((g - 0.3f) / 1.7f * 100.0f + 0.5f);
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}
void cheats_brightness_set(int pct)
{
    float p = (float)(pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100.0f;
    uint32_t a[1]; memcpy(a, &p, sizeof p);
    guest_callback(guest_cpu(), 0x0046c000u, 1, a);
}
