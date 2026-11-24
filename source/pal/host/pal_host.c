/* Host (PC / headless) backend: time + log. Threads come with the first
 * module that needs them on the host. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "pal/pal.h"
#include "core/gmem.h"

static FILE   *g_log;
static clock_t g_t0;

/* Golden-run verifier (docs/PERF_PLAN.md section 9). Host only.
 * TPM_FAKE_CLOCK=1: time is a function of the presented frame count only (16 ms per frame),
 *   so guest code that reads the clock behaves the same on every run.
 * TPM_GOLDEN=<file>: one line per presented frame with a hash of the screen and of the whole
 *   guest arena (g_gmem). Two runs must produce identical files. */
int             pal_3ds_fake_clock;   /* only the 3DS build uses it (r3d_gpu.c asks); the host has TPM_FAKE_CLOCK */
void pal_gpu_where(char *buf, int n) { if (n > 0) buf[0] = 0; }
void pal_where(const char *tag) { (void)tag; }
void pal_where_dump(void) {}
static int      g_fake_clock = -1;
static uint32_t g_frames_shown;
static uint64_t g_fake_reads;     /* clock reads: the clock must move even before the first present */
static FILE    *g_golden;
static int      g_golden_failed;

static int fake_clock(void)
{
    if (g_fake_clock < 0) g_fake_clock = getenv("TPM_FAKE_CLOCK") != NULL;
    return g_fake_clock;
}

uint32_t pal_time_ms(void)
{
    if (fake_clock()) return (uint32_t)(g_frames_shown * 16u + g_fake_reads++ / 8u);
    return (uint32_t)((clock() - g_t0) * 1000 / CLOCKS_PER_SEC);
}

static uint64_t fnv64(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static void golden_frame(const uint16_t *rgb565, uint32_t w, uint32_t h)
{
    if (!g_golden) {
        const char *path = getenv("TPM_GOLDEN");
        if (!path || g_golden_failed) return;
        g_golden = fopen(path, "w");
        if (!g_golden) { g_golden_failed = 1; return; }
    }
    uint64_t fb = fnv64(rgb565, (size_t)w * h * 2u, 0xcbf29ce484222325ull);
    uint64_t gm = g_gmem ? fnv64(g_gmem, g_gmem_size, 0xcbf29ce484222325ull) : 0;
    fprintf(g_golden, "frame %u fb=%016llx gm=%016llx\n", (unsigned)g_frames_shown,
            (unsigned long long)fb, (unsigned long long)gm);
    fflush(g_golden);
}

int pal_log_open(const char *path, const char *prev_path)
{
    g_t0 = clock();
    remove(prev_path);
    rename(path, prev_path);
    g_log = fopen(path, "w");
    return g_log != NULL;
}

void pal_log(const char *fmt, ...)
{
    FILE *out = g_log ? g_log : stderr;
    fprintf(out, "[%7lu ms] ", (unsigned long)pal_time_ms());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
}

void pal_log_flush(void) { fflush(g_log ? g_log : stderr); }
void pal_log_close(void) { if (g_log) fclose(g_log); g_log = NULL; }

#ifdef _WIN32
#include <windows.h>
static LARGE_INTEGER g_qpf, g_qp0;
uint64_t pal_time_us(void)
{
    LARGE_INTEGER t;
    if (fake_clock()) return (uint64_t)g_frames_shown * 16000u + g_fake_reads++ * 125u;
    if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_qp0); }
    QueryPerformanceCounter(&t);
    return (uint64_t)((t.QuadPart - g_qp0.QuadPart) * 1000000ull / (uint64_t)g_qpf.QuadPart);
}
void pal_sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#include <unistd.h>
uint64_t pal_time_us(void) { return (uint64_t)pal_time_ms() * 1000ull; }
void pal_sleep_ms(uint32_t ms) { usleep(ms * 1000u); }
#endif

/* Headless: audio is mixed (so timing and sample status are real); with TPM_WAV=<file>
 * it is also written as a 22050 Hz stereo WAV for listening/analysis. */
static FILE    *g_wav;
static uint32_t g_wav_frames;
static int      g_wav_checked;
static void wav_header(void)
{
    uint32_t data = g_wav_frames * 4u, hdr[11] = {0x46464952u, 36u + data, 0x45564157u, 0x20746d66u, 16u,
                                                  0x00020001u, 22050u, 22050u * 4u, 0x00100004u, 0x61746164u, data};
    fseek(g_wav, 0, SEEK_SET);
    fwrite(hdr, 4, 11, g_wav);
    fseek(g_wav, 0, SEEK_END);
}
void pal_audio_submit(const int16_t *frames, uint32_t n)
{
    if (!g_wav_checked) {
        g_wav_checked = 1;
        const char *p = getenv("TPM_WAV");
        if (p && (g_wav = fopen(p, "wb")) != NULL) wav_header();
    }
    if (!g_wav) return;
    fwrite(frames, 4, n, g_wav);
    g_wav_frames += n;
    if ((g_wav_frames & 0xffff) < n) { wav_header(); fflush(g_wav); }
}

/* Headless: frames are kept by core/video for screenshots; nothing to show. */
/* TPM_FRAME_MS=n: every presented frame takes at least n ms of wall time, to
 * reproduce a slow device's frame rate (and the game's catch-up steps). */
static void frame_pace(void)
{
    static int ms = -1;
    static uint32_t last;
    if (ms < 0) { const char *e = getenv("TPM_FRAME_MS"); ms = e ? atoi(e) : 0; }
    if (!ms) return;
    uint32_t now = pal_time_ms();
    if (last && now - last < (uint32_t)ms) pal_sleep_ms(ms - (now - last));
    last = pal_time_ms();
}
void pal_video_present(const uint16_t *rgb565, uint32_t w, uint32_t h)
{
    frame_pace();
    golden_frame(rgb565, w, h);
    g_frames_shown++;
}
void pal_video_present_flip(const uint16_t *rgb565, uint32_t w, uint32_t h)
{
    frame_pace();
    golden_frame(rgb565, w, h);
    g_frames_shown++;
}
void pal_video_present_movie(const uint16_t *rgb565, uint32_t w, uint32_t h)
{
    /* headless: the guest frame is what the golden run hashes, the presentation is not modelled */
    frame_pace();
    golden_frame(rgb565, w, h);
    g_frames_shown++;
}

/* Headless: input comes from the scenario runner only. */
int pal_input_next(pal_input_event *e) { (void)e; return 0; }

/* ---- threads ---- */
#include <stdlib.h>
#ifdef _WIN32
struct pal_thread { HANDLE h; void (*fn)(void *); void *arg; };
struct pal_sem { HANDLE h; };
static DWORD WINAPI thread_tramp(LPVOID p)
{
    struct pal_thread *t = p;
    t->fn(t->arg);
    return 0;
}
pal_thread *pal_thread_spawn(void (*fn)(void *), void *arg, uint32_t stack_bytes)
{
    struct pal_thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, stack_bytes, thread_tramp, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->h) { free(t); return NULL; }
    return t;
}
void pal_thread_wait(pal_thread *t)
{
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}
pal_sem *pal_sem_new(int initial)
{
    struct pal_sem *s = calloc(1, sizeof *s);
    if (s) s->h = CreateSemaphoreA(NULL, initial, 0x7fffffff, NULL);
    return s;
}
void pal_sem_wait(pal_sem *s) { WaitForSingleObject(s->h, INFINITE); }
void pal_sem_post(pal_sem *s) { ReleaseSemaphore(s->h, 1, NULL); }
void pal_sem_free(pal_sem *s) { if (s) { CloseHandle(s->h); free(s); } }
#else
#include <pthread.h>
#include <semaphore.h>
struct pal_thread { pthread_t t; void (*fn)(void *); void *arg; };
struct pal_sem { sem_t s; };
static void *thread_tramp(void *p) { struct pal_thread *t = p; t->fn(t->arg); return NULL; }
pal_thread *pal_thread_spawn(void (*fn)(void *), void *arg, uint32_t stack_bytes)
{
    struct pal_thread *t = calloc(1, sizeof *t);
    pthread_attr_t a;
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_bytes);
    int r = pthread_create(&t->t, &a, thread_tramp, t);
    pthread_attr_destroy(&a);
    if (r) { free(t); return NULL; }
    return t;
}
void pal_thread_wait(pal_thread *t) { if (t) { pthread_join(t->t, NULL); free(t); } }
pal_sem *pal_sem_new(int initial) { struct pal_sem *s = calloc(1, sizeof *s); if (s) sem_init(&s->s, 0, (unsigned)initial); return s; }
void pal_sem_wait(pal_sem *s) { while (sem_wait(&s->s) != 0) {} }
void pal_sem_post(pal_sem *s) { sem_post(&s->s); }
void pal_sem_free(pal_sem *s) { if (s) { sem_destroy(&s->s); free(s); } }
#endif

/* No GPU on the host: the core's software rasteriser renders (reference renderer). */
int      pal_gpu_init(uint32_t w, uint32_t h) { (void)w; (void)h; return 0; }
int      pal_gpu_target_count(void) { return 0; }
int      pal_gpu_direct_ok(void) { return 0; }
void     pal_gpu_target_select(int slot) { (void)slot; }
int      pal_gpu_target_stage(int slot) { (void)slot; return 0; }
const uint16_t *pal_gpu_staging(void) { return NULL; }
int      pal_gpu_present_direct(int slot, uint16_t *copy) { (void)slot; (void)copy; return 0; }
uint32_t pal_gpu_tex_create(uint32_t w, uint32_t h, int fmt) { (void)w; (void)h; (void)fmt; return 0; }
void     pal_gpu_tex_upload(uint32_t id, const uint16_t *t) { (void)id; (void)t; }
void     pal_gpu_tex_free(uint32_t id) { (void)id; }
void     pal_gpu_target_load(int slot, const uint8_t *p, uint32_t pitch) { (void)slot; (void)p; (void)pitch; }
void     pal_gpu_target_store(int slot, uint8_t *p, uint32_t pitch) { (void)slot; (void)p; (void)pitch; }
void     pal_gpu_draw(const pal_gpu_state *st, const pal_gpu_vertex *v, uint32_t n) { (void)st; (void)v; (void)n; }

pal_thread *pal_thread_spawn_bg(void (*fn)(void *), void *arg, uint32_t stack_bytes) { return pal_thread_spawn(fn, arg, stack_bytes); }

/* The host backend has no linear heap (no GPU targets or textures). */
uint32_t pal_linear_free(void) { return 0; }
uint32_t pal_host_free(void) { return 0; }
int g_aud_dbg_on;
