#include <string.h>
#include "core/input.h"
#include "pal/pal.h"

void (*g_input_key_hook)(uint32_t dik, int down);
static uint8_t     g_keys[256];
static int32_t     g_mdx, g_mdy, g_mdz;
static uint8_t     g_mbtn[4];
static input_event g_buf[2][INPUT_BUF];
static uint32_t    g_head[2], g_tail[2], g_size[2] = {INPUT_BUF, INPUT_BUF}, g_seq;

void input_reset(void)
{
    memset(g_keys, 0, sizeof g_keys);
    g_mdx = g_mdy = g_mdz = 0;
    memset(g_mbtn, 0, sizeof g_mbtn);
    g_head[0] = g_tail[0] = g_head[1] = g_tail[1] = 0;
    g_size[0] = g_size[1] = INPUT_BUF;
    g_seq = 0;
}

static void push(int dev, uint32_t ofs, uint32_t data)
{
    if (g_tail[dev] - g_head[dev] >= g_size[dev]) return;          /* DI drops on overflow */
    input_event *e = &g_buf[dev][g_tail[dev]++ % INPUT_BUF];
    e->ofs = ofs;
    e->data = data;
    e->time = pal_time_ms();
    e->seq = ++g_seq;
}

int g_skip_level;   /* sdmc:/3ds/tpm/skip.txt: straight to level 1 (no movies, no menu) */

/* Press RETURN a few times early on so the menu and the level start immediately. */
int g_logo_open;
void input_skip_tick(void)
{
    static uint32_t last;
    extern uint32_t video_frame_count(void);
    uint32_t f;
    if (!g_skip_level) return;
    if (g_skip_level == 2) {         /* menu.txt: skip the movies only, then leave the menu alone */
        extern int hle_movie_open(void) __attribute__((weak));
        extern int g_menu_reached;
        input_key(0x1c, !g_menu_reached && hle_movie_open && hle_movie_open() > 0 && (pal_time_ms() % 200u) < 60u);
        return;
    }
    {   /* battle.txt: movies do not advance the frame counter, so the frame-paced spam below never skips them. Time-paced instead. */
        extern int g_battle_mode;
        if (g_battle_mode) { input_key(0x1c, (pal_time_ms() % 150u) < 50u); return; }
    }
    f = video_frame_count();
    if (f == last) return;
    last = f;
    /* Spam RETURN: the movie player and the menu both take a keypress to move on. */
    input_key(0x1c, (f % 12u) < 3u);
}

/* Ghost mode (core/cheats.c) drives the player itself: the movement keys are tracked here but not shown to the game. */
int g_input_block_move;
static uint8_t g_phys[256];
int input_key_down(uint32_t dik) { return dik < 256 && g_phys[dik] != 0; }
static int is_move_key(uint32_t d) { return d == 0xc8 || d == 0xd0 || d == 0xcb || d == 0xcd || d == 0x2d || d == 0x38; }

void input_key(uint32_t dik, int down)
{
    if (dik > 255) return;
    g_phys[dik] = down ? 1 : 0;
    if (down) { extern void video_movie_skip(void); video_movie_skip(); }
    if (g_input_block_move && is_move_key(dik)) down = 0;     /* the game sees them released */
    if ((g_keys[dik] != 0) == (down != 0)) return;
    g_keys[dik] = down ? 0x80 : 0;
    push(0, dik, down ? 0x80u : 0u);
    if (g_input_key_hook) g_input_key_hook(dik, down);
}

void input_mouse_move(int32_t dx, int32_t dy)
{
    g_mdx += dx;
    g_mdy += dy;
    if (dx) push(1, 0, (uint32_t)dx);
    if (dy) push(1, 4, (uint32_t)dy);
}

void input_mouse_button(uint32_t b, int down)
{
    if (b > 3 || (g_mbtn[b] != 0) == (down != 0)) return;
    g_mbtn[b] = down ? 0x80 : 0;
    push(1, 12 + b, down ? 0x80u : 0u);
}

const uint8_t *input_keys(void) { return g_keys; }

void input_mouse_take(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t buttons[4])
{
    *dx = g_mdx; *dy = g_mdy; *dz = g_mdz;
    g_mdx = g_mdy = g_mdz = 0;
    memcpy(buttons, g_mbtn, 4);
}

uint32_t input_take(int dev, input_event *out, uint32_t max, int peek)
{
    uint32_t n = 0, h = g_head[dev];
    while (n < max && h != g_tail[dev]) out[n++] = g_buf[dev][h++ % INPUT_BUF];
    if (!peek) g_head[dev] = h;
    return n;
}

void input_set_buffer_size(int dev, uint32_t n) { g_size[dev] = n == 0 ? 0 : n > INPUT_BUF ? INPUT_BUF : n; }

static const struct { const char *name; uint8_t dik; } k_dik[] = {
    {"ESCAPE", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06}, {"6", 0x07}, {"7", 0x08},
    {"8", 0x09}, {"9", 0x0a}, {"0", 0x0b}, {"MINUS", 0x0c}, {"EQUALS", 0x0d}, {"BACK", 0x0e}, {"TAB", 0x0f},
    {"Q", 0x10}, {"W", 0x11}, {"E", 0x12}, {"R", 0x13}, {"T", 0x14}, {"Y", 0x15}, {"U", 0x16}, {"I", 0x17},
    {"O", 0x18}, {"P", 0x19}, {"LBRACKET", 0x1a}, {"RBRACKET", 0x1b}, {"RETURN", 0x1c}, {"LCONTROL", 0x1d},
    {"A", 0x1e}, {"S", 0x1f}, {"D", 0x20}, {"F", 0x21}, {"G", 0x22}, {"H", 0x23}, {"J", 0x24}, {"K", 0x25},
    {"L", 0x26}, {"SEMICOLON", 0x27}, {"APOSTROPHE", 0x28}, {"GRAVE", 0x29}, {"LSHIFT", 0x2a}, {"BACKSLASH", 0x2b},
    {"Z", 0x2c}, {"X", 0x2d}, {"C", 0x2e}, {"V", 0x2f}, {"B", 0x30}, {"N", 0x31}, {"M", 0x32}, {"COMMA", 0x33},
    {"PERIOD", 0x34}, {"SLASH", 0x35}, {"RSHIFT", 0x36}, {"MULTIPLY", 0x37}, {"LMENU", 0x38}, {"SPACE", 0x39},
    {"CAPITAL", 0x3a}, {"F1", 0x3b}, {"F2", 0x3c}, {"F3", 0x3d}, {"F4", 0x3e}, {"F5", 0x3f}, {"F6", 0x40},
    {"F7", 0x41}, {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44}, {"NUMPAD8", 0x48}, {"NUMPAD4", 0x4b},
    {"NUMPAD6", 0x4d}, {"NUMPAD2", 0x50}, {"F11", 0x57}, {"F12", 0x58}, {"NUMPADENTER", 0x9c},
    {"RCONTROL", 0x9d}, {"RMENU", 0xb8}, {"HOME", 0xc7}, {"UP", 0xc8}, {"PRIOR", 0xc9}, {"LEFT", 0xcb},
    {"RIGHT", 0xcd}, {"END", 0xcf}, {"DOWN", 0xd0}, {"NEXT", 0xd1}, {"INSERT", 0xd2}, {"DELETE", 0xd3},
};

uint32_t input_dik_by_name(const char *name)
{
    for (unsigned i = 0; i < sizeof k_dik / sizeof k_dik[0]; i++)
        if (!strcmp(k_dik[i].name, name)) return k_dik[i].dik;
    return 0;
}
