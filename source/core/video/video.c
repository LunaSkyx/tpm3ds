void hle_clock_present(void);
__attribute__((weak)) void hle_clock_present(void) {}   /* real one: core/hle/hle_kernel32.c; small tests link only the video code */
#include <string.h>
#include "core/prof.h"
#include "core/video/video.h"
#include "pal/pal.h"

uint32_t g_screen_w = 640, g_screen_h = 480;
static uint16_t g_frame[VIDEO_MAX_W * VIDEO_MAX_H];
static uint32_t g_w, g_h, g_count;
/* Last presented source; the GPU staging path defers filling g_frame until a
 * screenshot actually needs it. */
static const uint16_t *g_last_src;
static int g_last_flip;          /* source rows run bottom-up */
static int g_frame_valid;
#define MAX_CAPTURES 8
static uint32_t g_caps[MAX_CAPTURES];
static int g_ncaps;

void video_request_capture(uint32_t frame)
{
    if (!frame || g_ncaps >= MAX_CAPTURES) return;
    g_caps[g_ncaps++] = frame;
}

static int capture_now(uint32_t frame)
{
    for (int i = 0; i < g_ncaps; i++) {
        if (g_caps[i] > frame) continue;
        for (int k = i + 1; k < g_ncaps; k++) g_caps[k - 1] = g_caps[k];
        g_ncaps--;
        return 1;
    }
    return 0;
}

static void frame_from_source(void)
{
    if (g_frame_valid) return;
    if (!g_last_src) { memset(g_frame, 0, sizeof g_frame); g_frame_valid = 1; return; }
    uint32_t w = g_w, h = g_h;
    if (w > VIDEO_MAX_W) w = VIDEO_MAX_W;
    if (h > VIDEO_MAX_H) h = VIDEO_MAX_H;
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t *s = g_last_src + (g_last_flip ? g_h - 1 - y : y) * g_w;
        memcpy(g_frame + y * w, s, w * 2);
    }
    g_frame_valid = 1;
}

int video_present_silent;
/* See pal_3ds.c: give Bink's IO/decode thread the CPU during a long present. The real one is in
 * gthread.c; small test binaries link only the video code (same pattern as hle_clock_present). */
__attribute__((weak)) void pal_yield_to_guest(void) {}
__attribute__((weak)) int hle_movie_open(void) { return 0; }
static int frame_left_margin_black(uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y += 8u)
        for (uint32_t x = 0; x < 60u && x < w; x += 4u)
            if (g_frame[y * w + x]) return 0;
    return 1;
}
/* A movie frame is the only 2D frame that leaves a wide black margin on the right: the decoder
 * copies the picture at x = 0 (`BinkCopyToBuffer(..., dstx = 0, ...)`) and the rest of the
 * widescreen surface stays black. Detect that instead of trusting an open-file count (the intro
 * files are opened through a path that does not keep one). */
static int frame_right_margin_black(uint32_t w, uint32_t h)
{
    if (w < 32u) return 0;
    for (uint32_t y = 0; y < h; y += 8u)
        for (uint32_t x = w - w / 8u; x < w; x += 4u)
            if (g_frame[y * w + x]) return 0;
    return 1;
}   /* set for the locked-primary present: show the pixels but do not
 * count a frame or advance the clock (the game's own loop does that) */
/* Movie pacing. Every Bink movie here is 15 fps. Bink starts each frame on a steady grid, but frames differ in decode
 * cost (alternating ~13 ms in the logo), so they reached the screen with uneven gaps (53, 80, 53, 80 ms = jitter).
 * Show a frame no earlier than one frame period after the previous one; a frame that is already late goes out at
 * once, and a gap longer than 3 periods (new movie, pause, loading) restarts the grid. */
#define MOVIE_PERIOD_US 66667u
static void movie_pace_report(uint32_t n, uint64_t sum, uint64_t sumsq, uint32_t mx, uint32_t mn, uint32_t fast, uint32_t slow)
{
    if (n < 2) return;
    double m = (double)sum / n, v = (double)sumsq / n - m * m;
    unsigned long sdx = v > 0 ? (unsigned long)(__builtin_sqrt(v) / 100.0) : 0ul;   /* 0.1 ms units */
    unsigned long mean = (unsigned long)(m / 100.0);
    pal_log("movie: %lu frames, display gap mean %lu.%lu ms, sd %lu.%lu, min %lu max %lu, %lu under 55 ms, %lu over 80 ms",
            (unsigned long)n, mean / 10ul, mean % 10ul, sdx / 10ul, sdx % 10ul, (unsigned long)mn / 1000ul,
            (unsigned long)mx / 1000ul, (unsigned long)fast, (unsigned long)slow);
}
static void movie_pace(void)
{
    static uint64_t last, shown, sum, sumsq;
    static uint32_t n, mx, mn = 0xffffffffu, fast, slow;
    uint64_t now = pal_time_us();
    if (!last || now > last + 3u * MOVIE_PERIOD_US) {
        movie_pace_report(n, sum, sumsq, mx, mn, fast, slow);   /* the previous movie ended: report it */
        n = mx = fast = slow = 0; mn = 0xffffffffu; sum = sumsq = 0; shown = 0;
        last = now;
        return;
    }
    uint64_t target = last + MOVIE_PERIOD_US;
    while (now < target) {
        pal_yield_to_guest();                        /* Bink's IO thread keeps reading ahead meanwhile */
        now = pal_time_us();
        if (now + 1500u < target) pal_sleep_ms(1);
    }
    last = target > now ? target : now;
    if (shown) {                                     /* statistics of the real display gaps for the log */
        uint32_t g = (uint32_t)(last - shown);
        n++; sum += g; sumsq += (uint64_t)g * g;
        if (g > mx) mx = g;
        if (g < mn) mn = g;
        if (g < 55000u) fast++;
        if (g > 80000u) slow++;
    }
    shown = last;
}
void video_present(gptr src, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bits, uint32_t gmask,
                   const uint32_t *pal)
{
    if (w > VIDEO_MAX_W) w = VIDEO_MAX_W;
    if (h > VIDEO_MAX_H) h = VIDEO_MAX_H;
    if (video_present_silent && bits == 16 && gmask == 0x07e0 && pitch == w * 2u) {
        /* Movie frames (the locked primary, silent): scan the guest frame in place and blit straight from guest
         * memory - the 800x480 copy into g_frame was ~10 ms of a ~60 ms movie frame. g_frame is only filled
         * later if a screenshot asks for it (frame_from_source). */
        const uint16_t *gs = (const uint16_t *)gmem_span(src, pitch * h, 0);
        if (gs && w >= 32u) {
            int black = 1;
            for (uint32_t y = 0; y < h && black; y += 8u)
                for (uint32_t x = w - w / 8u; x < w; x += 4u)
                    if (gs[y * w + x]) { black = 0; break; }
            if (black) {
                extern void hle_movie_touch(uint32_t ms);
                g_w = w; g_h = h;
                hle_clock_present();
                g_last_src = gs; g_last_flip = 0; g_frame_valid = 0;
                if (capture_now(g_count)) frame_from_source();
                hle_movie_touch(800);
                movie_pace();
                pal_video_present_movie(gs, w, h);
                return;
            }
        }
    }
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *d = g_frame + y * w;
        if (bits == 16) {
            const uint8_t *s = gmem_span(src + y * pitch, w * 2, 0);
            if (!s) return;
            if (gmask == 0x07e0) memcpy(d, s, w * 2);
            else for (uint32_t x = 0; x < w; x++) {        /* 555 -> 565 */
                uint16_t v = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
                d[x] = (uint16_t)(((v & 0x7fe0u) << 1) | (v & 0x1fu) | ((v >> 4) & 0x20u));
            }
        } else if (bits == 8 && pal) {
            const uint8_t *s = gmem_span(src + y * pitch, w, 0);
            if (!s) return;
            for (uint32_t x = 0; x < w; x++) {
                uint32_t e = pal[s[x]];
                d[x] = (uint16_t)(((e & 0xf8u) << 8) | ((e >> 5) & 0x7e0u) | ((e >> 19) & 0x1fu));
            }
        } else {
            memset(d, 0, w * 2);
        }
    }
    g_w = w;
    g_h = h;
    /* The clock base must fold in elapsed time on every present, movie frames included: the movie
     * player presents through the locked-primary path ("silent": not counted), and if the base is
     * only updated on counted frames the guest clock stalls for the whole movie and then jumps by
     * seconds on the next counted frame - which leaves Bink's frame timing (32-bit microseconds)
     * permanently "not ready" and freezes the intro movies on their first frame. */
    hle_clock_present();
    if (!video_present_silent) g_count++;
    g_last_src = g_frame;
    g_last_flip = 0;
    g_frame_valid = 1;
    capture_now(g_count);
    if (video_present_silent && frame_right_margin_black(w, h)) {
        extern void hle_movie_touch(uint32_t ms);
        hle_movie_touch(800);
        movie_pace();
        pal_video_present_movie(g_frame, w, h);   /* a movie written into the locked primary */
    }
    else if (!video_present_silent && hle_movie_open() > 0 && frame_right_margin_black(w, h) && !frame_left_margin_black(w, h))
        pal_video_present_movie(g_frame, w, h);   /* the movie's last frame, presented by the game's own path as the menu
                                                   * comes up: still the left-aligned picture, not the centred menu box */
    else pal_video_present(g_frame, w, h);
}

/* The 3DS GPU path: the frame already exists in linear memory (bottom-up).
 * Present it directly; g_frame is only materialised for screenshots. */
void video_present_staging(const uint16_t *src, uint32_t w, uint32_t h)
{
    if (!src || !w || !h || w > VIDEO_MAX_W || h > VIDEO_MAX_H) return;
    g_w = w;
    g_h = h;
    if (!video_present_silent) { hle_clock_present(); g_count++; }
    g_last_src = src;
    g_last_flip = 1;
    g_frame_valid = 0;
    if (capture_now(g_count)) frame_from_source();
    prof_sub(PROF_SUB_FLIP_BLIT);
    pal_video_present_flip(src, w, h);
    prof_sub(PROF_SUB_NONE);
}

/* Direct present (the GPU transfer wrote the screen): a copy is only made when a screenshot is due. */
uint16_t *video_direct_begin(void)
{
    for (int i = 0; i < g_ncaps; i++) if (g_caps[i] <= g_count + 1) return g_frame;
    return NULL;
}

static void frame_hist(void)
{
    static uint64_t last;
    static uint32_t b[6], n;
    static uint64_t sum[6];
    uint64_t now = pal_time_us();
    if (last) {
        uint32_t ms = (uint32_t)((now - last) / 1000u);
        int k = ms < 25 ? 0 : ms < 33 ? 1 : ms < 40 ? 2 : ms < 60 ? 3 : ms < 100 ? 4 : 5;
        if (ms > 100) pal_log("slow frame %u ms (frame %u)", ms, (unsigned)g_count);
        b[k]++; sum[k] += now - last;
        if (++n % 300 == 0) {
            pal_log("frames/300: <25:%u 25-33:%u 33-40:%u 40-60:%u 60-100:%u >100:%u (avg us %llu/%llu/%llu/%llu/%llu/%llu)",
                    b[0], b[1], b[2], b[3], b[4], b[5],
                    (unsigned long long)(b[0] ? sum[0] / b[0] : 0), (unsigned long long)(b[1] ? sum[1] / b[1] : 0),
                    (unsigned long long)(b[2] ? sum[2] / b[2] : 0), (unsigned long long)(b[3] ? sum[3] / b[3] : 0),
                    (unsigned long long)(b[4] ? sum[4] / b[4] : 0), (unsigned long long)(b[5] ? sum[5] / b[5] : 0));
            memset(b, 0, sizeof b); memset(sum, 0, sizeof sum);
        }
    }
    last = now;
}

void video_direct_done(uint32_t w, uint32_t h, int copied)
{
    frame_hist();
    g_w = w;
    g_h = h;
    if (!video_present_silent) { hle_clock_present(); g_count++; }
    g_last_src = copied ? g_frame : NULL;
    g_last_flip = 0;
    g_frame_valid = copied;
    capture_now(g_count);
}

uint32_t video_frame_count(void) { return g_count; }

const uint16_t *video_last_frame(uint32_t *w, uint32_t *h)
{
    frame_from_source();
    *w = g_w;
    *h = g_h;
    return g_frame;
}

int video_save_bmp(const char *path)
{
    frame_from_source();
    if (!g_w || !g_h) return 0;
    uint32_t row = g_w * 3, padded = (row + 3) & ~3u, size = 54 + padded * g_h;
    uint8_t hdr[54] = {'B', 'M'};
    #define PUT32(o, v) do { hdr[o] = (uint8_t)(v); hdr[o + 1] = (uint8_t)((v) >> 8); \
                             hdr[o + 2] = (uint8_t)((v) >> 16); hdr[o + 3] = (uint8_t)((v) >> 24); } while (0)
    PUT32(2, size); PUT32(10, 54); PUT32(14, 40); PUT32(18, g_w); PUT32(22, g_h);
    hdr[26] = 1; hdr[28] = 24;
    PUT32(34, padded * g_h);
    pal_wfile *f = pal_wfile_create(path);
    if (!f) return 0;
    pal_wfile_write(f, hdr, sizeof hdr);
    static uint8_t line[VIDEO_MAX_W * 3 + 4];
    for (uint32_t y = g_h; y-- > 0;) {
        memset(line, 0, padded);
        for (uint32_t x = 0; x < g_w; x++) {
            uint16_t v = g_frame[y * g_w + x];
            line[3 * x + 0] = (uint8_t)((v & 0x1f) << 3 | (v & 0x1f) >> 2);
            line[3 * x + 1] = (uint8_t)(((v >> 5) & 0x3f) << 2 | ((v >> 5) & 0x3f) >> 4);
            line[3 * x + 2] = (uint8_t)((v >> 11) << 3 | (v >> 11) >> 2);
        }
        pal_wfile_write(f, line, padded);
    }
    return pal_wfile_close(f);
}
