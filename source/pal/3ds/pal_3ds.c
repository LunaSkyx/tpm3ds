/* 3DS backend: time, log, threads. */
#include <3ds.h>
#include <stdarg.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "pal/pal.h"
#include "core/audio/mixer.h"

static u64       g_t0;
static FILE     *g_log;
static LightLock g_log_lock;
static char      g_log_buf[16 * 1024];

/* Deterministic clock for debugging (main.c sets this from sdmc:/3ds/tpm/fakeclock.txt).
 * Same rule as pal_host.c's TPM_FAKE_CLOCK: time is a function of the presented frame count
 * only (16 ms per frame), so a scenario's frame N is the same game state on every run and can
 * be compared with a host run that uses TPM_FAKE_CLOCK=1. Only useful with nolimit.txt.
 * Audio and the frame limiter then see fake time; it is a diagnostics switch, not a mode. */
int pal_3ds_fake_clock;
static uint32_t g_frames_shown;
static uint64_t g_fake_reads;      /* the clock must move even before the first present */
void pal3ds_count_frame(void) { g_frames_shown++; if (pal_3ds_fake_clock && g_frames_shown % 50u == 0) pal_log("fake clock: %lu frames shown, %llu game-thread reads", (unsigned long)g_frames_shown, (unsigned long long)g_fake_reads); }
/* Fake clock: only the game (main) thread's reads advance it (audio, I/O and draw threads read it at timing-dependent
 * moments, which made two runs of the same scenario differ). */
static __thread int t_game;
void pal3ds_mark_game_thread(void) { t_game = 1; }
#define FAKE_READ() (t_game ? g_fake_reads++ : g_fake_reads)

/* Ask the guest scheduler to run another guest thread. The present path is host code running on a
 * guest thread, so a plain gthread_yield() is safe; this wrapper keeps pal_3ds.c free of guest
 * internals. */
void pal_yield_to_guest(void);



uint32_t pal_time_ms(void)
{
    if (pal_3ds_fake_clock) return (uint32_t)(g_frames_shown * 16u + FAKE_READ() / 8u);
    return (uint32_t)((svcGetSystemTick() - g_t0) / CPU_TICKS_PER_MSEC);
}

/* Log: lines are formatted into a RAM ring under a short lock; a writer thread on core 2 (New 3DS) copies the ring out and
 * does the SD card write + fflush OUTSIDE the lock. Before, pal_log() itself called fflush every 2 s on the game thread, and a
 * FAT flush on the 3DS SD card takes 30-140 ms: a hitch (100+ ms frame) every ~2 seconds during play (hardware log 2026-11-24).
 * Without core 2 (Old 3DS) or when the thread cannot start, the old synchronous flush every 2 s stays. */
#define LOG_RING (64 * 1024)
static char       g_ring[LOG_RING];
static size_t     g_ring_len;
static char       g_drain[LOG_RING];
static LightLock  g_drain_lock;
static Thread     g_log_thread;
static volatile int g_log_quit, g_log_async;
static uint32_t   g_log_dropped;

static void log_drain(void)
{
    LightLock_Lock(&g_drain_lock);
    LightLock_Lock(&g_log_lock);
    size_t n = g_ring_len;
    memcpy(g_drain, g_ring, n);
    g_ring_len = 0;
    LightLock_Unlock(&g_log_lock);
    if (g_log) {
        if (n) fwrite(g_drain, 1, n, g_log);
        fflush(g_log);
    }
    LightLock_Unlock(&g_drain_lock);
}

static void log_writer(void *arg)
{
    (void)arg;
    while (!g_log_quit) {
        svcSleepThread(500ll * 1000000ll);
        log_drain();
    }
}

int pal_log_open(const char *path, const char *prev_path)
{
    g_t0 = svcGetSystemTick();
    LightLock_Init(&g_log_lock);
    LightLock_Init(&g_drain_lock);
    remove(prev_path);
    rename(path, prev_path);
    g_log = fopen(path, "w");
    if (!g_log) return 0;
    setvbuf(g_log, g_log_buf, _IOFBF, sizeof g_log_buf);
    bool is_new = false;
    APT_CheckNew3DS(&is_new);
    if (is_new) {
        g_log_thread = threadCreate(log_writer, NULL, 16 * 1024, 0x3f, 2, false);
        g_log_async = g_log_thread != NULL;
    }
    return 1;
}

static const char *volatile g_where_ring[12];
static volatile uint32_t g_where_n;
void pal_where(const char *tag) { g_where_ring[g_where_n++ % 12u] = tag; }
void pal_where_dump(void)
{
    char line[300]; int w = snprintf(line, sizeof line, "where (oldest first):");
    uint32_t n = g_where_n;
    for (uint32_t k = 0; k < 12u && k < n; k++) {
        const char *t = g_where_ring[(n - (n < 12u ? n : 12u) + k) % 12u];
        w += snprintf(line + w, sizeof line - (size_t)w, " | %s", t ? t : "-");
        if (w > (int)sizeof line - 40) break;
    }
    pal_log("stall: %s (%lu markers)", line, (unsigned long)n);
}

void pal_log(const char *fmt, ...)
{
    if (!g_log) return;
    LightLock_Lock(&g_log_lock);
    if (g_log_async) {
        size_t room = LOG_RING - g_ring_len;
        int n = snprintf(g_ring + g_ring_len, room, "[%7lu ms] ", (unsigned long)pal_time_ms());
        if (n > 0 && (size_t)n < room) {
            va_list ap;
            va_start(ap, fmt);
            int m = vsnprintf(g_ring + g_ring_len + n, room - (size_t)n, fmt, ap);
            va_end(ap);
            if (m < 0) m = 0;
            size_t used = (size_t)n + (size_t)m;
            if (used + 1 >= room) used = room - 2;        /* truncated: keep what fits, end the line */
            g_ring[g_ring_len + used] = '\n';
            g_ring_len += used + 1;
        } else g_log_dropped++;
        LightLock_Unlock(&g_log_lock);
        return;
    }
    fprintf(g_log, "[%7lu ms] ", (unsigned long)pal_time_ms());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    /* a hang must not hide the last lines: flush at least every 2 s */
    static uint32_t last_flush;
    uint32_t now = pal_time_ms();
    if (now - last_flush >= 2000) { last_flush = now; fflush(g_log); }
    LightLock_Unlock(&g_log_lock);
}

void pal_log_flush(void)
{
    if (!g_log) return;
    if (g_log_async) { log_drain(); return; }
    LightLock_Lock(&g_log_lock);
    fflush(g_log);
    LightLock_Unlock(&g_log_lock);
}

void pal_log_close(void)
{
    if (!g_log) return;
    if (g_log_thread) {
        g_log_quit = 1;
        threadJoin(g_log_thread, 2000000000ull);
        threadFree(g_log_thread);
        g_log_thread = NULL;
    }
    if (g_log_async) {
        log_drain();
        g_log_async = 0;
    }
    fclose(g_log);
    g_log = NULL;
}

/* ---- threads ---- */
#define MAX_THREADS 16
typedef struct { Thread t; const char *name; pal_thread_fn fn; void *arg; } thread_slot;
static thread_slot   g_threads[MAX_THREADS];
static int           g_nthreads;
static volatile int  g_quit;

static void thread_entry(void *p)
{
    thread_slot *s = p;
    pal_log("thread '%s' start", s->name);
    s->fn(s->arg);
    pal_log("thread '%s' end", s->name);
}

int pal_thread_start(const char *name, pal_thread_fn fn, void *arg, unsigned stack_bytes)
{
    if (g_nthreads >= MAX_THREADS) { pal_log("thread '%s': registry full", name); return 0; }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    thread_slot *s = &g_threads[g_nthreads];
    s->name = name; s->fn = fn; s->arg = arg;
    /* core -2 = the app's default core (0). Core 1/2 placement is a later decision. */
    s->t = threadCreate(thread_entry, s, stack_bytes, prio + 1, -2, false);
    if (!s->t) { pal_log("thread '%s': threadCreate failed", name); return 0; }
    g_nthreads++;
    return 1;
}

void pal_request_quit(void) { g_quit = 1; }
int  pal_quit_requested(void) { return g_quit; }

int pal_threads_join_all(void)
{
    int stuck = 0;
    for (int i = g_nthreads - 1; i >= 0; i--) {
        thread_slot *s = &g_threads[i];
        uint32_t t = pal_time_ms();
        /* Bounded wait: a thread that ignores the quit flag is a bug; report it
         * loudly instead of hanging the console on "closing software". */
        Result r = threadJoin(s->t, 2000000000ull);
        if (R_FAILED(r)) { pal_log("JOIN FAILED '%s' (r=%08lx) after %lu ms", s->name,
                                   (unsigned long)r, (unsigned long)(pal_time_ms() - t)); stuck++; continue; }
        threadFree(s->t);
        pal_log("joined '%s' in %lu ms", s->name, (unsigned long)(pal_time_ms() - t));
    }
    g_nthreads = 0;
    return stuck;
}

uint64_t pal_time_us(void)
{
    /* Integer math: ticks * 1000 / CPU_TICKS_PER_MSEC == ticks * 1000000 / SYSCLOCK_ARM11.
     * t * 1000000 fits in 64 bits for ~19 hours of ticks, far beyond a run. */
    if (pal_3ds_fake_clock) return (uint64_t)(g_frames_shown * 16000u) + FAKE_READ() * 125u;
    uint64_t t = (uint64_t)(svcGetSystemTick() - g_t0);
    return t * 1000000ull / (uint64_t)SYSCLOCK_ARM11;
}

void pal_sleep_ms(uint32_t ms) { svcSleepThread((s64)ms * 1000000); }

/* ---- audio: ndsp channel 0, ring of wave buffers in linear memory ---- */
#define AUD_BUFS   20
#define AUD_FRAMES 1024                     /* ~46 ms per buffer at 22050 Hz */
static int       g_aud_ok;
static ndspWaveBuf g_wb[AUD_BUFS];
static int16_t  *g_aud_mem;
static int       g_aud_next;
static uint32_t  g_aud_fill;                /* frames in the current buffer */
static uint32_t  g_aud_dropped;

/* ---- mixer thread (New 3DS: core 2): keeps ndsp fed while the guest thread is busy decoding a movie ---- */
static LightLock g_aud_lock;
static Thread    g_aud_thread;
static volatile int g_aud_stop;
void pal_audio_lock(void)
{
    uint64_t t0 = 0; int warned = 0;
    while (LightLock_TryLock(&g_aud_lock)) {          /* nonzero = busy */
        svcSleepThread(30000);
        if (!t0) t0 = svcGetSystemTick();
        else if (!warned && svcGetSystemTick() - t0 > 400ull * CPU_TICKS_PER_MSEC) { warned = 1; pal_log("DBG audio lock wait > 400 ms"); pal_log_flush(); }
    }
}
void pal_audio_unlock(void) { LightLock_Unlock(&g_aud_lock); }
void pal_audio_relax(void)  { svcSleepThread(60000); }   /* 60 us: the polling guest must not re-take the lock before the mixer thread */
static void aud_thread_main(void *arg)
{
    (void)arg;
    unsigned n = 0;
    while (!g_aud_stop) { mixer_thread_tick(); svcSleepThread(4000000); }   /* every 4 ms */
}

int pal3ds_audio_init(void)
{
    if (R_FAILED(ndspInit())) { pal_log("audio: ndspInit failed (no dspfirm.cdc?) - running silent"); return 0; }
    g_aud_mem = linearAlloc(AUD_BUFS * AUD_FRAMES * 4);
    if (!g_aud_mem) { ndspExit(); pal_log("audio: no linear memory"); return 0; }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, 22050.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(0, mix);
    memset(g_wb, 0, sizeof g_wb);
    for (int i = 0; i < AUD_BUFS; i++) { g_wb[i].data_pcm16 = g_aud_mem + i * AUD_FRAMES * 2; g_wb[i].status = NDSP_WBUF_DONE; }
    g_aud_ok = 1;
    pal_log("audio: ndsp up, %d x %d frames", AUD_BUFS, AUD_FRAMES);
    LightLock_Init(&g_aud_lock);
    { bool n3 = false; APT_CheckNew3DS(&n3);
      if (n3) {
          g_aud_stop = 0;
          g_aud_thread = NULL; /* mixer thread disabled: with it the game hung in the first movie (docs/PLAN.md 43) */
          if (g_aud_thread) { mixer_set_threaded(1); pal_log("audio: mixer thread on core 2"); }
          else pal_log("audio: mixer thread FAILED, mixing on the guest thread");
      } else pal_log("audio: old 3DS, mixing on the guest thread"); }
    return 1;
}

void pal3ds_audio_fini(void)
{
    if (!g_aud_ok) return;
    if (g_aud_thread) { g_aud_stop = 1; threadJoin(g_aud_thread, U64_MAX); threadFree(g_aud_thread); g_aud_thread = NULL; mixer_set_threaded(0); }
    ndspChnReset(0);
    ndspExit();
    linearFree(g_aud_mem);
    g_aud_ok = 0;
    pal_log("audio: down, %lu frames dropped, %lu mixer underruns", (unsigned long)g_aud_dropped, (unsigned long)mixer_underruns());
}

/* audiodbg.txt: one log line per submit (wall time, frames so far, peak of the chunk) to see when sound really starts. */
static int g_aud_dbg = -1;
int g_aud_dbg_on;
static void aud_dbg(const int16_t *f, uint32_t n)
{
    static uint64_t total;
    if (g_aud_dbg < 0) { FILE *t = fopen("sdmc:/3ds/tpm/audiodbg.txt", "r"); g_aud_dbg = t != NULL; g_aud_dbg_on = g_aud_dbg; if (t) fclose(t); }
    if (!g_aud_dbg) return;
    int pk = 0;
    for (uint32_t i = 0; i < n * 2; i++) { int v = f[i] < 0 ? -f[i] : f[i]; if (v > pk) pk = v; }
    {   /* audiodbg: the exact PCM that goes to the DSP (22050 Hz stereo s16), with the wall time of each chunk in audio.idx */
        static FILE *raw, *idx; static uint64_t bytes; extern uint32_t pal_time_ms(void);
        if (!raw && bytes == 0) { raw = fopen("sdmc:/3ds/tpm/audio.raw", "wb"); idx = fopen("sdmc:/3ds/tpm/audio.idx", "w"); bytes = 1; }
        if (raw && bytes < 16000000u) { fwrite(f, 4, n, raw); if (idx) fprintf(idx, "%u %llu\n", (unsigned)pal_time_ms(), (unsigned long long)((bytes - 1) / 4)); bytes += (uint64_t)n * 4u; }
        else if (raw) { fclose(raw); raw = NULL; if (idx) { fclose(idx); idx = NULL; } }
    }
    pal_log("aud: submit %u frames (total %llu = %llu ms of audio) peak %d", (unsigned)n, (unsigned long long)total, (unsigned long long)(total * 1000u / 22050u), pk);
    total += n;
}
void pal_audio_submit(const int16_t *frames, uint32_t n)
{
    if (!g_aud_ok) return;
    aud_dbg(frames, n);
    while (n) {
        ndspWaveBuf *b = &g_wb[g_aud_next];
        if (b->status != NDSP_WBUF_DONE && b->status != NDSP_WBUF_FREE) { if (!g_aud_dropped) pal_log("audio: DSP behind, dropping (buf %d status %d)", g_aud_next, (int)b->status); g_aud_dropped += n; return; }  /* DSP behind */
        uint32_t k = AUD_FRAMES - g_aud_fill;
        if (k > n) k = n;
        memcpy(b->data_pcm16 + g_aud_fill * 2, frames, k * 4);
        g_aud_fill += k; frames += k * 2; n -= k;
        if (g_aud_fill == AUD_FRAMES) {
            b->nsamples = AUD_FRAMES;
            DSP_FlushDataCache(b->data_pcm16, AUD_FRAMES * 4);
            ndspChnWaveBufAdd(0, b);
            g_aud_next = (g_aud_next + 1) % AUD_BUFS;
            g_aud_fill = 0;
        }
    }
}

/* ---- video: the game's 640x480 frame, scaled to fit the 400x240 top screen ---- */
static uint32_t g_blit_count;
static uint64_t g_blit_ticks;

/* Per-channel average of two RGB565 pixels in each 16-bit half. The low bit
 * of every channel is masked off first so no carry crosses a channel or pixel
 * boundary. (A plain 16-bit average such as UHADD16 is NOT RGB565-safe: it
 * carries blue into green and green into red - the colour noise of the
 * 2026-11-12 blit.) */
static inline uint32_t avg565x2(uint32_t a, uint32_t b)
{
    return ((a & 0xF7DEF7DEu) >> 1) + ((b & 0xF7DEF7DEu) >> 1);
}

/* Average the two RGB565 pixels packed in p. */
static inline uint16_t avg565h(uint32_t p)
{
    return (uint16_t)(((p & 0x0000F7DEu) >> 1) + (((p >> 16) & 0x0000F7DEu) >> 1));
}

/* Exact (sum of four)>>2 for the odd-width tail, as in the old filter. */
static inline uint16_t avg565x4(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
    uint32_t r = ((a >> 11) + (b >> 11) + (c >> 11) + (d >> 11)) >> 2;
    uint32_t g = (((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((d >> 5) & 63)) >> 2;
    uint32_t bl = ((a & 31) + (b & 31) + (c & 31) + (d & 31)) >> 2;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* Clear only what the image does not overwrite. The framebuffer is column-major
 * (400 columns x 240 rows, row 239 first in each column); the image occupies
 * row 239-y0 down to row 240-y0-oh. */
static void clear_letterbox(uint16_t *fb, uint32_t x0, uint32_t ow, uint32_t y0, uint32_t oh)
{
    uint32_t rx = x0 + ow;
    if (x0) memset(fb, 0, x0 * 240 * sizeof *fb);
    if (rx < 400) memset(fb + rx * 240, 0, (400 - rx) * 240 * sizeof *fb);
    uint32_t top = 240 - y0 - oh;             /* rows above the image */
    if (top || y0) {
        for (uint32_t x = x0; x < rx; x++) {
            uint16_t *col = fb + x * 240;
            if (top) memset(col, 0, top * sizeof *col);
            if (y0) memset(col + (240 - y0), 0, y0 * sizeof *col);
        }
    }
}

extern void ui_poll(void);
extern void ui_frame(void);
void *pal_3ds_top_fb(void) { return gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); }
/* The top screen is double buffered and a swap only takes effect at the next vertical blank. Two swaps within one blank (the loading
 * screen presents once per percent of its bar, in bursts) make the next write go into the buffer the LCD is still scanning out:
 * the picture tears and "waves" at every update on real hardware. So a write that follows a swap by less than a frame waits for
 * the blank first. */
static uint64_t g_last_swap_tick;
void pal_3ds_vsync_guard(void)
{
    if (g_last_swap_tick && svcGetSystemTick() - g_last_swap_tick < 16000ull * CPU_TICKS_PER_MSEC / 1000ull) gspWaitForVBlank();
}
static void swap_top(void) { gfxSwapBuffers(); g_last_swap_tick = svcGetSystemTick(); }
void pal_3ds_top_swap(void) { GSPGPU_FlushDataCache(pal_3ds_top_fb(), 400 * 240 * 2); ui_frame(); swap_top(); }

static uint64_t g_blit_t_conv, g_blit_t_flush, g_blit_t_swap;
/* The game draws its 2D output (the menu screens and the Bink movies) as 4:3 content
 * centred in the display mode it runs in. In widescreen (800x480) that leaves 80 black
 * columns on each side, so the 2x2/nearest paths below would show the menu and the
 * videos pillarboxed (320 of 400 columns) instead of full screen. Fill means a
 * non-uniform scale here - the top screen is 5:3 and the content 4:3 - so crop the
 * centred 4:3 box and box-filter it to the whole 400x240, averaging every source pixel
 * an output pixel covers, per channel in RGB565 (the same rule as the 2x2 path; a plain
 * 16-bit average would carry blue into green). */
static void video_blit_fill43(const uint16_t *src, uint32_t w, uint32_t h, u16 *fb, int bottom_up,
                              uint32_t sx0)
{
    /* The 4:3 box inside the frame, scaled to the 400x240 screen with a box filter. The column and row
     * spans are row/column independent, so both are computed once here: the earlier version recomputed
     * the row span inside the column loop (240 divisions per column) and divided for every averaged
     * pixel, which made one movie frame cost tens of milliseconds - 43 % of the profile during the
     * intro. */
    const uint32_t sw = h * 4u / 3u;
    uint32_t ax[400], bx[400], ay[240], by[240];
    static const uint32_t inv16[17] = { 0, 65536u, 32768u, 21845u, 16384u, 13107u, 10922u, 9362u,
                                        8192u, 7281u, 6553u, 5957u, 5461u, 5041u, 4681u, 4369u, 4096u };
    if (sw > w || !h || sx0 + sw > w) return;
    for (uint32_t x = 0; x < 400u; x++) {
        uint32_t a = sx0 + x * sw / 400u;
        uint32_t b = sx0 + ((x + 1u) * sw + 399u) / 400u;
        if (b <= a) b = a + 1u;
        if (b > sx0 + sw) b = sx0 + sw;
        ax[x] = a;
        bx[x] = b;
    }
    for (uint32_t y = 0; y < 240u; y++) {
        uint32_t a = y * h / 240u, b = ((y + 1u) * h + 239u) / 240u;
        if (b <= a) b = a + 1u;
        if (b > h) b = h;
        ay[y] = a;
        by[y] = b;
    }
    /* Fast path for Bink movies (640x480 box -> 400x240): ~2x2 average per output pixel instead of the general
     * box filter (the blit was ~30 ms of a ~90 ms movie frame, which kept Bink below its 15 fps and starved its
     * audio). Not used for menus (their pixels stay exactly as before). */
    {
        extern int hle_movie_open(void) __attribute__((weak));
        if (hle_movie_open && hle_movie_open() > 0 && sx0 == 0u && sw == 640u && h == 480u) {   /* sx0 == 0: movie mode only, never the menus or the loading screen */
            const uint16_t *r0[240], *r1[240];
            for (uint32_t y = 0; y < 240u; y++) {
                r0[y] = src + (bottom_up ? 479u - 2u * y : 2u * y) * w;
                r1[y] = src + (bottom_up ? 478u - 2u * y : 2u * y + 1u) * w;
            }
            for (uint32_t x = 0; x < 400u; x++) {
                if ((x & 31u) == 0u) pal_yield_to_guest();
                u16 *col = fb + x * 240u + 239u;
                const uint32_t sx = ax[x];
                const uint32_t sx1 = sx + 1u < sx0 + sw ? sx + 1u : sx;
                for (uint32_t y = 0; y < 240u; y++) {
                    uint32_t a = r0[y][sx], b = r0[y][sx1], c = r1[y][sx], d = r1[y][sx1];
                    uint32_t t0 = ((a & 0xF7DEu) >> 1) + ((b & 0xF7DEu) >> 1);
                    uint32_t t1 = ((c & 0xF7DEu) >> 1) + ((d & 0xF7DEu) >> 1);
                    col[-(int)y] = (u16)(((t0 & 0xF7DEu) >> 1) + ((t1 & 0xF7DEu) >> 1));
                }
            }
            return;
        }
    }
    for (uint32_t x = 0; x < 400u; x++) {
        /* Cooperative scheduling: this blit is tens of milliseconds of straight-line work, and Bink's
         * IO/decode thread only gets the CPU at a yield point. Without one here it starved - its
         * compressed-data buffer measured buf=0 against need=12288 from the second frame on, so the
         * decode bailed every frame and the intro movies froze after two pictures. */
        if ((x & 31u) == 0u) pal_yield_to_guest();
        u16 *col = fb + x * 240u + 239u;
        const uint32_t x0 = ax[x], x1 = bx[x], nx = x1 - x0;
        for (uint32_t y = 0; y < 240u; y++) {
            uint32_t r = 0, g = 0, b = 0, n;
            const uint32_t y0 = ay[y], y1 = by[y];
            n = nx * (y1 - y0);
            if (!n) continue;
            for (uint32_t sy = y0; sy < y1; sy++) {
                const uint16_t *row = src + (bottom_up ? h - 1u - sy : sy) * w;
                for (uint32_t sx = x0; sx < x1; sx++) {
                    uint16_t v = row[sx];
                    r += (v >> 11) & 0x1fu; g += (v >> 5) & 0x3fu; b += v & 0x1fu;
                }
            }
            if (n <= 16u) {
                uint32_t k = inv16[n];
                col[-(int)y] = (uint16_t)((((r * k) >> 16) << 11) | (((g * k) >> 16) << 5) | ((b * k) >> 16));
            } else {
                col[-(int)y] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
            }
        }
    }
}


extern void pal_gpu_present_finish(void);
static void video_blit(const uint16_t *src, uint32_t w, uint32_t h, int bottom_up, int fill43)
{
    if (!w || !h) return;
    pal_gpu_present_finish();   /* an asynchronous GPU present still owns the top framebuffer */
    pal_3ds_vsync_guard();
    uint64_t t0 = svcGetSystemTick();
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);    /* 240x400, column-major, y flipped */
    if (fill43 && w * 3u > h * 4u) {
        /* wider than 4:3 (the 800x480 widescreen mode): crop the 4:3 2D content and fill the
         * screen with it. fill43 == 1 is the menu's centred box; fill43 == 2 is a movie, which
         * the game copies at x = 0 (`BinkCopyToBuffer(..., dstx = 0, dsty = 0, ...)`, 0x497beb)
         * and which therefore sits on the left of the widescreen frame. Only the 2D paths ask
         * for this: a staged GPU frame is a real widescreen picture and must not be cropped. */
        uint32_t sw = h * 4u / 3u;
        uint32_t sx0 = fill43 == 2 ? 0u : (w - sw) / 2u;
        video_blit_fill43(src, w, h, fb, bottom_up, sx0);
        goto present;
    }
    /* largest integer-friendly fit: 640x480 -> 320x240 (2x2 box), anything else nearest */
    uint32_t ow = w * 240u / h, oh = 240;
    if (ow > 400) { ow = 400; oh = h * 400u / w; }
    uint32_t x0 = (400 - ow) / 2, y0 = (240 - oh) / 2;
    clear_letterbox(fb, x0, ow, y0, oh);
    if (w == ow * 2 && h == oh * 2) {
        /* 2x2 box: two output pixels at a time (four source pixels per row). */
        uint32_t x = 0;
        for (; x + 1 < ow; x += 2) {
            u16 *c0 = fb + (x0 + x) * 240 + (239 - y0);
            u16 *c1 = c0 + 240;
            uint32_t sx = x * 2;
            for (uint32_t y = 0; y < oh; y++) {
                uint32_t sy = y * 2;
                const uint32_t *r0 = (const uint32_t *)(src + (bottom_up ? h - 1 - sy : sy) * w + sx);
                const uint32_t *r1 = (const uint32_t *)(src + (bottom_up ? h - 2 - sy : sy + 1) * w + sx);
                uint32_t p0 = avg565x2(r0[0], r1[0]);
                uint32_t p1 = avg565x2(r0[1], r1[1]);
                c0[-(int)y] = avg565h(p0);
                c1[-(int)y] = avg565h(p1);
            }
        }
        if (x < ow) {                                   /* odd output width: one 2x2 pixel */
            u16 *c = fb + (x0 + x) * 240 + (239 - y0);
            uint32_t sx = x * 2;
            for (uint32_t y = 0; y < oh; y++) {
                uint32_t sy = y * 2;
                const uint16_t *p = src + (bottom_up ? h - 1 - sy : sy) * w + sx;
                const uint16_t *q = src + (bottom_up ? h - 2 - sy : sy + 1) * w + sx;
                c[-(int)y] = avg565x4(p[0], p[1], q[0], q[1]);
            }
        }
    } else if (w == ow && h == oh) {
        /* already at screen size (GPU downscaled): transpose, rows are bottom-up */
        const int step = bottom_up ? -(int)w : (int)w;
        uint32_t x = 0;
        for (; x + 1 < ow; x += 2) {                    /* 2 columns x 2 rows per step: two 32-bit reads, two 32-bit writes */
            u16 *c0 = fb + (x0 + x) * 240 + (239 - y0);
            u16 *c1 = c0 + 240;
            const u16 *sp = src + (bottom_up ? (h - 1) * w : 0) + x;
            uint32_t y = 0;
            if (!(y0 & 1u) && !(oh & 1u)) {
                for (; y < oh; y += 2, sp += 2 * step) {
                    uint32_t v0 = *(const uint32_t *)sp;                      /* row y:   px x, x+1 */
                    uint32_t v1 = *(const uint32_t *)(sp + step);             /* row y+1: px x, x+1 */
                    /* column memory runs y-descending: [-(y+1)] low half, [-y] high half; 238-y-y0 is even */
                    *(uint32_t *)(c0 - (int)y - 1) = (v1 & 0xffffu) | (v0 << 16);
                    *(uint32_t *)(c1 - (int)y - 1) = (v1 >> 16) | (v0 & 0xffff0000u);
                }
            }
            for (; y < oh; y++, sp += step) {
                uint32_t v = *(const uint32_t *)sp;
                c0[-(int)y] = (u16)v;
                c1[-(int)y] = (u16)(v >> 16);
            }
        }
        for (; x < ow; x++) {
            u16 *col = fb + (x0 + x) * 240 + (239 - y0);
            const u16 *sp = src + (bottom_up ? (h - 1) * w : 0) + x;
            for (uint32_t y = 0; y < oh; y++, sp += step) col[-(int)y] = *sp;
        }
    } else {
        for (uint32_t x = 0; x < ow; x++) {
            u16 *col = fb + (x0 + x) * 240 + (239 - y0);
            uint32_t sx = x * w / ow;
            for (uint32_t y = 0; y < oh; y++) {
                uint32_t sy = y * h / oh;
                col[-(int)y] = src[(bottom_up ? h - 1 - sy : sy) * w + sx];
            }
        }
    }
present:
    /* Only the top screen was written; flush exactly that framebuffer. */
    uint64_t t1 = svcGetSystemTick();
    GSPGPU_FlushDataCache(fb, 400 * 240 * sizeof *fb);
    uint64_t t2 = svcGetSystemTick();
    ui_frame();
    swap_top();
    g_blit_t_conv += t1 - t0; g_blit_t_flush += t2 - t1; g_blit_t_swap += svcGetSystemTick() - t2;
    g_blit_ticks += (uint64_t)(svcGetSystemTick() - t0);
    if (++g_blit_count % 300 == 0) {
        pal_log("flip: conv %lu us, flush %lu us, swap %lu us (avg of %lu)", (unsigned long)(g_blit_t_conv * 1000ull / ((uint64_t)CPU_TICKS_PER_MSEC * g_blit_count)),
                (unsigned long)(g_blit_t_flush * 1000ull / ((uint64_t)CPU_TICKS_PER_MSEC * g_blit_count)),
                (unsigned long)(g_blit_t_swap * 1000ull / ((uint64_t)CPU_TICKS_PER_MSEC * g_blit_count)), (unsigned long)g_blit_count);
        pal_log("flip: %lu blits, avg %lu us (blit+flush+swap)",
                (unsigned long)g_blit_count,
                (unsigned long)(g_blit_ticks * 1000ull / ((uint64_t)CPU_TICKS_PER_MSEC * g_blit_count)));
    }
}
void pal_video_present(const uint16_t *src, uint32_t w, uint32_t h)
{
    pal3ds_count_frame();
    {   /* menu timing: log the average cost of a 2D present every 20 calls */
        static uint64_t tsum; static uint32_t n;
        uint64_t t0 = svcGetSystemTick();
        video_blit(src, w, h, 0, 1);   /* 2D content: fill the screen in widescreen */
        tsum += svcGetSystemTick() - t0;
        if (++n == 20u) { pal_log("menu: 2D present avg %lu us (20 frames, %ux%u)", (unsigned long)(tsum * 1000ull / ((uint64_t)CPU_TICKS_PER_MSEC * 20u)), (unsigned)w, (unsigned)h); n = 0; tsum = 0; }
    }
}

void pal_video_present_flip(const uint16_t *src, uint32_t w, uint32_t h)
{
    pal3ds_count_frame();
    video_blit(src, w, h, 1, 0);   /* staging is a full-widescreen frame: never crop */
}

void pal_video_present_movie(const uint16_t *src, uint32_t w, uint32_t h)
{
    pal3ds_count_frame();
    video_blit(src, w, h, 0, 2);   /* movie: crop the left-aligned 4:3 picture and fill the screen */
}

/* ---- input: buttons as DirectInput keys (layout below). HOME/power close via APT. ---- */
/* Layout (the game's default key bindings: Ctrl attack, X jump, Z force push, Alt roll, Space use,
 * Shift run, "," / "." previous / next weapon, 1..6 select weapon):
 *   D-pad up/down, Circle Pad  move + turn   C-Stick  free camera look (core/camera_look.c)
 *   D-pad left / right  roll left / right (arrow + Alt: the game's side roll; a held arrow also keeps turning)
 *   A  Return (menu confirm)   B  jump (X key)   X  attack (Ctrl)   Y  use (Space)
 *   L  run (Shift)     R  force push (Z)     ZR roll (Alt)   ZL next weapon (".")   Select  previous weapon (",")
 *   Start  Escape      ZL/ZR/C-Stick exist on New 3DS / 2DS XL only
 * Several buttons may map to one key (any of them held = key down). */
static const struct { u32 key; uint8_t dik; } k_map[] = {
    {KEY_DUP | KEY_CPAD_UP, 0xc8}, {KEY_DDOWN | KEY_CPAD_DOWN, 0xd0},
    {KEY_DLEFT | KEY_CPAD_LEFT, 0xcb}, {KEY_DRIGHT | KEY_CPAD_RIGHT, 0xcd},   /* the arrow comes first, the Alt below follows it */
    {KEY_A, 0x1c}, {KEY_B, 0x2d}, {KEY_ZR, 0x38}, {KEY_X, 0x1d}, {KEY_Y, 0x39},
    {KEY_L, 0x2a}, {KEY_R, 0x2c}, {KEY_ZL, 0x34}, {KEY_SELECT, 0x33}, {KEY_START, 0x01},
    {KEY_DLEFT, 0x38}, {KEY_DRIGHT, 0x38},   /* D-pad left / right = arrow + Alt = roll sideways (ZR still rolls too) */
};
#define NMAP (int)(sizeof k_map / sizeof k_map[0])
static uint8_t  g_down[NMAP];
static uint32_t g_last_apt;
static int      g_closing;

/* Free bytes in the linear heap (GPU targets, textures, ndsp, the video frames).
 * Logged once per 300 frames by the RAM trace in core/render/r3d.c. */
uint32_t pal_linear_free(void) { return (uint32_t)linearSpaceFree(); }
extern u32 __ctru_heap_size;
uint32_t pal_host_free(void) { struct mallinfo m = mallinfo(); return __ctru_heap_size > (u32)m.uordblks ? __ctru_heap_size - (u32)m.uordblks : 0; }

int pal3ds_closing(void) { return g_closing; }
void pal3ds_request_exit(void) { g_closing = 1; }

/* C-Stick position (-156..156, up = positive y); 0,0 on a plain 3DS. Used by core/camera_look.c. */
int pal3ds_cstick(int *x, int *y)
{
    circlePosition p = {0, 0};
    hidCstickRead(&p);
    *x = p.dx; *y = p.dy;
    return 1;
}


/* ---- HOME / sleep: runs inside aptMainLoop, before the other hooks (citro3d's) when the app is suspended ---- */
int g_svc_notrace;
static aptHookCookie g_apt_cookie;
static void apt_hook(APT_HookType hook, void *param)
{
    (void)param;
    extern void pal_gpu_quiesce(void);
    extern void r3d_q_fence(void);
    extern volatile int g_svc_trace;
    if (hook == APTHOOK_ONSUSPEND) { if (!g_svc_notrace) { pal_log("apt: suspend"); pal_log_flush(); } r3d_q_fence(); pal_gpu_quiesce(); g_svc_trace = !g_svc_notrace; }
    else if (hook == APTHOOK_ONRESTORE) { g_svc_trace = 0; pal_log("apt: restore"); pal_log_flush(); }
    else if (hook == APTHOOK_ONSLEEP) { if (!g_svc_notrace) { pal_log("apt: sleep"); pal_log_flush(); } r3d_q_fence(); pal_gpu_quiesce(); g_svc_trace = !g_svc_notrace; }
    else if (hook == APTHOOK_ONWAKEUP) { g_svc_trace = 0; pal_log("apt: wakeup"); pal_log_flush(); }
    else if (hook == APTHOOK_ONEXIT) { pal_log("apt: exit"); pal_log_flush(); }
}
/* Diagnostic: every hook registered before ours (libctru's gfx / ndsp, citro3d's) is wrapped so log.txt shows which one a
 * suspend stops in. aptHook() keeps the previous head's {next, callback, param} inside the new cookie, so the chain starting
 * at our cookie holds all older callbacks. */
static aptHookFn g_apt_orig[8];
#define APT_WRAP(i) static void apt_wrap##i(APT_HookType h, void *p) { pal_log("apt: hook %d (%p) type %d enter", i, (void *)g_apt_orig[i], (int)h); pal_log_flush(); g_apt_orig[i](h, p); pal_log("apt: hook %d exit", i); pal_log_flush(); }
APT_WRAP(0) APT_WRAP(1) APT_WRAP(2) APT_WRAP(3) APT_WRAP(4) APT_WRAP(5) APT_WRAP(6) APT_WRAP(7)
void pal_apt_hook_init(void)
{
    static aptHookFn const wraps[8] = { apt_wrap0, apt_wrap1, apt_wrap2, apt_wrap3, apt_wrap4, apt_wrap5, apt_wrap6, apt_wrap7 };
    aptHook(&g_apt_cookie, apt_hook, NULL);
    int n = 0;
    for (aptHookCookie *c = &g_apt_cookie; c && n < 8; c = c->next, n++) {
        if (!c->callback) break;
        g_apt_orig[n] = c->callback;
        c->callback = wraps[n];
    }
    pal_log("apt: %d older hook(s) wrapped", n);
}
int pal_input_next(pal_input_event *e)
{
    uint32_t now = pal_time_ms();
    if (now - g_last_apt >= 16 || g_last_apt == 0) {
        g_last_apt = now ? now : 1;
        if (!g_closing) {
            /* HOME / sleep: libctru suspends the app inside aptMainLoop (hooks hand the GPU and the DSP back and block until
             * the app is brought back); apt_hook logs both ends and idles the GPU first, so if the console ever stops here
             * log.txt on the SD card shows how far it got. */
            uint64_t t0 = svcGetSystemTick();
            int alive = aptMainLoop();
            uint32_t ms = (uint32_t)((svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC);
            if (ms > 100) { pal_log("apt: aptMainLoop took %lu ms, returned %d", (unsigned long)ms, alive); pal_log_flush(); }
            if (aptCheckHomePressRejected()) { extern void ui_home_pressed(void); ui_home_pressed(); }
            if (!alive) g_closing = 1;
        }
        if (!g_closing) { hidScanInput(); ui_poll(); }
    }
    if (g_closing) { e->type = PAL_IN_QUIT; e->code = 0; e->a = e->b = 0; return 1; }
    u32 held = hidKeysHeld();
    for (int i = 0; i < NMAP; i++) {
        uint8_t d = (held & k_map[i].key) != 0;
        if (d != g_down[i]) {
            g_down[i] = d;
            e->type = PAL_IN_KEY; e->code = k_map[i].dik; e->a = d; e->b = 0;
            return 1;
        }
    }
    return 0;
}

/* ---- threads (libctru). Guest threads hand a run token around, so priority
 * only matters relative to the audio thread; all run on the app core. ---- */
#include <stdlib.h>
struct pal_thread { Thread t; };
struct pal_sem { LightSemaphore s; };
pal_thread *pal_thread_spawn(void (*fn)(void *), void *arg, uint32_t stack_bytes)
{
    struct pal_thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    t->t = threadCreate(fn, arg, stack_bytes, prio, -2, false);
    if (!t->t) { pal_log("pal: threadCreate(%lu bytes) failed", (unsigned long)stack_bytes); free(t); return NULL; }
    return t;
}
pal_thread *pal_thread_spawn_bg(void (*fn)(void *), void *arg, uint32_t stack_bytes)
{
    struct pal_thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    bool is_new = false;
    APT_CheckNew3DS(&is_new);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (is_new) t->t = threadCreate(fn, arg, stack_bytes, prio, 2, false);
    if (!t->t) t->t = threadCreate(fn, arg, stack_bytes, 0x3f, -2, false);
    if (!t->t) { free(t); return NULL; }
    return t;
}
void pal_thread_wait(pal_thread *t)
{
    if (!t) return;
    threadJoin(t->t, U64_MAX);
    threadFree(t->t);
    free(t);
}
pal_sem *pal_sem_new(int initial) { struct pal_sem *s = calloc(1, sizeof *s); if (s) LightSemaphore_Init(&s->s, initial, 0x7fff); return s; }
void pal_sem_wait(pal_sem *s) { LightSemaphore_Acquire(&s->s, 1); }
void pal_sem_post(pal_sem *s) { LightSemaphore_Release(&s->s, 1); }
void pal_sem_free(pal_sem *s) { free(s); }

/* ---- directories: FSDIR_Read returns name, attributes and size together. The
 * stdio version needs a stat() per entry, ~30 ms each on the SD card. ---- */
#define DIR_BATCH 32
struct pal_dir { Handle h; FS_Archive arc; FS_DirectoryEntry e[DIR_BATCH]; u32 n, pos; };

extern int g_pal_dir_open;
pal_dir *pal_dir_open(const char *path)
{
    if (strncmp(path, "sdmc:", 5) != 0) return NULL;
    struct pal_dir *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    if (R_FAILED(FSUSER_OpenArchive(&d->arc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) { free(d); return NULL; }
    if (R_FAILED(FSUSER_OpenDirectory(&d->h, d->arc, fsMakePath(PATH_ASCII, path + 5)))) {
        FSUSER_CloseArchive(d->arc);
        free(d);
        return NULL;
    }
    g_pal_dir_open++;
    return d;
}

int pal_dir_next(pal_dir *d, char *name, size_t n, int *is_dir, uint32_t *size)
{
    if (d->pos == d->n) {                       /* one IPC per batch, not per entry */
        d->pos = d->n = 0;
        if (R_FAILED(FSDIR_Read(d->h, &d->n, DIR_BATCH, d->e))) d->n = 0;
        if (!d->n) return 0;
    }
    const FS_DirectoryEntry e = d->e[d->pos++];
    ssize_t len = utf16_to_utf8((uint8_t *)name, e.name, n - 1);
    name[len > 0 ? (size_t)len : 0] = 0;
    *is_dir = (e.attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
    *size = (uint32_t)e.fileSize;
    return 1;
}

void pal_dir_close(pal_dir *d)
{
    if (!d) return;
    g_pal_dir_open--;
    FSDIR_Close(d->h);
    FSUSER_CloseArchive(d->arc);
    free(d);
}
