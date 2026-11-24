/* New 3DS entry point (backend glue only): runs the recompiled WMAIN.EXE on the
 * player's own files in sdmc:/3ds/tpm/data (= the CD's GAMEDATA folder).
 *
 * Boot: speedup -> log -> gfx (top RGB565 for game frames, bottom console for
 * status) -> ndsp -> first boot only: unpack GOBS/BIG.Z to install/big.lab on a
 * worker thread with progress -> guest_init -> guest_run. The guest runs on this
 * thread; the pal pumps APT/HID from its input poll, and a HOME/power close stops
 * the guest at its next API call. Exit: guest threads joined in guest_run, no
 * VBlank waits once APT says we are closing. */
#include <3ds.h>
#include <stdio.h>
#include <sys/stat.h>
#include "core/video/video.h"
#include "core/bigz.h"
#include "core/guest.h"
#include "core/prof.h"
#include "core/scenario.h"
#include "pal/pal.h"

/* Memory plan (docs/PLAN.md): linear heap for GPU textures, vertex ring and ndsp;
 * the rest is the newlib heap holding the 64 MB guest arena and host stacks.
 * Lifted code recurses on the host stack, so the main thread gets a big one. */
u32 __ctru_linear_heap_size = 24u << 20;   /* textures, vertex ring, staging */
u32 __stacksize__ = 2u << 20;
extern u32 __ctru_heap_size;

extern int g_r3d_prof;

int  pal3ds_audio_init(void);
void pal3ds_audio_fini(void);
int  pal3ds_closing(void);
int  gpu_selftest(uint32_t w, uint32_t h);

#define TPM_ROOT    "sdmc:/3ds/tpm"
#define TPM_DATA    TPM_ROOT "/data"
#define TPM_INSTALL TPM_ROOT "/install"
#define TPM_SAVE    TPM_ROOT "/save"

static volatile int g_unpack_done;
static int          g_unpack_rc;
static bigz_progress g_unpack;
static char         g_unpack_err[160];

static void unpack_thread(void *arg)
{
    (void)arg;
    g_unpack.should_abort = pal_quit_requested;
    g_unpack_rc = bigz_unpack(TPM_DATA "/GOBS/BIG.Z", TPM_INSTALL "/big.lab", &g_unpack, g_unpack_err, sizeof g_unpack_err);
    __atomic_store_n(&g_unpack_done, 1, __ATOMIC_RELEASE);
}

/* sdmc:/3ds/tpm/prof.txt present: sampling profiler on core 2 (New 3DS), report in log.txt */
static volatile int g_prof_stop;
static void prof_thread(void *arg)
{
    (void)arg;
    while (!g_prof_stop) { svcSleepThread(1000000); prof_sample(); }
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

/* First boot: the CD only has big.lab inside BIG.Z. Returns 0 if the user quit. */
static int ensure_big_lab(void)
{
    if (file_exists(TPM_INSTALL "/big.lab") || file_exists(TPM_DATA "/GOBS/big.lab")) return 1;
    pal_log("unpack: GOBS/BIG.Z -> install/big.lab");
    pal_log_flush();
    uint32_t t = pal_time_ms();
    if (!pal_thread_start("unpack", unpack_thread, NULL, 64 * 1024)) return 0;
    while (!__atomic_load_n(&g_unpack_done, __ATOMIC_ACQUIRE)) {
        if (!aptMainLoop()) { pal_request_quit(); pal_threads_join_all(); return 0; }
        if (g_unpack.total_bytes)
            printf("\x1b[3;1HFirst boot: unpacking big.lab\n%3d%%  %lu / %lu KB   \n",
                   (int)((uint64_t)g_unpack.done_bytes * 100 / g_unpack.total_bytes),
                   (unsigned long)(g_unpack.done_bytes >> 10), (unsigned long)(g_unpack.total_bytes >> 10));
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    pal_threads_join_all();
    pal_log("unpack: rc=%d %lu bytes in %lu ms %s", g_unpack_rc, (unsigned long)g_unpack.done_bytes,
            (unsigned long)(pal_time_ms() - t), g_unpack_err);
    if (g_unpack_rc) printf("\x1b[31munpack failed:\x1b[0m %s\n", g_unpack_err);
    return g_unpack_rc == 0;
}

static void wait_for_exit(const char *msg)
{
    printf("%s\nPress START to quit.\n", msg);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

int main(void)
{
    { extern void pal3ds_mark_game_thread(void); pal3ds_mark_game_thread(); }
    osSetSpeedupEnable(true);
    g_fp_runfast = !file_exists(TPM_ROOT "/norunfast.txt");
    cpu_fp_runfast();
    mkdir("sdmc:/3ds", 0777);
    mkdir(TPM_ROOT, 0777);
    mkdir(TPM_INSTALL, 0777);
    mkdir(TPM_SAVE, 0777);
    int log_ok = pal_log_open(TPM_ROOT "/log.txt", TPM_ROOT "/log_prev.txt");

    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    consoleInit(GFX_BOTTOM, NULL);
    printf("The Phantom Menace 3DS\n");

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    pal_log("boot: tpm3ds, log=%s new3ds=%d app_region=%lu KB heap=%lu KB linear_free=%lu KB", log_ok ? "ok" : "FAILED",
            n3ds, (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10),
            (unsigned long)(__ctru_heap_size >> 10), (unsigned long)(linearSpaceFree() >> 10));
    pal3ds_audio_init();

    if (file_exists(TPM_ROOT "/callbench.txt")) {     /* perf plan A1: call marshalling cost (core/callbench.c) */
        extern void callbench_run(void);
        callbench_run();
    }
    if (file_exists(TPM_ROOT "/gputest.txt")) {       /* GPU backend self-test (see gpu_selftest.c) */
        int pass = gpu_selftest(640, 480);
        printf("GPU self-test: %s (log.txt)\n", pass ? "PASS" : "FAIL");
    }
    if (file_exists(TPM_ROOT "/cpubench.txt")) {
        if (file_exists(TPM_ROOT "/nospeed.txt")) { osSetSpeedupEnable(false); pal_log("cpubench: speedup OFF"); }   /* emulated-CPU throughput microbenchmarks (log.txt) */
        volatile uint32_t vi = 1; volatile float vf = 1.0f; volatile double vd = 1.0; static volatile uint32_t mem[4096];
        uint64_t t0, t1; uint32_t n = 4000000;
        t0 = svcGetSystemTick(); for (uint32_t i = 0; i < n; i++) vi = vi * 3u + i; t1 = svcGetSystemTick();
        pal_log("cpubench: int mul-add %.1f ns/iter", (double)(t1 - t0) * 1e9 / (CPU_TICKS_PER_MSEC * 1000.0) / n);
        t0 = svcGetSystemTick(); for (uint32_t i = 0; i < n; i++) vf = vf * 1.0000001f + 0.5f; t1 = svcGetSystemTick();
        pal_log("cpubench: float mul-add %.1f ns/iter", (double)(t1 - t0) * 1e9 / (CPU_TICKS_PER_MSEC * 1000.0) / n);
        t0 = svcGetSystemTick(); for (uint32_t i = 0; i < n; i++) vd = vd * 1.0000001 + 0.5; t1 = svcGetSystemTick();
        pal_log("cpubench: double mul-add %.1f ns/iter", (double)(t1 - t0) * 1e9 / (CPU_TICKS_PER_MSEC * 1000.0) / n);
        t0 = svcGetSystemTick(); for (uint32_t i = 0; i < n; i++) mem[(i * 7u) & 4095u] += i; t1 = svcGetSystemTick();
        pal_log("cpubench: memory rmw %.1f ns/iter", (double)(t1 - t0) * 1e9 / (CPU_TICKS_PER_MSEC * 1000.0) / n);
        t0 = svcGetSystemTick(); for (uint32_t i = 0; i < n; i++) vd = (double)(int32_t)(i & 255u) / 3.0; t1 = svcGetSystemTick();
        pal_log("cpubench: int->double + div %.1f ns/iter", (double)(t1 - t0) * 1e9 / (CPU_TICKS_PER_MSEC * 1000.0) / n);
        pal_log_flush();
    }
    int ok = file_exists(TPM_DATA "/BIN/WMAIN.EXE");
    if (!ok) {
        pal_log("boot: %s/BIN/WMAIN.EXE missing", TPM_DATA);
        wait_for_exit("Missing " TPM_DATA "/BIN/WMAIN.EXE.\nCopy your CD's GAMEDATA folder\ncontents to " TPM_DATA ".");
    } else if (ensure_big_lab()) {
        guest_config cfg = {0};
        cfg.data_dir = TPM_DATA;
        cfg.install_dir = TPM_INSTALL;
        cfg.save_dir = TPM_SAVE;
        cfg.trace = 0;
        { extern int g_clock_clamp; g_clock_clamp = !file_exists(TPM_ROOT "/noclamp.txt"); }
        { extern int g_xf_off; g_xf_off = file_exists(TPM_ROOT "/noasync.txt"); pal_log("gpu: async present %s", g_xf_off ? "OFF (noasync.txt)" : "enabled (needs core 2)"); }
        { extern int r3d_q_start(void); if (n3ds && file_exists(TPM_ROOT "/drawthread.txt"))   { extern int g_r3d_q_stress_ms; FILE *sf = fopen(TPM_ROOT "/drawthread.txt", "r"); if (sf) { if (fscanf(sf, "%d", &g_r3d_q_stress_ms) != 1) g_r3d_q_stress_ms = 0; fclose(sf); } }
        if (n3ds && file_exists(TPM_ROOT "/drawthread.txt"))   /* opt-in: shows graphics errors on hardware, see docs/PLAN.md 46l */ pal_log("gpu: draw thread %s", r3d_q_start() ? "on (core 2)" : "FAILED, drawing on the game thread"); else pal_log("gpu: draw thread off"); }
        { extern int g_wc_lod_enable, g_wc_lod_fix; FILE *lf = fopen(TPM_ROOT "/lodfix.txt", "r"); int fx = 0;
          if (lf) { if (fscanf(lf, "%d", &fx) != 1) fx = 0; fclose(lf); }
          g_wc_lod_fix = fx; g_wc_lod_enable = file_exists(TPM_ROOT "/lod.txt") && !file_exists(TPM_ROOT "/60fps.txt");   /* opt-in: drops big floor/wall polygons, see docs/PLAN.md 46l */
          pal_log("lod: adaptive draw distance %s%s", g_wc_lod_enable ? "on" : "off", fx ? " (fixed, lodfix.txt)" : ""); }
        { extern int g_fbdump_start; FILE *ff = fopen(TPM_ROOT "/fbdump.txt", "r"); if (ff) { if (fscanf(ff, "%d", &g_fbdump_start) != 1) g_fbdump_start = 0; fclose(ff); } }
        { extern void pal_gpu_set_split(uint32_t v); if (file_exists(TPM_ROOT "/split.txt")) { pal_gpu_set_split(1100); pal_log("gpu: early submit ON (split.txt)"); } }
        { extern char g_popt_mask[16]; FILE *pf = fopen(TPM_ROOT "/popt.txt", "r"); if (pf) { if (fgets(g_popt_mask, 13, pf)) pal_log("popt.txt: forcing POPT mask %s", g_popt_mask); fclose(pf); } }
        { extern int g_perf_default_low; g_perf_default_low = !file_exists(TPM_ROOT "/perfhigh.txt"); }   /* game option "Computer Rating": Low unless perfhigh.txt */
        cfg.nolimit = file_exists(TPM_ROOT "/nolimit.txt");   /* benchmarking only */
        cfg.hz60 = file_exists(TPM_ROOT "/60fps.txt");   /* original 30 fps logic unless 60fps.txt exists (the 60 fps mode doubles logic steps per frame) */
        if (!file_exists(TPM_ROOT "/4to3.txt")) g_screen_w = 800;   /* widescreen 800x480 unless this file exists */
        pal_log("boot: screen %lux%lu", (unsigned long)g_screen_w, (unsigned long)g_screen_h);
        if (!guest_init(&cfg)) {
            pal_log("boot: guest_init failed");
            wait_for_exit("Could not load the game (see log.txt).");
        } else {
            scenario_load(TPM_ROOT "/scenario.txt", TPM_ROOT);
            g_r3d_prof = file_exists(TPM_ROOT "/r3dprof.txt");
            { extern int g_native_off; g_native_off = file_exists(TPM_ROOT "/nonative.txt"); if (g_native_off) pal_log("natives: OFF"); }
            { extern int g_ai_native_off; g_ai_native_off = file_exists(TPM_ROOT "/noai.txt"); if (g_ai_native_off) pal_log("ai native: OFF (noai.txt)"); }
            { extern int pal_3ds_fake_clock; pal_3ds_fake_clock = file_exists(TPM_ROOT "/fakeclock.txt"); if (pal_3ds_fake_clock) pal_log("clock: FAKE (fakeclock.txt): 16 ms per presented frame, use with nolimit.txt"); }
            { extern int g_skip_level; g_skip_level = file_exists(TPM_ROOT "/skip.txt"); if (g_skip_level) pal_log("skip: straight to level 1 (movies and menu skipped)");
              else if (file_exists(TPM_ROOT "/menu.txt")) { g_skip_level = 2; pal_log("skip: movies skipped, stop at the menu (menu.txt)"); }
              if (file_exists(TPM_ROOT "/battle_start.txt")) { extern int g_battle_mode; g_battle_mode = 2; g_skip_level = 1; pal_log("battle: normal level start, movies skipped (battle_start.txt)"); }
              else if (file_exists(TPM_ROOT "/battle.txt")) { extern int g_battle_mode, g_battle_slot; g_battle_mode = 1; g_skip_level = 1; { FILE *bf = fopen(TPM_ROOT "/battle.txt", "r"); int bs = 0; if (bf) { if (fscanf(bf, "%d", &bs) == 1 && bs > 0) g_battle_slot = bs; fclose(bf); } } pal_log("battle: level 1, then load slot %d at once (battle.txt)", g_battle_slot); } }
            pal_log("r3d: raster timing %s", g_r3d_prof ? "on (r3dprof.txt)" : "off");
            { extern void ui_init(void); ui_init(); }
            pal_log_flush();
            Thread prof = NULL;
            if (file_exists(TPM_ROOT "/prof.txt")) {
                prof = threadCreate(prof_thread, NULL, 16 * 1024, 0x18, 2, false);
                pal_log("prof: sampler %s", prof ? "on core 2" : "FAILED");
            }
            guest_stall_monitor_start();
            int r = guest_run();
            if (prof) { g_prof_stop = 1; threadJoin(prof, U64_MAX); threadFree(prof); prof_report(64); }
            pal_log("exit: guest stopped, reason %d (%s), scenario %s", r, guest_stop_text(),
                    scenario_check() ? "ok" : "FAILED");
            { extern void r3d_q_shutdown(void); r3d_q_shutdown(); }
            guest_fini();
            if (!pal3ds_closing() && r != GUEST_STOP_EXIT && r != GUEST_STOP_REQUESTED) {
                printf("\nStopped: %s\n", guest_stop_text());
                wait_for_exit("");
            }
        }
    }

    uint32_t t_exit = pal_time_ms();
    pal_request_quit();
    int stuck = pal_threads_join_all();
    pal3ds_audio_fini();
    gfxExit();
    pal_log("exit: closing=%d stuck threads=%d, shutdown %lu ms", pal3ds_closing(), stuck,
            (unsigned long)(pal_time_ms() - t_exit));
    pal_log_close();
    return 0;
}
