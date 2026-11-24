#include <stdio.h>
#include <string.h>
#include "core/audio/mixer.h"
#include "pal/pal.h"

/* The mixer may run on its own thread (3DS: core 2): every public entry point takes this lock. Weak no-ops
 * here, real ones in pal_3ds.c. */
__attribute__((weak)) void pal_audio_lock(void) {}
__attribute__((weak)) void pal_audio_unlock(void) {}
__attribute__((weak)) void pal_audio_relax(void) {}   /* lets the mixer thread win the lock after a poll */
static int g_threaded;
static uint32_t g_underruns, g_starves, g_clipped;   /* g_clipped: output samples that hit the 16-bit limit (hard clip = crackle) */
static void service_u(void);
static uint64_t g_movie_since_us;   /* when the current movie stretch began (0: none): a movie's first seconds get the large lead */

typedef struct {
    int      used, status;
    gptr     data;
    uint32_t len;                     /* bytes */
    int      fmt, channels, bits, rate, block_align;
    uint32_t frames;                  /* total frames */
    uint64_t pos;                     /* 32.32 frame position */
    uint64_t step;
    float    vol, pan;
    int      loops, played;
    /* streaming (Miles double buffering): two guest buffers played alternately */
    int      ever_starved;            /* this stream has run dry once: the measured movie speed is not trusted any more */
    uint64_t played_frames;           /* source frames of finished buffers (stream time for the debug log) */
    uint64_t t_start_us;              /* wall time of the stream's first buffer */
    uint32_t skip_pending;            /* source frames of silence to fast-forward through (movie A/V resync) */
    int      first;                   /* stream has not played yet */
    uint32_t fade;                    /* frames of fade-in left (stream start / restart after starving) */
    int      stream, scur;            /* scur: buffer playing, -1 starved */
    int      starved;
    float    smul;                    /* movie stream speed (0 = 1.0): follows Bink's decode speed, see mixer_render */
    int      sok;                     /* buffers switched since the last starve */
    gptr     sbuf[2];
    uint32_t slen[2], spos[2];        /* bytes; spos == slen once played */
    int      sfull[2], send;          /* loaded and not yet played; end-of-stream seen */
    uint32_t eob;                     /* buffers finished so far */
    /* IMA ADPCM decode state: decoded block cache */
    int16_t  blk[2 * 2048];
    int32_t  blk_index;               /* block number in blk, -1 none */
    uint32_t blk_first;               /* first frame of that block (no per-sample division) */
    uint32_t blk_frames;
} voice;

static voice    g_v[MIX_VOICES];
static float    g_master = 1.0f;
static uint64_t g_last_us;
static int      g_clock_started;

static int mixer_alloc_u(void)
{
    for (int i = 0; i < MIX_VOICES; i++)
        if (!g_v[i].used) {
            memset(&g_v[i], 0, sizeof g_v[i]);
            g_v[i].used = 1;
            g_v[i].status = VS_DONE;
            g_v[i].vol = 1.0f;
            g_v[i].loops = 1;
            g_v[i].blk_index = -1;
            return i;
        }
    pal_log("mixer: no free voice");
    return -1;
}

static void mixer_release_u(int v) { if (v >= 0 && v < MIX_VOICES) g_v[v].used = 0; }

static voice *V(int v) { return (v >= 0 && v < MIX_VOICES && g_v[v].used) ? &g_v[v] : NULL; }

static int mixer_set_data_u(int v, gptr data, uint32_t len, int fmt, int channels, int bits, int rate, int block_align)
{
    voice *x = V(v);
    if (!x || channels < 1 || channels > 2 || rate <= 0) return 0;
    if (fmt == 1 && bits != 8 && bits != 16) return 0;
    if (fmt == 0x11 && (bits != 4 || block_align <= 4 * channels)) return 0;
    if (fmt != 1 && fmt != 0x11) { pal_log("mixer: unsupported wave format %d", fmt); return 0; }
    x->data = data; x->len = len; x->fmt = fmt; x->channels = channels; x->bits = bits;
    x->rate = rate; x->block_align = block_align;
    if (fmt == 1) x->frames = len / (uint32_t)(channels * bits / 8);
    else {
        uint32_t per_blk = (uint32_t)((block_align - 4 * channels) * 8 / (4 * channels) + 1);
        x->frames = len / (uint32_t)block_align * per_blk;
        x->blk_frames = per_blk;
    }
    x->step = ((uint64_t)rate << 32) / MIX_RATE;
    x->pos = 0;
    x->status = VS_DONE;
    x->blk_index = -1;
    return 1;
}

static int mixer_set_wav_u(int v, gptr w)
{
    if (gmem_rd32(w) != 0x46464952u || gmem_rd32(w + 8) != 0x45564157u) return 0;   /* RIFF / WAVE */
    uint32_t total = gmem_rd32(w + 4) + 8, off = 12;
    int fmt = 0, ch = 0, bits = 0, rate = 0, align = 0;
    while (off + 8 <= total) {
        uint32_t id = gmem_rd32(w + off), n = gmem_rd32(w + off + 4);
        if (id == 0x20746d66u) {                            /* "fmt " */
            fmt = gmem_rd16(w + off + 8);
            ch = gmem_rd16(w + off + 10);
            rate = (int)gmem_rd32(w + off + 12);
            align = gmem_rd16(w + off + 20);
            bits = gmem_rd16(w + off + 22);
        } else if (id == 0x61746164u) {                     /* "data" */
            return mixer_set_data_u(v, w + off + 8, n, fmt, ch, bits, rate, align);
        }
        off += 8 + ((n + 1) & ~1u);
    }
    return 0;
}

/* ---- streaming ---- */
/* Movie audio starts abruptly (first Bink block, or the first block after the stream ran dry): fade in over ~23 ms so the step is not a click. */
#define STREAM_FADE 512u
#define STREAM_AV_SKIP_MS 0u
static int mixer_stream_begin_u(int v, int channels, int bits, int rate)
{
    voice *x = V(v);
    if (!x || channels < 1 || channels > 2 || (bits != 8 && bits != 16) || rate <= 0) return 0;
    x->fmt = 1; x->channels = channels; x->bits = bits; x->rate = rate;
    x->step = ((uint64_t)rate << 32) / MIX_RATE;
    x->stream = 1; x->scur = -1; x->send = 0;
    x->first = 1;
    x->played_frames = 0;
    x->smul = 0.0f; x->ever_starved = 0;
    x->sfull[0] = x->sfull[1] = 0;
    x->slen[0] = x->slen[1] = x->spos[0] = x->spos[1] = 0;
    x->data = 0; x->len = 0; x->frames = 0; x->pos = 0;
    x->status = VS_DONE;
    return 1;
}

static void stream_switch(voice *x)
{
    int n = x->scur < 0 ? 0 : x->scur ^ 1;
    if (!x->sfull[n] && x->sfull[n ^ 1]) n ^= 1;
    if (!x->sfull[n]) { x->scur = -1; x->frames = 0; if (x->send) x->status = VS_DONE; return; }
    x->scur = n;
    x->data = x->sbuf[n];
    x->len = x->slen[n];
    x->frames = x->len / (uint32_t)(x->channels * x->bits / 8);
    x->pos = 0;
}

static void mixer_stream_load_u(int v, int n, gptr data, uint32_t len)
{
    voice *x = V(v);
    if (!x || !x->stream || n < 0 || n > 1) return;
    if (len == 0) { x->send = 1; return; }
    x->sbuf[n] = data; x->slen[n] = len; x->spos[n] = 0; x->sfull[n] = 1; x->send = 0;
    {   extern int g_aud_dbg_on; static int cnt;
        if (g_aud_dbg_on && cnt < 200) {
            int pk = 0; uint32_t m = len < 8192u ? len : 8192u;
            if (x->bits == 8) for (uint32_t k = 0; k < m; k++) { int v = ((int)gmem_rd8(data + k) - 128) << 8; if (v < 0) v = -v; if (v > pk) pk = v; }
            else for (uint32_t k = 0; k + 1 < m; k += 2) { int v = (int16_t)gmem_rd16(data + k); if (v < 0) v = -v; if (v > pk) pk = v; }
            static int prevpk; int onset_ = pk > 100 && prevpk <= 100; prevpk = pk;
            if (x->bits == 16) {
                static int seen_loud; uint32_t fi = 0xffffffffu;
                for (uint32_t k = 0; k + 1 < len; k += 2) { int v = (int16_t)gmem_rd16(data + k); if (v < 0) v = -v; if (v > 150) { fi = k / 2u; break; } }
                if (fi != 0xffffffffu && !seen_loud) { seen_loud = 1; pal_log("aud: FIRST LOUD sample %u in this buffer (stream-time of buffer start %lu ms, rate %d, ch %d)", (unsigned)fi, (unsigned long)(x->played_frames * 1000u / (uint32_t)(x->rate ? x->rate : 1)), x->rate, x->channels); }
                if (x->scur < 0 && x->first) seen_loud = 0;
            }
            if (x->bits == 16 && (cnt < 12 || onset_)) {
                char eb[200]; int o = 0; uint32_t smp = m / 2;
                for (uint32_t sg = 0; sg < 24 && sg * 128u < smp; sg++) { int e = 0; for (uint32_t k = 0; k < 128u && sg * 128u + k < smp; k++) { int v = (int16_t)gmem_rd16(data + (sg * 128u + k) * 2u); if (v < 0) v = -v; if (v > e) e = v; } o += snprintf(eb + o, sizeof eb - (size_t)o, "%d ", e); }
                pal_log("aud: envelope/128 smp: %s", eb);
            }
            cnt++;
            pal_log("aud: stream_load voice %d buf %d addr %08lx len %lu first-peak %d bits %d ch %d rate %d (scur %d, playing %d)", v, n, (unsigned long)data, (unsigned long)len, pk, x->bits, x->channels, x->rate, x->scur, x->status == VS_PLAYING);
        } }
    if (x->scur < 0) {
        stream_switch(x); x->fade = STREAM_FADE;
        if (x->first && x->scur >= 0) {
            /* Everything rendered ends up ~lead (180-250 ms) in the DSP queue before it is heard, while the picture is on screen at once:
             * start the soundtrack that much into the stream so sound and picture meet (the intro's music has to hit with the title). */
            uint64_t sk = (uint64_t)x->rate * STREAM_AV_SKIP_MS / 1000u;
            if (sk < x->frames) x->pos = sk << 32;
        }
        if (x->first) x->t_start_us = pal_time_us();
        x->first = 0;
    }
    if (x->status == VS_DONE) { x->status = VS_PLAYING; x->played = 0; }
}

static int mixer_stream_ready_u(int v)
{
    voice *x = V(v);
    service_u();
    if (!x || !x->stream) return -1;
    for (int k = 0; k < 2; k++) {
        int n = x->scur < 0 ? k : (k == 0 ? x->scur ^ 1 : x->scur);   /* the idle buffer first */
        if (!x->sfull[n]) return n;
    }
    return -1;
}

static void mixer_stream_info_u(int v, uint32_t pos[2], uint32_t len[2])
{
    voice *x = V(v);
    for (int n = 0; n < 2; n++) {
        pos[n] = len[n] = 0;
        if (!x) continue;
        len[n] = x->slen[n];
        pos[n] = (n == x->scur) ? (uint32_t)(x->pos >> 32) * (uint32_t)(x->channels * x->bits / 8) : x->spos[n];
    }
}

static void mixer_set_rate_u(int v, int rate)
{
    voice *x = V(v);
    if (x && rate > 0) { x->rate = rate; x->step = ((uint64_t)rate << 32) / MIX_RATE; }
}

static uint32_t mixer_get_pos_u(int v) { voice *x = V(v); service_u(); return x ? (uint32_t)(x->pos >> 32) : 0; }
static void mixer_set_pos_u(int v, uint32_t frame) { voice *x = V(v); if (x) x->pos = (uint64_t)(x->frames ? frame % x->frames : 0) << 32; }
static void mixer_play_u(int v, int looping)
{
    voice *x = V(v);
    if (!x || !x->data) return;
    x->loops = looping ? 0 : 1;
    x->played = 0;
    x->status = VS_PLAYING;
}

static uint32_t mixer_stream_eob_u(int v) { voice *x = V(v); return x ? x->eob : 0; }

static void mixer_start_u(int v)  { voice *x = V(v); if (x && x->stream) { x->status = VS_PLAYING; return; } if (x && x->data) { x->pos = 0; x->played = 0; x->status = VS_PLAYING; } }
static void mixer_stop_u(int v)   { voice *x = V(v); if (x && x->status == VS_PLAYING) x->status = VS_STOPPED; }
static void mixer_resume_u(int v) { voice *x = V(v); if (x && x->status == VS_STOPPED) x->status = VS_PLAYING; }
static void mixer_end_u(int v)    { voice *x = V(v); if (x) x->status = VS_DONE; }
static int mixer_status_u(int v) { voice *x = V(v); service_u(); return x ? x->status : VS_FREE; }
static void mixer_set_volume_u(int v, float vol) { voice *x = V(v); if (x) x->vol = vol < 0 ? 0 : vol > 1 ? 1 : vol; }
static void mixer_set_pan_u(int v, float pan)    { voice *x = V(v); if (x) x->pan = pan < -1 ? -1 : pan > 1 ? 1 : pan; }
static void mixer_set_loops_u(int v, int loops)  { voice *x = V(v); if (x) x->loops = loops < 0 ? 1 : loops; }
static void mixer_set_master_u(float vol) { g_master = vol < 0 ? 0 : vol > 1 ? 1 : vol; }
static float mixer_master_u(void) { return g_master; }

/* ---- IMA ADPCM (Microsoft layout: per-channel 4-byte header, then 4-byte interleave) */
static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t ima_idx[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static int16_t ima_nibble(int *pred, int *idx, int nib)
{
    int step = ima_step[*idx], diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    *pred += (nib & 8) ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += ima_idx[nib];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}

static void ima_decode_block(voice *x, int32_t b)
{
    gptr p = x->data + (uint32_t)b * (uint32_t)x->block_align;
    int ch = x->channels, pred[2], idx[2];
    for (int c = 0; c < ch; c++) {
        pred[c] = (int16_t)gmem_rd16(p + 4u * (uint32_t)c);
        idx[c] = gmem_rd8(p + 4u * (uint32_t)c + 2);
        if (idx[c] > 88) idx[c] = 88;
        x->blk[c] = (int16_t)pred[c];
    }
    uint32_t n = 1;
    gptr q = p + 4u * (uint32_t)ch;
    while (n < x->blk_frames && n + 7 < 2048) {
        for (int c = 0; c < ch; c++) {
            for (int k = 0; k < 4; k++) {
                uint8_t byte = gmem_rd8(q++);
                x->blk[(n + 2 * (uint32_t)k) * (uint32_t)ch + (uint32_t)c] = ima_nibble(&pred[c], &idx[c], byte & 15);
                x->blk[(n + 2 * (uint32_t)k + 1) * (uint32_t)ch + (uint32_t)c] = ima_nibble(&pred[c], &idx[c], byte >> 4);
            }
        }
        n += 8;
    }
    x->blk_index = b;
    x->blk_first = (uint32_t)b * x->blk_frames;
}

static void frame_at(voice *x, uint32_t f, int32_t *l, int32_t *r)
{
    if (x->fmt == 1) {
        if (x->bits == 16) {
            gptr a = x->data + f * 2u * (uint32_t)x->channels;
            *l = (int16_t)gmem_rd16(a);
            *r = x->channels == 2 ? (int16_t)gmem_rd16(a + 2) : *l;
        } else {
            gptr a = x->data + f * (uint32_t)x->channels;
            *l = ((int32_t)gmem_rd8(a) - 128) << 8;
            *r = x->channels == 2 ? ((int32_t)gmem_rd8(a + 1) - 128) << 8 : *l;
        }
    } else {
        if (x->blk_index < 0 || f < x->blk_first || f - x->blk_first >= x->blk_frames)
            ima_decode_block(x, (int32_t)(f / x->blk_frames));
        uint32_t k = f - x->blk_first;
        *l = x->blk[k * (uint32_t)x->channels];
        *r = x->channels == 2 ? x->blk[k * 2u + 1u] : *l;
    }
}

/* How long a movie frame that is due now has to be held back so that it appears when its soundtrack position is heard. The picture
 * runs on the wall clock, the sound is rendered ahead of it (DSP queue) and, for slow-decoding movies, plays a little slower than
 * real time (smul); the frame's time stamp is the time since the stream started. 0 = no movie stream playing. */
uint64_t mixer_movie_hold_us(void)
{
    uint64_t now = pal_time_us();
    for (int i = 0; i < MIX_VOICES; i++) {
        voice *x = &g_v[i];
        if (!x->used || !x->stream || x->status != VS_PLAYING || !x->rate || !x->t_start_us) continue;
        int64_t ahead = g_last_us > now ? (int64_t)(g_last_us - now) : 0;
        int64_t heard = (int64_t)((x->played_frames + (x->pos >> 32)) * 1000000u / (uint32_t)x->rate) - ahead;   /* content time audible now */
        int64_t due = (int64_t)(now - x->t_start_us);                                                            /* content time of the frame */
        if (due < 2000000) return 0;     /* not at the very start: holding the first frames back starves Bink's own audio decode (nothing buffered yet) */
        float sp = x->smul > 0.5f ? x->smul : 1.0f;
        int64_t h = (int64_t)((float)(due - heard) / sp);
        if (h < 0) h = 0;
        if (h > 1200000) h = 1200000;
        return (uint64_t)h;
    }
    return 0;
}
/* Pull the soundtrack of the playing movie forward by up to `us` - through silence only (it stops before the first audible
 * sample): the picture does not wait for the sound, the sound catches up with the picture while nothing is audible. */
void mixer_movie_skip_ahead(uint64_t us)
{
    pal_audio_lock();
    for (int i = 0; i < MIX_VOICES; i++) {
        voice *x = &g_v[i];
        if (!x->used || !x->stream || x->status != VS_PLAYING || !x->rate) continue;
        if (us > 1500000u) us = 1500000u;
        x->skip_pending = (uint32_t)(us * (uint64_t)x->rate / 1000000u);
        break;
    }
    pal_audio_unlock();
}

/* debug: stream time (ms) the mixer has rendered up to for the movie stream, -1 = none */
long mixer_dbg_stream_ms(void)
{
    for (int i = 0; i < MIX_VOICES; i++) { voice *x = &g_v[i]; if (x->used && x->stream && x->status == VS_PLAYING && x->rate) return (long)((x->played_frames + (x->pos >> 32)) * 1000u / (uint32_t)x->rate); }
    return -1;
}
void mixer_render(int16_t *out, uint32_t frames)
{
    static int32_t acc[2 * 1024];
    extern int hle_movie_open(void) __attribute__((weak));
    int mv_open = hle_movie_open && hle_movie_open() > 0;
    if (!mv_open) for (int i = 0; i < MIX_VOICES; i++) g_v[i].smul = 0.0f;
    while (frames) {
        uint32_t n = frames > 1024 ? 1024 : frames;
        memset(acc, 0, sizeof(int32_t) * 2 * n);
        for (int i = 0; i < MIX_VOICES; i++) {
            voice *x = &g_v[i];
            if (!x->used || x->status != VS_PLAYING || (!x->frames && !x->stream)) continue;
            if (x->stream && mv_open) {
                /* A movie that really runs at full speed (the intro: display gaps on the 15 fps grid, measured over >= 8 frames) gets its soundtrack
                 * at full speed too: the fixed 93 % start detuned its music and let it fall ~0.4 s behind the picture after 6 s. A stream that
                 * has run dry once, or a slower movie (the logo on the console decodes at ~85 %), keeps the starve-driven speed. */
                extern uint32_t video_movie_speed_permille(void) __attribute__((weak));
                /* not in the first 2.5 s: a short movie such as the monkey runs its first buffers dry at full speed (hang), and the intro's first seconds are silent */
                uint32_t pm = (video_movie_speed_permille && !x->ever_starved && x->t_start_us && pal_time_us() - x->t_start_us > 2500000u) ? video_movie_speed_permille() : 0;
                if (pm >= 980u) x->smul = x->smul == 0.0f ? 1.0f : x->smul + (1.0f - x->smul) * 0.2f;
                else if (x->smul == 0.0f) x->smul = 0.93f;       /* Bink on the 3DS runs at ~93-96 % of real time */
            }
            if (x->skip_pending && x->stream && x->scur >= 0) {
                uint32_t f0 = (uint32_t)(x->pos >> 32), lim = x->frames > f0 ? x->frames - f0 : 0, n_ = x->skip_pending < lim ? x->skip_pending : lim, j = 0;
                for (; j < n_; j++) { int32_t l_, r_; frame_at(x, f0 + j, &l_, &r_); if (l_ > 400 || l_ < -400 || r_ > 400 || r_ < -400) { x->skip_pending = 0; break; } }
                if (j > 400u) j -= 200u;                 /* stay clear of the first audible sample (fade-in of the music) */
                x->pos += (uint64_t)j << 32;
                if (x->skip_pending) x->skip_pending -= x->skip_pending < j ? x->skip_pending : j;
                pal_log("mixer: movie sound skipped %u ms of silence, %u ms still pending", (unsigned)(j * 1000u / (uint32_t)x->rate), (unsigned)(x->skip_pending * 1000u / (uint32_t)x->rate));
            }
            float gl = x->vol * g_master * (x->pan > 0 ? 1.0f - x->pan : 1.0f);
            float gr = x->vol * g_master * (x->pan < 0 ? 1.0f + x->pan : 1.0f);
            int32_t il = (int32_t)(gl * 256), ir = (int32_t)(gr * 256);
            int vpk = 0;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t f = (uint32_t)(x->pos >> 32);
                if (x->stream && (x->scur < 0 || f >= x->frames)) {
                    if (x->scur >= 0) {
                        x->played_frames += x->frames;
                        x->spos[x->scur] = x->slen[x->scur];
                        x->sfull[x->scur] = 0;
                        x->eob++;
                        stream_switch(x);
                        /* Bink decodes slower than real time and produces its soundtrack at that pace: play the
                         * stream a little slower instead of cutting out (video runs at the same slower pace,
                         * so lip-sync holds). Speeds back up while the buffers keep arriving in time. */
                        if (x->scur >= 0 && mv_open && ++x->sok >= 3) {
                            x->sok = 0;
                            if (x->smul == 0.0f) x->smul = 1.0f;
                            x->smul += 0.003f; if (x->smul > 1.0f) x->smul = 1.0f;
                        }
                    }
                    if (x->scur < 0) {                       /* starved: silence until loaded */
                        if (!x->starved && mv_open) {
                            if (x->smul == 0.0f) x->smul = 1.0f;
                            x->smul *= 0.97f; if (x->smul < 0.5f) x->smul = 0.5f;
                            x->sok = 0;
                        }
                        if (!x->starved) { x->starved = 1; x->ever_starved = 1; g_starves++; if (g_starves <= 60) pal_log("mixer: stream starved (voice %d, buffers loaded %d/%d, speed %d%%, movie %d)", i, x->sfull[0], x->sfull[1], (int)(x->smul * 100), mv_open); }
                        break;
                    }
                    x->starved = 0;
                    f = 0;
                } else if (f >= x->frames) {
                    x->played++;
                    if (x->loops == 0 || x->played < x->loops) { x->pos -= (uint64_t)x->frames << 32; f = (uint32_t)(x->pos >> 32); }
                    else { x->status = VS_DONE; break; }
                }
                int32_t l, r;
                frame_at(x, f, &l, &r);
                if (x->step != 0x100000000ull || (x->stream && x->smul != 0.0f && x->smul != 1.0f)) {
                    /* resampling (11 kHz movie sound -> 22 kHz, or the stream speed): linear interpolation instead of repeating the nearest
                     * sample - the stair steps were the harsh, "tinny" sound of the movie music */
                    uint32_t fr = (uint32_t)(x->pos & 0xffffffffu) >> 16;
                    if (f + 1u < x->frames && fr) {
                        int32_t l2, r2;
                        frame_at(x, f + 1u, &l2, &r2);
                        l += (int32_t)(((int64_t)(l2 - l) * (int32_t)fr) >> 16);
                        r += (int32_t)(((int64_t)(r2 - r) * (int32_t)fr) >> 16);
                    }
                }
                if (x->fade) { int32_t g = (int32_t)(STREAM_FADE - x->fade); l = l * g / (int32_t)STREAM_FADE; r = r * g / (int32_t)STREAM_FADE; x->fade--; }
                acc[2 * k] += (l * il) >> 8;
                acc[2 * k + 1] += (r * ir) >> 8;
                { extern int g_aud_dbg_on; if (g_aud_dbg_on) { int pl = ((l * il) >> 8); if (pl < 0) pl = -pl; if (pl > vpk) vpk = pl; } }
                x->pos += (x->stream && x->smul != 0.0f && x->smul != 1.0f && mv_open) ? (uint64_t)((double)x->step * x->smul) : x->step;
            }
            {   extern int g_aud_dbg_on; static int cnt; static int loud[MIX_VOICES];
                int onset = vpk > 6000 && !loud[i]; loud[i] = vpk > 200;
                if (g_aud_dbg_on && onset && cnt < 120) { cnt++; pal_log("aud: voice %d peak %d stream-time %lu ms stream %d status %d scur %d pos %lu/%lu vol %.2f smul %.2f bits %d ch %d", i, vpk, (unsigned long)((x->played_frames + (x->pos >> 32)) * 1000u / (uint32_t)(x->rate ? x->rate : 1)), x->stream, x->status, x->scur, (unsigned long)(x->pos >> 32), (unsigned long)x->frames, x->vol, x->smul, x->bits, x->channels); }
            }
        }
        for (uint32_t k = 0; k < 2 * n; k++) {
            /* Soft limiter: music plus blaster fire regularly sums past 16 bits (~1000 samples per 5 s in a fight on the log), and a hard
             * clip is a click. Above 3/4 of full scale the excess is compressed (e*R/(e+R): slope 1 at the knee, asymptote at full scale). */
            int32_t v = acc[k];
            if (v > 24576 || v < -24576) {
                int32_t e = (v < 0 ? -v : v) - 24576, y = 24576 + (int32_t)(((int64_t)e * 8191) / (e + 8191));
                if (e > 8191) g_clipped++;
                v = v < 0 ? -y : y;
            }
            out[k] = (int16_t)v;
        }
        out += 2 * n;
        frames -= n;
    }
}

/* Render ahead of the wall clock by a lead: the guest thread only gets here when the game calls into the sound
 * layer, and a Bink movie decode/present leaves 90-150 ms between calls (every such pause = a hole = stutter).
 * With the mixer thread the lead only has to cover scheduling jitter. g_last_us = wall-clock time rendered up to. */
static void service_lead_u(uint64_t io_lead_us)
{
    uint64_t now = pal_time_us();
    {   /* movie audio statistics: one line per movie (gaps between service calls decide how far ahead the mixer has to render) */
        extern int hle_movie_open(void) __attribute__((weak));
        static struct { int on; uint64_t prev, maxgap; uint32_t n, g100, g150, g200, und0, starve0; } mv;
        int open = hle_movie_open && hle_movie_open() > 0;
        if (open && !mv.on) { memset(&mv, 0, sizeof mv); mv.on = 1; mv.und0 = g_underruns; mv.starve0 = g_starves; g_movie_since_us = now; }
        if (!open) g_movie_since_us = 0;
        if (open) {
            if (mv.prev) { uint64_t gp = now - mv.prev; if (gp > mv.maxgap) mv.maxgap = gp; if (gp > 100000u) mv.g100++; if (gp > 150000u) mv.g150++; if (gp > 200000u) mv.g200++; }
            mv.prev = now; mv.n++;
        } else if (mv.on) {
            mv.on = 0;
            pal_log("mixer: movie audio: %u service calls, max gap %u ms, gaps >100/150/200 ms: %u/%u/%u, underruns %u, starves %u", (unsigned)mv.n,
                    (unsigned)(mv.maxgap / 1000u), (unsigned)mv.g100, (unsigned)mv.g150, (unsigned)mv.g200, (unsigned)(g_underruns - mv.und0), (unsigned)(g_starves - mv.starve0));
        }
    }
    /* The guest can be away from the sound layer for a long stretch (the last part of a level load: pure computation, no
     * API calls). Watch the gaps between calls: after a long one keep the DSP fed further ahead for a while (the ring holds
     * ~900 ms), so the next such stretch does not run it dry - that dry spell is the crackle. Decays after 8 s. */
    static uint64_t s_prev, s_gap, s_gap_t;
    if (s_prev && now - s_prev > 150000u) { s_gap = now - s_prev; s_gap_t = now; }
    s_prev = now;
    uint64_t gap_lead = (s_gap_t && now - s_gap_t < 8000000u) ? s_gap * 13u / 10u : 0u;
    if (gap_lead > 700000u) gap_lead = 700000u;
    if (gap_lead > io_lead_us) io_lead_us = gap_lead;
    if (!g_clock_started) { g_last_us = now; g_clock_started = 1; return; }
    if (g_last_us < now) {                       /* the DSP ran dry (or the first call): restart from "now" */
        if (g_last_us + 20000u < now) { g_underruns++; if (!g_threaded && g_underruns <= 40) pal_log("mixer: underrun %u ms", (unsigned)((now - g_last_us) / 1000u)); }
        g_last_us = now;
    }
    /* movies: the guest is away for 90-250 ms per decoded frame -> render far ahead; gameplay: short lead (latency) */
    extern int hle_movie_open(void) __attribute__((weak));
    uint64_t target = now + (g_threaded ? 70000u : (hle_movie_open && hle_movie_open() > 0) ? (g_movie_since_us && now - g_movie_since_us < 5000000u ? 250000u : 180000u) : 100000u);
    if (io_lead_us && target < now + io_lead_us) target = now + io_lead_us;
    if (g_last_us >= target) return;             /* already further ahead than the (now shorter) lead */
    uint64_t due = (target - g_last_us) * MIX_RATE / 1000000u;
    if (due < 256) return;
    if (due > MIX_RATE / 2) due = MIX_RATE / 2;
    g_last_us += due * 1000000u / MIX_RATE;
    static int16_t buf[2 * (MIX_RATE / 2 + 1)];
    {   /* cost of the software mix on the calling thread (a hitch source when the guest thread mixes) */
        static uint64_t acc, since, calls;
        uint64_t t0 = pal_time_us();
        mixer_render(buf, (uint32_t)due);
        pal_audio_submit(buf, (uint32_t)due);
        acc += pal_time_us() - t0; calls++;
        if (!since) since = t0;
        if (t0 - since >= 5000000u) {
            pal_log("mixer: %llu ms of %llu ms spent mixing (%llu calls), clipped samples %u", (unsigned long long)(acc / 1000u), (unsigned long long)((t0 - since) / 1000u), (unsigned long long)calls, (unsigned)g_clipped);
            g_clipped = 0;
            acc = 0; calls = 0; since = t0;
        }
    }
}

static void service_u(void) { service_lead_u(0); }
/* File IO (level loading creates hundreds of cache files, 80 ms each, with the game away from the sound layer for
 * half a second at a time): called from the IO calls, it renders 250 ms ahead so the music rides over those gaps. */
void mixer_service_io(void)
{
    if (g_threaded) return;
    pal_audio_lock(); service_lead_u(250000u); pal_audio_unlock();
}
/* guest-thread entry: a no-op while the mixer thread does the work */
void mixer_service(void)
{
    if (g_threaded) return;
    pal_audio_lock(); service_u(); pal_audio_unlock();
}
/* mixer-thread entry (pal_3ds.c) */
void mixer_thread_tick(void) { pal_audio_lock(); service_u(); pal_audio_unlock(); }
void mixer_set_threaded(int on) { g_threaded = on; }
uint32_t mixer_underruns(void) { return g_underruns; }

/* ---- public API: lock around the unlocked internals ---- */
int mixer_alloc(void) { pal_audio_lock(); int r_ = mixer_alloc_u(); pal_audio_unlock(); return r_; }
void mixer_release(int v) { pal_audio_lock(); mixer_release_u(v); pal_audio_unlock(); }
int mixer_set_data(int v, gptr data, uint32_t len, int fmt, int channels, int bits, int rate, int block_align) { pal_audio_lock(); int r_ = mixer_set_data_u(v, data, len, fmt, channels, bits, rate, block_align); pal_audio_unlock(); return r_; }
int mixer_set_wav(int v, gptr w) { pal_audio_lock(); int r_ = mixer_set_wav_u(v, w); pal_audio_unlock(); return r_; }
int mixer_stream_begin(int v, int channels, int bits, int rate) { pal_audio_lock(); int r_ = mixer_stream_begin_u(v, channels, bits, rate); pal_audio_unlock(); return r_; }
void mixer_stream_load(int v, int n, gptr data, uint32_t len) { pal_audio_lock(); mixer_stream_load_u(v, n, data, len); pal_audio_unlock(); }
int mixer_stream_ready(int v) { pal_audio_lock(); int r_ = mixer_stream_ready_u(v); pal_audio_unlock(); if (g_threaded) pal_audio_relax(); return r_; }
void mixer_stream_info(int v, uint32_t pos[2], uint32_t len[2]) { pal_audio_lock(); mixer_stream_info_u(v, pos, len); pal_audio_unlock(); }
void mixer_set_rate(int v, int rate) { pal_audio_lock(); mixer_set_rate_u(v, rate); pal_audio_unlock(); }
uint32_t mixer_get_pos(int v) { pal_audio_lock(); uint32_t r_ = mixer_get_pos_u(v); pal_audio_unlock(); return r_; }
void mixer_set_pos(int v, uint32_t frame) { pal_audio_lock(); mixer_set_pos_u(v, frame); pal_audio_unlock(); }
void mixer_play(int v, int looping) { pal_audio_lock(); mixer_play_u(v, looping); pal_audio_unlock(); }
uint32_t mixer_stream_eob(int v) { pal_audio_lock(); uint32_t r_ = mixer_stream_eob_u(v); pal_audio_unlock(); return r_; }
void mixer_start(int v) { pal_audio_lock(); mixer_start_u(v); pal_audio_unlock(); }
void mixer_stop(int v) { pal_audio_lock(); mixer_stop_u(v); pal_audio_unlock(); }
void mixer_resume(int v) { pal_audio_lock(); mixer_resume_u(v); pal_audio_unlock(); }
void mixer_end(int v) { pal_audio_lock(); mixer_end_u(v); pal_audio_unlock(); }
int mixer_status(int v) { pal_audio_lock(); int r_ = mixer_status_u(v); pal_audio_unlock(); return r_; }
void mixer_set_volume(int v, float vol) { pal_audio_lock(); mixer_set_volume_u(v, vol); pal_audio_unlock(); }
void mixer_set_pan(int v, float pan) { pal_audio_lock(); mixer_set_pan_u(v, pan); pal_audio_unlock(); }
void mixer_set_loops(int v, int loops) { pal_audio_lock(); mixer_set_loops_u(v, loops); pal_audio_unlock(); }
void mixer_set_master(float vol) { pal_audio_lock(); mixer_set_master_u(vol); pal_audio_unlock(); }
float mixer_master(void) { pal_audio_lock(); float r_ = mixer_master_u(); pal_audio_unlock(); return r_; }
