/* Bottom-screen interface: a subtle dark touch panel with three tabs (Game settings, Cheats, Saves) that can be hidden
 * completely (the screen then stays black except for a faint corner dot that brings it back). Everything is drawn into a
 * 320x240 shadow buffer and copied to the bottom framebuffer before each swap (the screen is double buffered, so a change
 * is copied twice). State persists in sdmc:/3ds/tpm/ui.cfg. */
#include <3ds.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "core/cheats.h"
#include "core/audio/mixer.h"
#include "pal/pal.h"
#include "pal/3ds/ui_font.h"

#define SW 320
#define SH 240
#define CFG_PATH "sdmc:/3ds/tpm/ui.cfg"

extern void input_key(uint32_t dik, int down);
extern int  g_cam_sens;
extern const char *guest_save_dir(void);

static u16 g_sh[SW * SH];           /* column-major like the framebuffer: g_sh[x*SH + (SH-1-y)] */
static int g_dirty = 2;
static int g_on = 1, g_tab, g_fps_show, g_inited;
static int g_touch_down, g_tx, g_ty, g_drag = -1, g_inj_active, g_inj_x, g_inj_y;
static uint32_t g_frames, g_fps, g_fps_t, g_fps_n;
static char g_msg[48]; static uint32_t g_msg_until;
static int g_slots_stale = 1, g_nslots, g_slot_id[100]; static char g_slot_name[100][20]; static long g_slot_t[100];

/* ---- palette (RGB565) ---- */
static inline u16 rgb(int r, int g, int b) { return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
static inline void unrgb(u16 c, int *r, int *g, int *b) { *r = (c >> 11) << 3; *g = ((c >> 5) & 63) << 2; *b = (c & 31) << 3; }
#define C_BG     rgb(14, 17, 24)
#define C_PANEL  rgb(24, 29, 40)
#define C_PANEL2 rgb(33, 40, 54)
#define C_TEXT   rgb(214, 220, 232)
#define C_DIM    rgb(112, 122, 142)
#define C_ACC    rgb(214, 170, 74)     /* muted gold */
#define C_ACCDIM rgb(120, 98, 48)

static void put(int x, int y, u16 c) { if ((unsigned)x < SW && (unsigned)y < SH) g_sh[x * SH + (SH - 1 - y)] = c; }
static void blend(int x, int y, u16 c, int a)
{
    if ((unsigned)x >= SW || (unsigned)y >= SH || a <= 0) return;
    u16 *p = &g_sh[x * SH + (SH - 1 - y)];
    if (a >= 255) { *p = c; return; }
    int r0, g0, b0, r1, g1, b1; unrgb(*p, &r0, &g0, &b0); unrgb(c, &r1, &g1, &b1);
    *p = rgb(r0 + (r1 - r0) * a / 255, g0 + (g1 - g0) * a / 255, b0 + (b1 - b0) * a / 255);
}
static void rect(int x, int y, int w, int h, u16 c) { for (int i = 0; i < w; i++) for (int j = 0; j < h; j++) put(x + i, y + j, c); }
/* rounded rect, radius r, anti-aliased corners */
static void rrect(int x, int y, int w, int h, int r, u16 c)
{
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int dx = i < r ? r - 1 - i : i >= w - r ? i - (w - r) : -1, dy = j < r ? r - 1 - j : j >= h - r ? j - (h - r) : -1;
        int a = 255;
        if (dx >= 0 && dy >= 0) {
            int d2 = (dx * 2 + 1) * (dx * 2 + 1) + (dy * 2 + 1) * (dy * 2 + 1), R = 2 * r;
            if (d2 > R * R + 2 * R) a = 0; else if (d2 > R * R - 2 * R) a = 128;
        }
        if (a) blend(x + i, y + j, c, a);
    }
}

static int text_w(const ui_font_t *f, const char *s) { int w = 0; for (; *s; s++) if (*s >= 32 && *s < 127) w += f->g[*s - 32].adv; return w; }
static void text(const ui_font_t *f, int x, int y, const char *s, u16 c)
{
    for (; *s; s++) {
        if (*s < 32 || *s > 126) continue;
        const ui_glyph *g = &f->g[*s - 32];
        for (int j = 0; j < g->h; j++) for (int i = 0; i < g->w; i++) blend(x + g->xo + i, y + g->yo + j, c, f->a[g->off + j * g->w + i]);
        x += g->adv;
    }
}
static void text_c(const ui_font_t *f, int cx, int y, const char *s, u16 c) { text(f, cx - text_w(f, s) / 2, y, s, c); }

/* ---- layout ---- */
#define HDR 26
static const char *const k_tabs[] = { "Game", "Cheats", "Saves" };
static int hit(int x, int y, int w, int h) { return g_tx >= x && g_tx < x + w && g_ty >= y && g_ty < y + h; }

static void toast(const char *m) { snprintf(g_msg, sizeof g_msg, "%s", m); g_msg_until = g_frames + 120; g_dirty = 2; }

static void cfg_save(void)
{
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "on=%d cam=%d fps=%d tab=%d\n", g_on, g_cam_sens, g_fps_show, g_tab);
    fclose(f);
}
static void cfg_load(void)
{
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    int on, cam, fps, tab, vol, bri;
    char line[160] = "";
    if (fgets(line, sizeof line, f)) {
        if (sscanf(line, "on=%d vol=%d bri=%d cam=%d fps=%d tab=%d", &on, &vol, &bri, &cam, &fps, &tab) == 6 ||    /* the older format */
            sscanf(line, "on=%d cam=%d fps=%d tab=%d", &on, &cam, &fps, &tab) == 4) {
            g_on = on != 0;
            g_cam_sens = cam < 25 ? 25 : cam > 300 ? 300 : cam; g_fps_show = fps != 0; g_tab = tab < 0 || tab > 2 ? 0 : tab;
        }
    }
    fclose(f);
}

/* ---- widgets: each draws, and reacts when touched (tap = press edge, drag = held on a slider) ---- */
static int g_press;     /* 1 on the frame the finger lands */
static int g_widget;    /* running id, so a drag stays with the slider it started on */

static int slider(int x, int y, int w, const char *label, int v, int lo, int hi, const char *val)
{
    int id = ++g_widget;
    text(&ui_font_M, x, y, label, C_TEXT);
    int vw = text_w(&ui_font_M, val);
    text(&ui_font_M, x + w - vw, y, val, C_ACC);
    int ty = y + 20, tw = w - 8, tx = x + 4;
    if (g_touch_down && (g_drag == id || (g_press && hit(x, y + 10, w, 24)))) {
        g_drag = id;
        int nv = lo + (g_tx - tx) * (hi - lo) / tw;
        v = nv < lo ? lo : nv > hi ? hi : nv;
        g_dirty = 2;
    }
    rrect(tx, ty, tw, 4, 2, C_PANEL2);
    int fx = (v - lo) * tw / (hi - lo);
    rrect(tx, ty, fx < 4 ? 4 : fx, 4, 2, C_ACCDIM);
    rrect(tx + fx - 5, ty - 4, 10, 12, 4, C_ACC);
    return v;
}

static int toggle(int x, int y, int w, int h, const char *label, int on, int enabled)
{
    int hot = enabled && g_press && hit(x, y, w, h);
    rrect(x, y, w, h, 6, on ? C_ACCDIM : C_PANEL2);
    if (on) rrect(x + 1, y + 1, w - 2, h - 2, 5, rgb(44, 36, 20));
    text(&ui_font_M, x + 10, y + (h - ui_font_M.height) / 2 + 1, label, enabled ? C_TEXT : C_DIM);
    int px = x + w - 30, py = y + h / 2 - 6;
    rrect(px, py, 24, 12, 6, on ? C_ACC : rgb(60, 68, 86));
    rrect(px + (on ? 13 : 1), py + 1, 10, 10, 5, enabled ? rgb(240, 240, 240) : rgb(130, 134, 144));
    if (hot) { g_dirty = 2; return !on; }
    return on;
}

static int button(int x, int y, int w, int h, const char *label, int enabled)
{
    int hot = enabled && g_press && hit(x, y, w, h);
    rrect(x, y, w, h, 6, hot ? C_ACCDIM : C_PANEL2);
    text_c(&ui_font_M, x + w / 2, y + (h - ui_font_M.height) / 2 + 1, label, enabled ? C_TEXT : C_DIM);
    if (hot) g_dirty = 2;
    return hot;
}

static void tab_game(void)
{
    char v[16];
    int cur = cheats_brightness_get();      /* the game's own brightness option (like its options menu) */
    snprintf(v, sizeof v, "%d%%", cur);
    int nv = slider(16, 38, 288, "Brightness", cur, 0, 100, v);
    if (nv != cur) cheats_brightness_set(nv);
    snprintf(v, sizeof v, "%d%%", g_cam_sens);
    g_cam_sens = slider(16, 90, 288, "Camera speed (C-Stick)", g_cam_sens, 25, 300, v);
    int f = toggle(16, 142, 140, 30, "FPS counter", g_fps_show, 1);
    if (f != g_fps_show) g_fps_show = f;
    if (button(164, 142, 140, 30, "Hide panel", 1)) { g_on = 0; g_dirty = 2; cfg_save(); }
    if (button(16, 192, 288, 30, "Exit game", 1)) { extern void pal3ds_request_exit(void); cfg_save(); pal3ds_request_exit(); }
}

static void tab_cheats(void)
{
    int ok = cheats_in_level();
    struct { const char *n; int *f; } t[] = {
        { "God mode", &g_cheat.god }, { "Infinite ammo", &g_cheat.ammo }, { "Ghost (beta)", &g_cheat.fly },
        { "Slow motion", &g_cheat.slowmo }, { "Perfect aim", &g_cheat.aim },
    };
    for (int i = 0; i < 5; i++) {
        int x = 12 + (i & 1) * 152, y = 34 + (i >> 1) * 40;
        int nv = toggle(x, y, 144, 34, t[i].n, *t[i].f, ok);
        if (nv != *t[i].f) cheats_set(t[i].f, nv);
    }
    if (button(164, 114, 144, 34, "Heal", ok)) cheats_heal();
    if (button(12, 154, 144, 34, "All weapons", ok)) { cheats_all_weapons(); toast("All weapons given"); }
    char hb[40]; snprintf(hb, sizeof hb, "Hero: %s", cheats_hero_name());
    if (button(164, 154, 144, 34, hb, ok)) { cheats_next_hero(); toast("Switched hero"); }
    if (!ok) text_c(&ui_font_S, 160, 200, "Available while playing a level", C_DIM);
    else if (g_cheat.fly) text_c(&ui_font_S, 160, 200, "Ghost: B (jump) = up, ZR = down", C_DIM);
    else { char h[40]; snprintf(h, sizeof h, "Health %d", cheats_health()); text_c(&ui_font_S, 160, 200, h, C_DIM); }
}

static void tab_saves(void)
{
    text(&ui_font_M, 16, 36, "Game saves", C_TEXT);
    text(&ui_font_S, 16, 50, "Checkpoint saves in the game's own format.", C_DIM);
    if (button(16, 66, 288, 32, "Quick save", cheats_can_load())) { int ok = cheats_quick_save(); g_slots_stale = 1; toast(ok ? "Saved to slot 90" : "Save failed"); }
    text(&ui_font_S, 16, 108, cheats_can_load() ? "Tap a slot to load it" : "Saved slots (load while playing)", C_DIM);
    if (g_slots_stale) {                           /* the directory is scanned on entering the tab, not on every redraw */
        const char *dir = guest_save_dir();
        g_nslots = 0;
        for (int slot = 0; slot < 100 && dir && g_nslots < 100; slot++) {
            char p[300];
            snprintf(p, sizeof p, "%s/save/Zanzi%02d.sav", dir, slot);
            FILE *f = fopen(p, "rb");
            if (!f) continue;
            uint8_t h[0x38];
            size_t got = fread(h, 1, sizeof h, f);
            fclose(f);
            /* the game's own validity test (FUN_0045158f): magic, record size 0x1a8, version 2 / 3 */
            if (got < sizeof h || memcmp(h, "ZANZIBAR", 8) != 0 || *(uint32_t *)(h + 8) != 0x1a8u || h[0xc] != 2 || *(uint16_t *)(h + 0xe) != 3) continue;
            char *nm = g_slot_name[g_nslots];
            int k = 0;
            for (int i = 0; i < 24 && h[0x20 + i] && k < 17; i++) if (h[0x20 + i] >= 32 && h[0x20 + i] < 127) nm[k++] = (char)h[0x20 + i];
            if (h[0x20 + 17] && k == 17) { nm[15] = '.'; nm[16] = '.'; }
            nm[k] = 0;
            if (!k) snprintf(nm, sizeof g_slot_name[0], "Slot %d", slot);
            struct stat st; g_slot_t[g_nslots] = stat(p, &st) == 0 ? (long)st.st_mtime : 0;
            g_slot_id[g_nslots++] = slot;
        }
        for (int i = 1; i < g_nslots; i++) {                       /* newest save first (ties: higher slot number first) */
            int id = g_slot_id[i]; long t = g_slot_t[i]; char nm[20]; memcpy(nm, g_slot_name[i], sizeof nm);
            int j = i - 1;
            while (j >= 0 && (g_slot_t[j] < t || (g_slot_t[j] == t && g_slot_id[j] < id))) { g_slot_id[j + 1] = g_slot_id[j]; g_slot_t[j + 1] = g_slot_t[j]; memcpy(g_slot_name[j + 1], g_slot_name[j], sizeof nm); j--; }
            g_slot_id[j + 1] = id; g_slot_t[j + 1] = t; memcpy(g_slot_name[j + 1], nm, sizeof nm);
        }
        g_slots_stale = 0;
    }
    /* 8 slots per page (2 columns x 4 rows); Prev / Next below when there are more */
    enum { PER_PAGE = 8 };
    static int page;
    int pages = (g_nslots + PER_PAGE - 1) / PER_PAGE;
    if (g_slots_stale || page >= pages) page = 0;
    for (int n = 0; n < PER_PAGE && page * PER_PAGE + n < g_nslots; n++) {
        int k = page * PER_PAGE + n;
        int x = 16 + (n & 1) * 150, y = 124 + (n >> 1) * 22;
        if (button(x, y, 140, 20, g_slot_name[k], cheats_can_load())) {
            cheats_request_load(g_slot_id[k]); toast("Loading...");
        }
    }
    if (!g_nslots) text(&ui_font_S, 16, 124, "No saves yet.", C_DIM);
    if (pages > 1) {
        char pg[24]; snprintf(pg, sizeof pg, "Page %d / %d", page + 1, pages);
        if (button(16, 214, 90, 20, "< Prev", page > 0)) { page--; g_dirty = 2; }
        text_c(&ui_font_S, 160, 218, pg, C_DIM);
        if (button(214, 214, 90, 20, "Next >", page + 1 < pages)) { page++; g_dirty = 2; }
    }
}

static void draw_all(void)
{
    g_widget = 0;
    for (int i = 0; i < SW * SH; i++) g_sh[i] = C_BG;
    if (!g_on) {                                                   /* hidden: only a faint restore dot */
        rrect(SW - 20, SH - 20, 10, 10, 5, rgb(40, 46, 60));
        return;
    }
    rect(0, 0, SW, HDR, C_PANEL);
    int tw = 70;
    for (int i = 0; i < 3; i++) {
        int x = 8 + i * (tw + 4);
        if (g_press && hit(x, 0, tw, HDR) && g_tab != i) { g_tab = i; g_dirty = 2; g_drag = -1; g_slots_stale = 1; cfg_save(); }
        text_c(&ui_font_M, x + tw / 2, 6, k_tabs[i], g_tab == i ? C_ACC : C_DIM);
        if (g_tab == i) rect(x + 12, HDR - 3, tw - 24, 2, C_ACC);
    }
    char fp[24] = "";
    if (g_fps_show) snprintf(fp, sizeof fp, "%lu fps", (unsigned long)g_fps);
    text(&ui_font_S, SW - 60, 8, fp, C_DIM);
    if (g_press && hit(SW - 18, 0, 18, HDR)) { g_on = 0; g_dirty = 2; cfg_save(); }
    rrect(SW - 14, 9, 8, 8, 4, C_DIM);                             /* hide button */
    rrect(SW - 12, 11, 4, 4, 2, C_PANEL);
    if (g_tab == 0) tab_game(); else if (g_tab == 1) tab_cheats(); else tab_saves();
    if (g_msg_until > g_frames) text_c(&ui_font_S, 160, 226, g_msg, C_ACC);
}

/* Called once per input poll (after hidScanInput). */
void ui_poll(void)
{
    if (!g_inited) return;
    touchPosition tp = {0, 0};
    u32 held = hidKeysHeld();
    int down = (held & KEY_TOUCH) != 0;
    if (down) { hidTouchRead(&tp); g_tx = tp.px; g_ty = tp.py; }
    if (g_inj_active) { down = 1; g_tx = g_inj_x; g_ty = g_inj_y; }
    if (down && !g_touch_down) g_press = 1;
    if (!down) { if (g_drag >= 0) cfg_save(); g_drag = -1; }     /* a finished slider drag is saved */
    g_touch_down = down;
    if (!g_on) {
        if (g_press && g_tx >= SW - 40 && g_ty >= SH - 40) { g_on = 1; g_dirty = 2; cfg_save(); }
        g_press = 0;
        return;
    }
    if (g_press || (down && g_drag >= 0)) g_dirty = 2;
}

void ui_home_pressed(void) { toast("HOME is off: use Exit (Game tab)"); }
void ui_inject_touch(int x, int y, int down) { g_inj_active = down; g_inj_x = x; g_inj_y = y; }

/* Called right before each gfxSwapBuffers. */
void ui_frame(void)
{
    if (!g_inited) return;
    g_frames++;
    g_fps_n++;
    uint32_t now = pal_time_ms();
    if (now - g_fps_t >= 1000) { g_fps = g_fps_n * 1000u / (now - g_fps_t); g_fps_n = 0; g_fps_t = now; if (g_fps_show && g_on) g_dirty = 2; }
    if (g_msg_until == g_frames) g_dirty = 2;
    if (g_dirty > 0) {
        if (g_dirty == 2) { g_dirty = 0; draw_all(); g_press = 0; if (g_dirty) draw_all(); g_dirty = 2; }   /* a widget that changed state redraws once so it shows the new state */
        u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        memcpy(fb, g_sh, sizeof g_sh);
        GSPGPU_FlushDataCache(fb, sizeof g_sh);
        g_dirty--;
    }
}

int ui_save_bmp(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t row = SW * 3, size = 54 + row * SH;
    uint8_t h[54] = {'B', 'M'};
    memcpy(h + 2, &size, 4); h[10] = 54; h[14] = 40;
    int32_t w = SW, ht = SH; memcpy(h + 18, &w, 4); memcpy(h + 22, &ht, 4); h[26] = 1; h[28] = 24;
    fwrite(h, 1, 54, f);
    uint8_t line[SW * 3];
    for (int y = SH - 1; y >= 0; y--) {
        for (int x = 0; x < SW; x++) {
            int r, g, b; unrgb(g_sh[x * SH + (SH - 1 - y)], &r, &g, &b);
            line[x * 3] = (uint8_t)b; line[x * 3 + 1] = (uint8_t)g; line[x * 3 + 2] = (uint8_t)r;
        }
        fwrite(line, 1, sizeof line, f);
    }
    fclose(f);
    return 1;
}

/* Device bisect switches (empty files in sdmc:/3ds/tpm/): noui.txt = no bottom-screen UI at all, nohook.txt = no apt hook,
 * trace.txt = log every kernel call during suspend (DEBUG ONLY: it freezes HOME itself), nohome.txt = refuse the HOME press. */
static int sw(const char *name)
{
    char p[96]; snprintf(p, sizeof p, "sdmc:/3ds/tpm/%s", name);
    FILE *f = fopen(p, "rb"); if (f) { fclose(f); pal_log("switch: %s", name); return 1; }
    return 0;
}
extern int g_svc_notrace;
void ui_init(void)
{
    if (!sw("nohook.txt")) { extern void pal_apt_hook_init(void); pal_apt_hook_init(); }
    g_svc_notrace = !sw("trace.txt");     /* the suspend trace writes to the SD card mid-suspend, which stalls HOME: opt-in only */
    if (sw("nohome.txt")) { aptSetHomeAllowed(false); pal_log("apt: HOME disabled (nohome.txt)"); }
    if (sw("noui.txt")) return;                      /* g_inited stays 0: ui_poll / ui_frame do nothing */
    cfg_load();
    g_inited = 1;
    g_dirty = 2;
    g_press = 0;
    draw_all();
}
