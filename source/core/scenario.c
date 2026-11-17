#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/input.h"
#include "core/prof.h"
#include "core/scenario.h"
#include "core/render/r3d.h"
#include "core/video/video.h"
#include "pal/pal.h"

#define MAXCMD 256
enum { C_KEY_DOWN, C_KEY_UP, C_SHOT, C_LOG, C_PROF_RESET, C_TOUCH, C_TOUCH_UP, C_UISHOT };
extern void ui_inject_touch(int x, int y, int down) __attribute__((weak));
extern int  ui_save_bmp(const char *path) __attribute__((weak));
typedef struct { uint32_t frame; int kind; uint32_t code; char text[96]; int done; } cmd;

static cmd      g_cmds[MAXCMD];
static int      g_n;
static uint32_t g_quit_after, g_expect_min;
static int      g_quit;
static char     g_out[256];

static void add(uint32_t frame, int kind, uint32_t code, const char *text)
{
    if (g_n >= MAXCMD) return;
    g_cmds[g_n] = (cmd){frame, kind, code, {0}, 0};
    snprintf(g_cmds[g_n].text, sizeof g_cmds[g_n].text, "%s", text ? text : "");
    g_n++;
}

int scenario_load(const char *path, const char *out_dir)
{
    g_n = 0; g_quit_after = 0; g_expect_min = 0; g_quit = 0;
    snprintf(g_out, sizeof g_out, "%s", out_dir ? out_dir : ".");
    pal_file *f = pal_file_open(path);
    if (!f) return 0;
    int64_t size = pal_file_size(f);
    char *buf = malloc((size_t)size + 1);
    if (!buf) { pal_file_close(f); return 0; }
    size_t got = pal_file_read_at(f, 0, buf, (size_t)size);
    pal_file_close(f);
    buf[got] = 0;
    int line_no = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        line_no++;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        unsigned n, tx, ty;
        char a[64], b[96], what[16];
        if (sscanf(line, "quit_after_frames %u", &n) == 1) g_quit_after = n;
        else if (sscanf(line, "expect_frames_min %u", &n) == 1) g_expect_min = n;
        else if (sscanf(line, "at %u key %63s %15s", &n, a, what) == 3) {
            uint32_t dik = input_dik_by_name(a);
            if (!dik) { pal_log("scenario:%d: unknown key '%s'", line_no, a); continue; }
            if (!strcmp(what, "down") || !strcmp(what, "tap")) add(n, C_KEY_DOWN, dik, a);
            if (!strcmp(what, "up")) add(n, C_KEY_UP, dik, a);
            if (!strcmp(what, "tap")) add(n + 2, C_KEY_UP, dik, a);
        } else if (sscanf(line, "at %u screenshot %95s", &n, b) == 2) { add(n, C_SHOT, 0, b); video_request_capture(n); }
        else if (sscanf(line, "at %u log %95[^\n]", &n, b) == 2) add(n, C_LOG, 0, b);
        else if (strstr(line, "prof_reset") && sscanf(line, "at %u prof_reset", &n) == 1) add(n, C_PROF_RESET, 0, NULL);
        else if (sscanf(line, "at %u touch %u %u", &n, &tx, &ty) == 3) add(n, C_TOUCH, (tx << 16) | (ty & 0xffff), NULL);
        else if (strstr(line, "touch_up") && sscanf(line, "at %u touch_up", &n) == 1) add(n, C_TOUCH_UP, 0, NULL);
        else if (sscanf(line, "at %u uishot %95s", &n, b) == 2) { add(n, C_UISHOT, 0, b); }
        else pal_log("scenario:%d: cannot parse '%s'", line_no, line);
    }
    free(buf);
    pal_log("scenario: %s: %d commands, quit after %lu frames", path, g_n, (unsigned long)g_quit_after);
    return 1;
}

void scenario_tick(void)
{
    uint32_t frame = video_frame_count();
    for (int i = 0; i < g_n; i++) {
        cmd *c = &g_cmds[i];
        if (c->done || c->frame > frame) continue;
        c->done = 1;
        switch (c->kind) {
        case C_KEY_DOWN: input_key(c->code, 1); pal_log("scenario: frame %lu key %s down", (unsigned long)frame, c->text); break;
        case C_KEY_UP:   input_key(c->code, 0); pal_log("scenario: frame %lu key %s up", (unsigned long)frame, c->text); break;
        case C_LOG:      pal_log("scenario: frame %lu: %s", (unsigned long)frame, c->text); break;
        case C_PROF_RESET: prof_reset(); pal_log("scenario: frame %lu: profiler reset", (unsigned long)frame); break;
        case C_TOUCH:    if (ui_inject_touch) ui_inject_touch((int)(c->code >> 16), (int)(c->code & 0xffff), 1); break;
        case C_TOUCH_UP: if (ui_inject_touch) ui_inject_touch(0, 0, 0); break;
        case C_UISHOT: {
            char path[400];
            snprintf(path, sizeof path, "%s/%s", g_out, c->text);
            pal_log("scenario: frame %lu ui screenshot %s -> %s", (unsigned long)frame, path, ui_save_bmp && ui_save_bmp(path) ? "ok" : "FAILED");
            break;
        }
        case C_SHOT: {
            char path[400];
            snprintf(path, sizeof path, "%s/%s", g_out, c->text);
            int ok = video_save_bmp(path);
            pal_log("scenario: frame %lu (scene %lu) screenshot %s -> %s", (unsigned long)frame, (unsigned long)g_r3d.scenes, path, ok ? "ok" : "FAILED");
            break;
        }
        }
    }
    if (g_quit_after && frame >= g_quit_after && !g_quit) {
        g_quit = 1;
        pal_log("scenario: quit after %lu frames", (unsigned long)frame);
    }
}

int scenario_quit_requested(void) { return g_quit; }

int scenario_check(void)
{
    uint32_t frames = video_frame_count();
    if (g_expect_min && frames < g_expect_min) {
        pal_log("scenario: FAIL: %lu frames presented, expected at least %lu", (unsigned long)frames,
                (unsigned long)g_expect_min);
        return 0;
    }
    return 1;
}
