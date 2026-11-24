/* Headless host runner: loads WMAIN.EXE from the player's GAMEDATA dir and runs
 * it with a wall-clock limit. Usage: tpm_host <GAMEDATA dir> [max_ms] [trace] [scenario file] */
#include <stdio.h>
#include <stdlib.h>
#include "core/video/video.h"
#include "core/bigz.h"
#include "core/guest.h"
#include "core/prof.h"
#include "core/render/r3d.h"
#include "core/scenario.h"
#include "pal/pal.h"

/* TPM_PROF=1: sampling profiler (core/prof.c), report in the log.
 * TPM_PROF=2: exact PC sampling of all threads (pcprof.c) -> build-host/pcprof.txt, see tools/pcprof.py.
 * TPM_NORASTER=1: 3D draws are not rendered, so the profile shows the guest code.
 * TPM_HZ60=0: the original's 30 fps cap instead of its 60 fps mode (default).
 * TPM_NOLIMIT=1: the game's frame limiter off (frames back to back; benchmarks). */
void pcprof_start(void);
void pcprof_stop(const char *path);
static volatile int g_prof_stop;
static void prof_thread(void *arg)
{
    (void)arg;
    while (!g_prof_stop) { pal_sleep_ms(1); prof_sample(); }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <GAMEDATA dir> [max_ms] [trace]\n", argv[0]); return 2; }
    pal_log_open("build-host/run_log.txt", "build-host/run_log_prev.txt");
    pal_mkdir("build-host/save");
    pal_mkdir("build-host/install");
    pal_file *have = pal_file_open("build-host/install/big.lab");
    if (have) pal_file_close(have);
    else {
        char src[512], err[128];
        snprintf(src, sizeof src, "%s/GOBS/BIG.Z", argv[1]);
        int rc = bigz_unpack(src, "build-host/install/big.lab", NULL, err, sizeof err);
        pal_log("host: unpack %s -> build-host/install/big.lab: rc %d %s", src, rc, err);
        if (rc) { fprintf(stderr, "cannot unpack %s: %s\n", src, err); return 1; }
    }
    guest_config cfg = {0};
    cfg.install_dir = "build-host/install";
    cfg.data_dir = argv[1];
    cfg.save_dir = "build-host/save";
    cfg.max_ms = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 20000;
    cfg.trace = argc > 3 ? atoi(argv[3]) : 1;
    cfg.hz60 = !(getenv("TPM_HZ60") && atoi(getenv("TPM_HZ60")) == 0);
    if (getenv("TPM_WIDE")) g_screen_w = 800;   /* widescreen test (3DS default) */
    /* Same testing mode as the 3DS (`sdmc:/3ds/tpm/skip.txt`): spam RETURN so the boot
     * goes menu -> level 1 in a couple of seconds. Lets a device scenario be reproduced
     * on the host, where the guest frame can be captured and inspected. */
    {
        extern int g_skip_level;
        pal_file *skip = pal_file_open("build-host/skip.txt");
        g_skip_level = skip != NULL;
        if (skip) { pal_file_close(skip); pal_log("host: skip mode (build-host/skip.txt)"); }
    }
    /* TPM_BATTLE=<slot>: like battle.txt on the 3DS (level 1, then load that save at once) */
    if (getenv("TPM_BATTLE")) {
        extern int g_battle_mode, g_battle_slot, g_skip_level;
        g_battle_mode = 1; g_skip_level = 1; g_battle_slot = atoi(getenv("TPM_BATTLE")) > 0 ? atoi(getenv("TPM_BATTLE")) : g_battle_slot;
        pal_log("host: battle mode, slot %d", g_battle_slot);
    }
    cfg.nolimit = getenv("TPM_NOLIMIT") != NULL;
    if (getenv("TPM_ARENA_MB")) cfg.arena_size = (uint32_t)strtoul(getenv("TPM_ARENA_MB"), NULL, 0) << 20;
    if (!guest_init(&cfg)) { pal_log("host: guest_init failed"); pal_log_close(); return 1; }
    if (argc > 4) scenario_load(argv[4], "build-host");
    g_r3d_null = getenv("TPM_NORASTER") != NULL;
    { extern int g_r3d_diag; g_r3d_diag = getenv("TPM_R3D_DIAG") != NULL; }
    g_r3d_prof = getenv("TPM_R3DPROF") != NULL || getenv("TPM_PROF") != NULL;
    int pc = getenv("TPM_PROF") && atoi(getenv("TPM_PROF")) == 2;
    if (pc) pcprof_start();
    pal_thread *prof = getenv("TPM_PROF") && !pc ? pal_thread_spawn(prof_thread, NULL, 65536) : NULL;
    int r = guest_run();
    if (prof) { g_prof_stop = 1; pal_thread_wait(prof); prof_report(30); }
    if (pc) pcprof_stop("build-host/pcprof.txt");
    int scen_ok = scenario_check();
    printf("guest stopped: reason %d (%s), exit code %lu, see build-host/run_log.txt\n", r, guest_stop_text(),
           (unsigned long)guest_exit_code());
    guest_fini();
    pal_log_close();
    return (r == GUEST_STOP_EXIT || (argc > 4 && r == GUEST_STOP_REQUESTED)) && scen_ok ? 0 : 1;
}
