/* Runtime of the host icount build: one counter per guest byte address in
 * 0x400000..0x9fffff, zeroed when the scenario resets the profiler, dumped
 * at exit to build-icount/icount.txt ("count addr" lines, plus a frames line). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/prof.h"
#include "core/video/video.h"
#define ICNT_N 0x600000u
uint32_t *g_icnt;
static uint32_t g_epoch, g_frame0;

#include <windows.h>
/* A watcher thread zeroes the counters when prof_reset bumps the epoch. */
static DWORD WINAPI watch(LPVOID arg)
{
    (void)arg;
    for (;;) {
        if (g_epoch != g_prof_epoch) {
            g_epoch = g_prof_epoch;
            memset(g_icnt, 0, ICNT_N * sizeof *g_icnt);
            g_frame0 = video_frame_count();
        }
        Sleep(5);
    }
    return 0;
}

static void __attribute__((constructor)) icnt_init(void)
{
    g_icnt = calloc(ICNT_N, sizeof *g_icnt);
    CreateThread(NULL, 0, watch, NULL, 0, NULL);
}

static void __attribute__((destructor)) icnt_dump(void)
{
    FILE *f = fopen("build-icount/icount.txt", "w");
    if (!f) return;
    fprintf(f, "# frames %lu\n", (unsigned long)(video_frame_count() - g_frame0));
    for (uint32_t i = 0; i < ICNT_N; i++)
        if (g_icnt[i]) fprintf(f, "%lu %06lx\n", (unsigned long)g_icnt[i], (unsigned long)(i + 0x400000u));
    fclose(f);
}
