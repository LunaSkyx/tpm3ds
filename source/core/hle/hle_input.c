/* Keyboard names (scancode set 1, US layout) and the platform input pump. */
#include <stdio.h>
#include <string.h>
#include "core/hle/hle_user32.h"
#include "core/guest.h"
#include "core/input.h"
#include "core/scenario.h"
#include "pal/pal.h"

static const char *const k_names[0x60] = {
    0, "Esc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "Backspace", "Tab",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "Enter", "Ctrl", "A", "S",
    "D", "F", "G", "H", "J", "K", "L", ";", "'", "`", "Shift", "\\", "Z", "X", "C", "V",
    "B", "N", "M", ",", ".", "/", "Right Shift", "Num *", "Alt", "Space", "Caps Lock", "F1", "F2", "F3",
    "F4", "F5", "F6", "F7", "F8", "F9", "F10", "Pause", "Scroll Lock", "Num 7", "Num 8", "Num 9", "Num -", "Num 4", "Num 5", "Num 6",
    "Num +", "Num 1", "Num 2", "Num 3", "Num 0", "Num Del", 0, 0, 0, "F11", "F12", 0, 0, 0, 0, 0,
};

void hle_key_name(uint32_t sc, char *out, uint32_t n)
{
    static const struct { uint32_t sc; const char *name; } ext[] = {
        {0x11c, "Num Enter"}, {0x11d, "Right Ctrl"}, {0x135, "Num /"}, {0x138, "Right Alt"},
        {0x147, "Home"}, {0x148, "Up"}, {0x149, "Page Up"}, {0x14b, "Left"}, {0x14d, "Right"},
        {0x14f, "End"}, {0x150, "Down"}, {0x151, "Page Down"}, {0x152, "Insert"}, {0x153, "Delete"},
    };
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (ext[i].sc == sc) { snprintf(out, n, "%s", ext[i].name); return; }
    if (sc < 0x60 && k_names[sc]) snprintf(out, n, "%s", k_names[sc]);
    else snprintf(out, n, "Key %03X", (unsigned)sc);
}

void camera_look_tick(void);   /* core/camera_look.c: C-Stick camera orbit */

/* Drains platform input into core/input, runs the scenario, honours quit requests. */
void hle_pump_platform(void)
{
    pal_input_event e;
    while (pal_input_next(&e)) {
        switch (e.type) {
        case PAL_IN_KEY:          input_key(e.code, e.a); break;
        case PAL_IN_MOUSE_MOVE:   input_mouse_move(e.a, e.b); break;
        case PAL_IN_MOUSE_BUTTON: input_mouse_button(e.code, e.a); break;
        case PAL_IN_QUIT:         guest_stop(guest_cpu(), GUEST_STOP_REQUESTED, "platform quit"); break;
        }
    }
    camera_look_tick();
    { extern void cheats_tick(void); cheats_tick(); }
    { extern void input_skip_tick(void); input_skip_tick(); }
    {   /* diagnostic: how many actor slots are alive, once a second */
        static uint32_t last;
        extern uint32_t video_frame_count(void);
        uint32_t f = video_frame_count();
        { extern int g_skip_level; if (!g_skip_level) { last = f; } }
        if (f + 1u >= last + 60u) {
            uint32_t i, n = 0;
            last = f;
            for (i = 0; i < 64u; i++)
                if (gmem_rd32(0x006bfa50u + 0x124u * i + 0x10u) != 0u) n++;
            pal_log("actors: %lu alive of 64 (frame %lu)", (unsigned long)n, (unsigned long)f);
            { extern void r3d_gpu_dbg_fvf_log(void); r3d_gpu_dbg_fvf_log(); }
        }
    }
    scenario_tick();
    if (scenario_quit_requested()) guest_stop(guest_cpu(), GUEST_STOP_REQUESTED, "scenario finished");
}

/* ---- keys as window messages: Windows delivers WM_KEYDOWN/WM_KEYUP/WM_CHAR to the
 * focused window alongside DirectInput (non-exclusive keyboard). ---- */
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP   0x0101u
#define WM_CHAR    0x0102u

/* DIK -> virtual key (US layout). Extended keys (0x9c..) are listed explicitly. */
static uint32_t dik_to_vk(uint32_t dik)
{
    static const uint8_t base[0x59] = {
        0, 0x1b, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xbd, 0xbb, 0x08, 0x09,
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xdb, 0xdd, 0x0d, 0x11, 'A', 'S',
        'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xba, 0xde, 0xc0, 0x10, 0xdc, 'Z', 'X', 'C', 'V',
        'B', 'N', 'M', 0xbc, 0xbe, 0xbf, 0x10, 0x6a, 0x12, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74,
        0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x67, 0x68, 0x69, 0x6d, 0x64, 0x65, 0x66, 0x6b, 0x61,
        0x62, 0x63, 0x60, 0x6e, 0, 0, 0, 0x7a, 0x7b,
    };
    switch (dik) {
    case 0x9c: return 0x0d; case 0x9d: return 0x11; case 0xb8: return 0x12;
    case 0xc7: return 0x24; case 0xc8: return 0x26; case 0xc9: return 0x21; case 0xcb: return 0x25;
    case 0xcd: return 0x27; case 0xcf: return 0x23; case 0xd0: return 0x28; case 0xd1: return 0x22;
    case 0xd2: return 0x2d; case 0xd3: return 0x2e;
    default: return dik < sizeof base ? base[dik] : 0;
    }
}

static uint8_t g_vk_down[256];
const uint8_t *hle_vk_state(void) { return g_vk_down; }

static void key_hook(uint32_t dik, int down)
{
    uint32_t vk = dik_to_vk(dik);
    if (!vk) return;
    g_vk_down[vk] = down ? 0x80 : 0;
    uint32_t lp = 1u | ((dik & 0x7fu) << 16) | ((dik & 0x80u) ? 1u << 24 : 0) | (down ? 0 : 0xc0000000u);
    hle_post_message(down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    if (!down) return;
    uint32_t ch = 0;
    if (vk == 0x0d || vk == 0x1b || vk == 0x08 || vk == 0x09 || vk == 0x20) ch = vk;
    else if (vk >= 'A' && vk <= 'Z') ch = vk + 32;
    else if (vk >= '0' && vk <= '9') ch = vk;
    if (ch) hle_post_message(WM_CHAR, ch, lp);
}

void hle_input_init(void)
{
    memset(g_vk_down, 0, sizeof g_vk_down);
    g_input_key_hook = key_hook;
}
