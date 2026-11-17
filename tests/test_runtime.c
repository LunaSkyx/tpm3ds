/* Runtime pieces under the lifted code. Regression tests:
 *  - guest threads: sleepers wake in time order, and a thread that polls through
 *    API calls lets due threads run (iMUSE init waited 4 s per try for its
 *    timer callback, 12 s per boot)
 *  - Miles double-buffered streams (Bink audio) play buffers alternately and
 *    count finished buffers
 *  - DirectSound-style looping voices wrap and report their position (iMUSE)
 *  - table (W) fog: pre-transformed geometry with specular alpha 0 was drawn
 *    fully fogged (black/green characters in level 1)
 *  - x87 float->int stores (fist): rounding per control word, integer
 *    indefinite outside the range (inline fast path for round-to-nearest)
 *  - game option hz60 finds its code in this exe and sets the game's flag */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "core/audio/mixer.h"
#include "core/game_options.h"
#include "core/gheap.h"
#include "core/gthread.h"
#include "core/guest.h"
#include "core/render/r3d.h"
#include "pal/pal.h"

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- threads ---- */
static char     g_order[8];
static int      g_norder;
static volatile int g_flag;

typedef struct { uint32_t ms; char tag; } sleeper;
static void sleeper_body(cpu_t *c, void *p)
{
    (void)c;
    sleeper *s = p;
    gthread_sleep(s->ms);
    g_order[g_norder++] = s->tag;
}
static void flag_body(cpu_t *c, void *p) { (void)c; (void)p; gthread_sleep(5); g_flag = 1; }

static void test_threads(void)
{
    static sleeper a = {60, 'A'}, b = {20, 'B'};
    g_norder = 0;
    gthread_create_host(sleeper_body, &a);
    gthread_create_host(sleeper_body, &b);
    gthread_sleep(120);
    g_order[g_norder] = 0;
    CHECK(!strcmp(g_order, "BA"), "sleepers ran in order '%s', want 'BA'", g_order);

    /* the main thread polls (as through timeGetTime) without blocking */
    g_flag = 0;
    gthread_create_host(flag_body, NULL);
    uint32_t t0 = pal_time_ms();
    while (!g_flag && pal_time_ms() - t0 < 500) gthread_preempt();
    CHECK(g_flag, "a due thread did not run while the main thread polled");
    CHECK(pal_time_ms() - t0 < 100, "polling thread waited %lu ms for a 5 ms sleeper", (unsigned long)(pal_time_ms() - t0));
}

/* ---- mixer ---- */
static void test_stream(void)
{
    int v = mixer_alloc();
    gptr b0 = gheap_alloc(200), b1 = gheap_alloc(200);
    for (uint32_t k = 0; k < 100; k++) { gmem_wr16(b0 + 2 * k, 1000); gmem_wr16(b1 + 2 * k, (uint16_t)-1000); }
    CHECK(mixer_stream_begin(v, 1, 16, MIX_RATE), "stream_begin");
    CHECK(mixer_stream_ready(v) == 0, "empty stream: buffer 0 ready");
    mixer_stream_load(v, 0, b0, 200);
    CHECK(mixer_stream_ready(v) == 1, "buffer 0 loaded: buffer 1 ready (got %d)", mixer_stream_ready(v));
    mixer_stream_load(v, 1, b1, 200);
    CHECK(mixer_stream_ready(v) == -1, "both loaded: none ready");
    int16_t out[2 * 250];
    mixer_render(out, 250);
    CHECK(out[0] > 0 && out[2 * 99] > 0 && out[2 * 100] < 0 && out[2 * 199] < 0, "buffers played in order");
    CHECK(out[2 * 230] == 0, "starved stream is silent (%d)", out[2 * 230]);
    CHECK(mixer_stream_eob(v) == 2, "finished buffers %lu, want 2", (unsigned long)mixer_stream_eob(v));
    CHECK(mixer_status(v) == VS_PLAYING, "starved stream keeps playing until end of stream");
    mixer_release(v);
    gheap_free(b0);
    gheap_free(b1);
}

static void test_loop(void)
{
    int v = mixer_alloc();
    gptr d = gheap_alloc(400);
    for (uint32_t k = 0; k < 100; k++) gmem_wr32(d + 4 * k, 0x01000100u);
    CHECK(mixer_set_data(v, d, 400, 1, 2, 16, MIX_RATE, 4), "set_data");
    mixer_play(v, 1);
    int16_t out[2 * 250];
    mixer_render(out, 250);
    CHECK(mixer_status(v) == VS_PLAYING, "looping voice still playing");
    CHECK(mixer_get_pos(v) == 50, "looping position %lu, want 50", (unsigned long)mixer_get_pos(v));
    mixer_play(v, 0);
    mixer_render(out, 100);
    CHECK(mixer_status(v) == VS_DONE, "one-shot voice done at the end");
    mixer_release(v);
    gheap_free(d);
}

/* ---- rasteriser: table fog ---- */
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void test_fog(void)
{
    dd_surface t = {0};
    t.w = 8; t.h = 8; t.pitch = 16;
    t.pf = (pixfmt){DDPF_RGB, 16, 0xf800, 0x07e0, 0x001f, 0};
    t.pixels = gheap_alloc(16 * 8);
    r3d_reset();
    r3d_set_target(&t, NULL);
    g_r3d.rs[7] = 0;                         /* no z-buffer */
    g_r3d.rs[22] = 1;                        /* no culling */
    g_r3d.rs[28] = 1;                        /* FOGENABLE */
    g_r3d.rs[34] = 0x00ff0000u;              /* fog colour red */
    g_r3d.rs[35] = 3;                        /* LINEAR table fog */
    g_r3d.rs[36] = fbits(16.0f);
    g_r3d.rs[37] = fbits(56.0f);
    /* XYZRHW|DIFFUSE|SPECULAR: two triangles covering the target, white,
     * specular alpha 0 (vertex fog would say "fully fogged"), at depth w */
    gptr vb = gheap_alloc(6 * 24);
    const float xy[6][2] = {{0, 0}, {8, 0}, {0, 8}, {8, 0}, {8, 8}, {0, 8}};
    for (int pass = 0; pass < 2; pass++) {
        float w = pass == 0 ? 8.0f : 100.0f;  /* nearer than fog start / beyond fog end */
        for (int k = 0; k < 6; k++) {
            gptr p = vb + 24u * (uint32_t)k;
            gmem_wr32(p, fbits(xy[k][0])); gmem_wr32(p + 4, fbits(xy[k][1]));
            gmem_wr32(p + 8, fbits(0.5f)); gmem_wr32(p + 12, fbits(1.0f / w));
            gmem_wr32(p + 16, 0xffffffffu); gmem_wr32(p + 20, 0x00000000u);
        }
        r3d_draw(PT_TRIANGLELIST, FVF_XYZRHW | FVF_DIFFUSE | FVF_SPECULAR, vb, 6, 0, 0);
        uint16_t px = gmem_rd16(t.pixels + 16 * 4 + 2 * 4);
        if (pass == 0) CHECK(px == 0xffff, "near pixel %04x, want unfogged white", px);
        else CHECK(px == 0xf800, "far pixel %04x, want fog red", px);
    }
    r3d_set_target(NULL, NULL);
}

static void test_fist(void)
{
    const uint16_t rn = 0x027f, down = 0x067f, up = 0x0a7f, chop = 0x0e7f;   /* default FCW + RC */
    CHECK(cpu_fist32(rn, 2.5) == 2u, "2.5 -> %u, want 2 (ties to even)", cpu_fist32(rn, 2.5));
    CHECK(cpu_fist32(rn, 3.5) == 4u, "3.5 -> %u, want 4", cpu_fist32(rn, 3.5));
    CHECK(cpu_fist32(rn, -2.5) == (uint32_t)-2, "-2.5 -> %d, want -2", (int)cpu_fist32(rn, -2.5));
    CHECK(cpu_fist32(rn, -0.4) == 0u, "-0.4 -> %d", (int)cpu_fist32(rn, -0.4));
    CHECK(cpu_fist32(rn, 255.7) == 256u, "255.7 -> %u", cpu_fist32(rn, 255.7));
    CHECK(cpu_fist32(rn, 2147483647.0) == 0x7fffffffu, "INT_MAX");
    CHECK(cpu_fist32(rn, -2147483648.0) == 0x80000000u, "INT_MIN");
    CHECK(cpu_fist32(rn, 2147483648.0) == 0x80000000u, "overflow -> indefinite");
    CHECK(cpu_fist32(rn, -3e9) == 0x80000000u, "underflow -> indefinite");
    CHECK(cpu_fist32(rn, NAN) == 0x80000000u, "NaN -> indefinite");
    CHECK(cpu_fist32(down, -1.5) == (uint32_t)-2, "floor -1.5 -> %d", (int)cpu_fist32(down, -1.5));
    CHECK(cpu_fist32(up, 1.2) == 2u, "ceil 1.2 -> %u", cpu_fist32(up, 1.2));
    CHECK(cpu_fist32(chop, -1.7) == (uint32_t)-1, "trunc -1.7 -> %d", (int)cpu_fist32(chop, -1.7));
    CHECK(cpu_fist16(rn, 40000.0) == 0x8000u, "fist16 overflow -> indefinite");
    CHECK(cpu_fist16(rn, -7.5) == 0xfff8u, "fist16 -7.5 -> %04x", cpu_fist16(rn, -7.5));
}

int main(int argc, char **argv)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/GAMEDATA", argc > 1 ? argv[1] : "game_files");
    guest_config cfg = {0};
    cfg.data_dir = dir;
    cfg.save_dir = "build-host/save-test";
    cfg.max_ms = 5000;
    if (!guest_init(&cfg)) { printf("SKIP test_runtime: WMAIN.EXE not found under %s\n", dir); return 0; }
    test_fist();
    test_threads();
    gthread_shutdown();
    test_stream();
    test_loop();
    test_fog();
    CHECK(gmem_rd32(0x00882294u) == 0, "60 fps flag set before the option");
    CHECK(game_option_apply("hz60") == 1, "hz60 not applied (exe code differs?)");
    CHECK(gmem_rd32(0x00882294u) == 1, "60 fps flag not set");
    CHECK(game_option_apply("nonsense") == 0, "unknown option accepted");
    guest_fini();
    printf("%s test_runtime: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail != 0;
}
