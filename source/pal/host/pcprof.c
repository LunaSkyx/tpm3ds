/* Host profiler with exact attribution (TPM_PROF=2, Windows hosts): every
 * millisecond all other threads of the process are suspended briefly and their
 * instruction pointers recorded. Lifted guest functions are C functions, so
 * the histogram says which lifted function, HLE call, runtime helper or
 * rasteriser code really runs - unlike core/prof.c, which can only charge the
 * last guest function entered. Addresses outside this exe (Windows waiting in
 * Sleep etc.) are counted as one bucket. Written as "count offset" lines to
 * build-host/pcprof.txt; tools/pcprof.py symbolises them.
 * Reset by the scenario command "at N prof_reset" (g_prof_epoch). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "core/prof.h"
#include "pal/pal.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

#define PC_N 65536
static uint64_t g_addr[PC_N];
static uint32_t g_cnt[PC_N];
static uint32_t g_outside, g_total;
static volatile int g_stop;
static uintptr_t g_lo, g_hi;

static void add(uint64_t pc)
{
    g_total++;
    if (pc < g_lo || pc >= g_hi) { g_outside++; return; }
    uint32_t h = (uint32_t)((pc * 0x9E3779B97F4A7C15ull) >> 48) & (PC_N - 1);
    for (int n = 0; n < PC_N; n++, h = (h + 1) & (PC_N - 1)) {
        if (g_addr[h] == pc) { g_cnt[h]++; return; }
        if (!g_addr[h]) { g_addr[h] = pc; g_cnt[h] = 1; return; }
    }
}

/* Thread handles are re-listed every 100 ms (a Toolhelp snapshot costs ~1 ms);
 * between refreshes each round only suspends, reads RIP and resumes. */
#define PC_THREADS 64
static void sampler(void *arg)
{
    (void)arg;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    uint32_t epoch = g_prof_epoch;
    HANDLE th[PC_THREADS];
    int nth = 0;
    LARGE_INTEGER fq, t0, t, last = {0};
    QueryPerformanceFrequency(&fq);
    while (!g_stop) {
        QueryPerformanceCounter(&t0);              /* ~0.25 ms; Sleep(1) waits a whole timer tick */
        do { SwitchToThread(); QueryPerformanceCounter(&t); } while ((t.QuadPart - t0.QuadPart) * 4000 < fq.QuadPart);
        if (epoch != g_prof_epoch) {
            epoch = g_prof_epoch;
            memset(g_addr, 0, sizeof g_addr);
            memset(g_cnt, 0, sizeof g_cnt);
            g_outside = g_total = 0;
        }
        if ((t.QuadPart - last.QuadPart) * 10 > fq.QuadPart) {
            last = t;
            for (int i = 0; i < nth; i++) CloseHandle(th[i]);
            nth = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) continue;
            THREADENTRY32 te;
            te.dwSize = sizeof te;
            for (BOOL ok = Thread32First(snap, &te); ok && nth < PC_THREADS; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (h) th[nth++] = h;
            }
            CloseHandle(snap);
        }
        for (int i = 0; i < nth; i++) {
            if (SuspendThread(th[i]) == (DWORD)-1) continue;
            CONTEXT cx;
            cx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th[i], &cx)) add(cx.Rip);
            ResumeThread(th[i]);
        }
    }
    for (int i = 0; i < nth; i++) CloseHandle(th[i]);
}

static pal_thread *g_thread;

void pcprof_start(void)
{
    HMODULE m = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)m + dos->e_lfanew);
    g_lo = (uintptr_t)m;
    g_hi = g_lo + nt->OptionalHeader.SizeOfImage;
    g_thread = pal_thread_spawn(sampler, NULL, 65536);
}

void pcprof_stop(const char *path)
{
    if (!g_thread) return;
    g_stop = 1;
    pal_thread_wait(g_thread);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# total %lu outside %lu base %llx\n", (unsigned long)g_total, (unsigned long)g_outside,
            (unsigned long long)g_lo);
    for (int i = 0; i < PC_N; i++)
        if (g_cnt[i]) fprintf(f, "%lu %llx\n", (unsigned long)g_cnt[i], (unsigned long long)(g_addr[i] - g_lo));
    fclose(f);
    pal_log("pcprof: %lu samples (%lu outside the exe) -> %s", (unsigned long)g_total, (unsigned long)g_outside, path);
}
#else
void pcprof_start(void) { pal_log("pcprof: Windows hosts only"); }
void pcprof_stop(const char *path) { (void)path; }
#endif
