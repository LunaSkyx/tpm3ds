/* Miles Sound System (MSS32.DLL) HLE on top of the software mixer.
 * HSAMPLE/H3DSAMPLE are small guest blocks (the real ones are structs, so a
 * stray dereference reads zeros instead of faulting); a table maps them to voices.
 * 3D: one listener, distance roll-off between min and max distance, panning
 * from the listener's right vector. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "core/audio/mixer.h"
#include "core/gheap.h"
#include "core/guest.h"
#include "core/hle/hle.h"
#include "core/hle/hle_dsound.h"
#include "pal/pal.h"

#define MAXS 64
typedef struct {
    gptr  h;
    int   voice, is3d;
    float pos[3], min_d, max_d, vol;
    int   channels, bits, rate;        /* streaming format (set_sample_type) */
    uint32_t user[8], eob_cb, eob_seen;
} msample;

static msample g_s[MAXS];
static int     g_trace;   /* TPM_TRACE_MSS: log sample files, starts and 3D gain (debug) */
static gptr    g_dig, g_provider_name, g_err;
static float   g_lpos[3], g_lface[3] = {0, 0, 1}, g_lup[3] = {0, 1, 0};
static gptr    g_listener;
#define PROVIDER 0x00000301u

static float farg(cpu_t *c, int n) { uint32_t u = ARG(n); float f; memcpy(&f, &u, 4); return f; }

static msample *find(gptr h)
{
    for (int i = 0; i < MAXS; i++) if (g_s[i].h && g_s[i].h == h) return &g_s[i];
    return NULL;
}

static gptr alloc_sample(int is3d)
{
    for (int i = 0; i < MAXS; i++)
        if (!g_s[i].h) {
            int v = mixer_alloc();
            if (v < 0) return 0;
            gptr h = gheap_alloc(0x100);
            if (!h) { mixer_release(v); return 0; }
            memset(&g_s[i], 0, sizeof g_s[i]);
            g_s[i].h = h; g_s[i].voice = v; g_s[i].is3d = is3d;
            g_s[i].min_d = 1.0f; g_s[i].max_d = 100.0f; g_s[i].vol = 1.0f;
            g_s[i].channels = 1; g_s[i].bits = 16; g_s[i].rate = 22050;
            return h;
        }
    pal_log("mss: sample table full");
    return 0;
}

static void release_sample(gptr h)
{
    msample *s = find(h);
    if (!s) return;
    mixer_release(s->voice);
    gheap_free(s->h);
    s->h = 0;
}

static void update3d(msample *s)
{
    float d[3] = {s->pos[0] - g_lpos[0], s->pos[1] - g_lpos[1], s->pos[2] - g_lpos[2]};
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float g = 1.0f;
    if (dist > s->max_d) g = 0.0f;
    else if (dist > s->min_d && dist > 0) g = s->min_d / dist;
    /* right = up x face (left-handed, as Direct3D/Miles) */
    float r[3] = {g_lup[1] * g_lface[2] - g_lup[2] * g_lface[1], g_lup[2] * g_lface[0] - g_lup[0] * g_lface[2],
                  g_lup[0] * g_lface[1] - g_lup[1] * g_lface[0]};
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    float pan = (dist > 0 && rl > 0) ? (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (dist * rl) : 0.0f;
    mixer_set_volume(s->voice, s->vol * g);
    mixer_set_pan(s->voice, pan * 0.8f);
}

static void update_all3d(void)
{
    for (int i = 0; i < MAXS; i++) if (g_s[i].h && g_s[i].is3d) update3d(&g_s[i]);
}

/* ---- driver ---- */
static void m_quick_startup(cpu_t *c)
{
    pal_log("mss: quick_startup digital=%lu midi=%lu %lu Hz %lu bit %lu ch", (unsigned long)ARG(0),
            (unsigned long)ARG(1), (unsigned long)ARG(2), (unsigned long)ARG(3), (unsigned long)ARG(4));
    if (!g_dig) g_dig = gheap_alloc(0x100);
    hle_ret(c, 1, 5);
}
static void m_quick_handles(cpu_t *c)
{
    if (ARG(0)) gmem_wr32(ARG(0), ARG(0) ? g_dig : 0);
    if (ARG(1)) gmem_wr32(ARG(1), 0);
    if (ARG(2)) gmem_wr32(ARG(2), 0);
    hle_ret(c, 0, 3);
}
static void m_quick_shutdown(cpu_t *c)
{
    for (int i = 0; i < MAXS; i++) if (g_s[i].h) release_sample(g_s[i].h);
    hle_ret(c, 0, 0);
}
static void m_set_preference(cpu_t *c) { hle_ret(c, 0, 2); }
/* AIL_lock/unlock are called far more often than the mixer needs servicing (the clock read alone
 * costs more than the check): service every 8th call; position/status queries still do it each time. */
static void m_lock(cpu_t *c) { static unsigned n; if ((++n & 7u) == 0) mixer_service(); hle_ret(c, 0, 0); }
static void m_last_error(cpu_t *c) { hle_ret(c, g_err, 0); }
static void m_digital_master_volume(cpu_t *c) { hle_ret(c, (uint32_t)(mixer_master() * 127.0f + 0.5f), 1); }
static void m_set_digital_master_volume(cpu_t *c) { mixer_set_master((float)(int32_t)ARG(1) / 127.0f); hle_ret(c, 0, 2); }
/* get_DirectSound_info(sample, *lpDS, *lpDSB): the DirectSound object Miles mixes into
 * (iMUSE streams music through it). */
static void m_get_DirectSound_info(cpu_t *c)
{
    if (ARG(1)) gmem_wr32(ARG(1), hle_dsound_object());
    if (ARG(2)) gmem_wr32(ARG(2), 0);
    hle_ret(c, 0, 3);
}

/* ---- 2D samples ---- */
extern int hle_movie_open(void);
/* EXPERIMENT: while a movie is open, Bink asks for its audio stream here; refusing makes it play without a
 * soundtrack, which skips the audio gate that froze the picture (docs/PLAN.md sections 32-37). */
static void m_allocate_sample_handle(cpu_t *c) { hle_ret(c, alloc_sample(0), 1); }
static void m_release_sample_handle(cpu_t *c) { release_sample(ARG(0)); hle_ret(c, 0, 1); }
static void m_init_sample(cpu_t *c)
{
    msample *s = find(ARG(0));
    if (s) { mixer_end(s->voice); mixer_set_volume(s->voice, 1.0f); mixer_set_pan(s->voice, 0); mixer_set_loops(s->voice, 1); s->vol = 1; }
    hle_ret(c, 0, 1);
}
static int set_file(gptr h, gptr img)
{
    msample *s = find(h);
    int ok = s && mixer_set_wav(s->voice, img);
    /* Loading a file starts a fresh sound: loop count back to "play once". The game sets the count (0 = forever) only for
     * looping sounds, right before AIL_start_*; a 3D handle is reused for the next sound without AIL_init_sample, so
     * without this a looped ambience leaves the slot looping and the next speech line repeats until the handle is freed. */
    if (ok) mixer_set_loops(s->voice, 1);
    if (!ok) pal_log("mss: set_sample_file(%08lx, %08lx) failed (not a supported WAV)", (unsigned long)h, (unsigned long)img);
    else if (g_trace) pal_log("mss: set_file %08lx 3d=%d riff=%lu", (unsigned long)h, s->is3d, (unsigned long)gmem_rd32(img + 4));
    return ok;
}
static void m_set_sample_file(cpu_t *c) { hle_ret(c, (uint32_t)set_file(ARG(0), ARG(1)), 3); }
#define VOICE_OP(name, op, nargs) \
    static void name(cpu_t *c) { msample *s = find(ARG(0)); if (s) op(s->voice); hle_ret(c, 0, nargs); }
static void m_start_sample(cpu_t *c)
{
    msample *s = find(ARG(0));
    if (s) {
        mixer_start(s->voice);
        if (g_trace) {
            float d[3] = {s->pos[0] - g_lpos[0], s->pos[1] - g_lpos[1], s->pos[2] - g_lpos[2]};
            pal_log("mss: start %08lx 3d=%d vol=%.2f dist=%.1f min=%.1f max=%.1f", (unsigned long)s->h, s->is3d, s->vol,
                    sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), s->min_d, s->max_d);
        }
    }
    hle_ret(c, 0, 1);
}
VOICE_OP(m_stop_sample, mixer_stop, 1)
VOICE_OP(m_resume_sample, mixer_resume, 1)
VOICE_OP(m_end_sample, mixer_end, 1)
static void m_sample_status(cpu_t *c) { msample *s = find(ARG(0)); hle_ret(c, s ? (uint32_t)mixer_status(s->voice) : VS_FREE, 1); }
static void m_set_sample_volume(cpu_t *c)
{
    msample *s = find(ARG(0));
    if (s) { s->vol = (float)(int32_t)ARG(1) / 127.0f; s->is3d ? update3d(s) : mixer_set_volume(s->voice, s->vol); }
    hle_ret(c, 0, 2);
}
static void m_set_sample_loop_count(cpu_t *c) { msample *s = find(ARG(0)); if (s) mixer_set_loops(s->voice, (int)ARG(1)); hle_ret(c, 0, 2); }

/* ---- streaming (double buffering; used by the lifted BINKW32.DLL) ---- */
static int fmt_channels(uint32_t f) { return (f & 2u) ? 2 : 1; }      /* DIG_F_MONO_8 0, MONO_16 1, STEREO_8 2, STEREO_16 3 */
static int fmt_bits(uint32_t f) { return (f & 1u) ? 16 : 8; }

/* End-of-buffer callbacks fire on the guest thread, at the next Miles call. */
static void pump_eob(cpu_t *c)
{
    for (int i = 0; i < MAXS; i++) {
        msample *s = &g_s[i];
        if (!s->h || !s->eob_cb) continue;
        uint32_t n = mixer_stream_eob(s->voice);
        while (s->eob_seen != n) {
            s->eob_seen++;
            uint32_t a[1] = {s->h};
            guest_callback(c, s->eob_cb, 1, a);
        }
    }
}

static void m_minimum_sample_buffer_size(cpu_t *c)
{
    uint32_t rate = ARG(1), f = ARG(2), bpf = (uint32_t)(fmt_channels(f) * fmt_bits(f) / 8);
    uint32_t n = (rate * bpf / 2 + 4095u) & ~4095u;                  /* ~500 ms per buffer: a Bink frame decodes in 90-250 ms on the 3DS and refills the stream only then (was 125 ms: the stream starved every ~250 ms = stutter) */
    hle_ret(c, n ? n : 4096u, 3);
}
static void m_set_sample_type(cpu_t *c)
{
    msample *s = find(ARG(0));
    if (s) {
        s->channels = fmt_channels(ARG(1));
        s->bits = fmt_bits(ARG(1));
        if (s->bits == 8 && (ARG(2) & 1u)) pal_log("mss: signed 8-bit samples not supported");
        mixer_stream_begin(s->voice, s->channels, s->bits, s->rate);
        s->eob_seen = mixer_stream_eob(s->voice);
    }
    hle_ret(c, 0, 3);
}
static void m_set_sample_playback_rate(cpu_t *c)
{
    msample *s = find(ARG(0));
    if (s) { s->rate = (int)ARG(1); mixer_set_rate(s->voice, s->rate); }
    hle_ret(c, 0, 2);
}
static void m_set_sample_pan(cpu_t *c)
{
    msample *s = find(ARG(0));
    if (s) mixer_set_pan(s->voice, ((float)(int32_t)ARG(1) - 64.0f) / 63.0f);
    hle_ret(c, 0, 2);
}
static void m_set_sample_user_data(cpu_t *c)
{
    msample *s = find(ARG(0));
    if (s && ARG(1) < 8) s->user[ARG(1)] = ARG(2);
    hle_ret(c, 0, 3);
}
static void m_sample_user_data(cpu_t *c)
{
    msample *s = find(ARG(0));
    hle_ret(c, s && ARG(1) < 8 ? s->user[ARG(1)] : 0, 2);
}
static void m_register_EOB_callback(cpu_t *c)
{
    msample *s = find(ARG(0));
    uint32_t old = s ? s->eob_cb : 0;
    if (s) { s->eob_cb = ARG(1); s->eob_seen = mixer_stream_eob(s->voice); }
    hle_ret(c, old, 2);
}
static void m_serve(cpu_t *c) { mixer_service(); pump_eob(c); hle_ret(c, 0, 0); }
static void m_sample_buffer_ready(cpu_t *c)
{
    msample *s = find(ARG(0));
    pump_eob(c);
    hle_ret(c, s ? (uint32_t)mixer_stream_ready(s->voice) : 0xffffffffu, 1);
}
/* load_sample_buffer(S, buff_num, buffer, len) */
static void m_load_sample_buffer(cpu_t *c)
{
    msample *s = find(ARG(0));
    if (s) mixer_stream_load(s->voice, (int)ARG(1), ARG(2), ARG(3));
    hle_ret(c, 0, 4);
}
/* sample_buffer_info(S, *pos0, *len0, *pos1, *len1) */
static void m_sample_buffer_info(cpu_t *c)
{
    msample *s = find(ARG(0));
    uint32_t pos[2] = {0, 0}, len[2] = {0, 0};
    if (s) mixer_stream_info(s->voice, pos, len);
    if (ARG(1)) gmem_wr32(ARG(1), pos[0]);
    if (ARG(2)) gmem_wr32(ARG(2), len[0]);
    if (ARG(3)) gmem_wr32(ARG(3), pos[1]);
    if (ARG(4)) gmem_wr32(ARG(4), len[1]);
    hle_ret(c, 1, 5);   /* nonzero = "stream info valid": Bink (0x8f28b0) uses it to mark the audio stream as running */
}
static void m_mem_alloc_lock(cpu_t *c) { hle_ret(c, gheap_alloc(ARG(0)), 1); }
static void m_mem_free_lock(cpu_t *c) { gheap_free(ARG(0)); hle_ret(c, 0, 1); }
static void m_get_preference(cpu_t *c)
{
    pal_log("mss: get_preference(%lu) -> 0", (unsigned long)ARG(0));
    hle_ret(c, 0, 1);
}

/* ---- 3D ---- */
static void m_enumerate_3D_providers(cpu_t *c)
{
    gptr next = ARG(0);
    if (gmem_rd32(next) != 0) { hle_ret(c, 0, 3); return; }   /* exactly one provider */
    gmem_wr32(next, 1);
    gmem_wr32(ARG(1), PROVIDER);
    gmem_wr32(ARG(2), g_provider_name);
    hle_ret(c, 1, 3);
}
static void m_open_3D_provider(cpu_t *c) { hle_ret(c, ARG(0) == PROVIDER ? 0u : 1u, 1); }
static void m_close_3D_provider(cpu_t *c) { hle_ret(c, 0, 1); }
static void m_set_3D_provider_preference(cpu_t *c) { hle_ret(c, 0, 3); }
static void m_allocate_3D_sample_handle(cpu_t *c) { hle_ret(c, alloc_sample(1), 1); }
static void m_set_3D_sample_file(cpu_t *c)
{
    int ok = set_file(ARG(0), ARG(1));
    msample *s = find(ARG(0));
    if (ok && s) update3d(s);
    hle_ret(c, (uint32_t)ok, 2);
}
static void m_set_3D_sample_float_distances(cpu_t *c)
{
    msample *s = find(ARG(0));
    if (s) { s->max_d = farg(c, 1); s->min_d = farg(c, 2); update3d(s); }
    hle_ret(c, 0, 5);
}
static void m_set_3D_position(cpu_t *c)
{
    float p[3] = {farg(c, 1), farg(c, 2), farg(c, 3)};
    if (ARG(0) == g_listener) { memcpy(g_lpos, p, sizeof p); update_all3d(); }
    else { msample *s = find(ARG(0)); if (s) { memcpy(s->pos, p, sizeof p); update3d(s); } }
    hle_ret(c, 0, 4);
}
static void m_set_3D_orientation(cpu_t *c)
{
    if (ARG(0) == g_listener) {
        for (int k = 0; k < 3; k++) { g_lface[k] = farg(c, 1 + k); g_lup[k] = farg(c, 4 + k); }
        update_all3d();
    }
    hle_ret(c, 0, 7);
}
static void m_open_3D_listener(cpu_t *c)
{
    if (!g_listener) g_listener = gheap_alloc(0x40);
    hle_ret(c, g_listener, 1);
}
static void m_close_3D_listener(cpu_t *c) { hle_ret(c, 0, 1); }

static const hle_def defs[] = {
    {"MSS32.dll", "_AIL_quick_startup@20", m_quick_startup},
    {"MSS32.dll", "_AIL_quick_handles@12", m_quick_handles},
    {"MSS32.dll", "_AIL_quick_shutdown@0", m_quick_shutdown},
    {"MSS32.dll", "_AIL_set_preference@8", m_set_preference},
    {"MSS32.dll", "_AIL_lock@0", m_lock},
    {"MSS32.dll", "_AIL_unlock@0", m_lock},
    {"MSS32.dll", "_AIL_last_error@0", m_last_error},
    {"MSS32.dll", "_AIL_digital_master_volume@4", m_digital_master_volume},
    {"MSS32.dll", "_AIL_set_digital_master_volume@8", m_set_digital_master_volume},
    {"MSS32.dll", "_AIL_get_DirectSound_info@12", m_get_DirectSound_info},
    {"MSS32.dll", "_AIL_allocate_sample_handle@4", m_allocate_sample_handle},
    {"MSS32.dll", "_AIL_release_sample_handle@4", m_release_sample_handle},
    {"MSS32.dll", "_AIL_init_sample@4", m_init_sample},
    {"MSS32.dll", "_AIL_set_sample_file@12", m_set_sample_file},
    {"MSS32.dll", "_AIL_start_sample@4", m_start_sample},
    {"MSS32.dll", "_AIL_stop_sample@4", m_stop_sample},
    {"MSS32.dll", "_AIL_resume_sample@4", m_resume_sample},
    {"MSS32.dll", "_AIL_end_sample@4", m_end_sample},
    {"MSS32.dll", "_AIL_sample_status@4", m_sample_status},
    {"MSS32.dll", "_AIL_set_sample_volume@8", m_set_sample_volume},
    {"MSS32.dll", "_AIL_set_sample_loop_count@8", m_set_sample_loop_count},
    {"MSS32.dll", "_AIL_minimum_sample_buffer_size@12", m_minimum_sample_buffer_size},
    {"MSS32.dll", "_AIL_set_sample_type@12", m_set_sample_type},
    {"MSS32.dll", "_AIL_set_sample_playback_rate@8", m_set_sample_playback_rate},
    {"MSS32.dll", "_AIL_set_sample_pan@8", m_set_sample_pan},
    {"MSS32.dll", "_AIL_set_sample_user_data@12", m_set_sample_user_data},
    {"MSS32.dll", "_AIL_sample_user_data@8", m_sample_user_data},
    {"MSS32.dll", "_AIL_register_EOB_callback@8", m_register_EOB_callback},
    {"MSS32.dll", "_AIL_serve@0", m_serve},
    {"MSS32.dll", "_AIL_sample_buffer_ready@4", m_sample_buffer_ready},
    {"MSS32.dll", "_AIL_load_sample_buffer@16", m_load_sample_buffer},
    {"MSS32.dll", "_AIL_sample_buffer_info@20", m_sample_buffer_info},
    {"MSS32.dll", "_AIL_mem_alloc_lock@4", m_mem_alloc_lock},
    {"MSS32.dll", "_AIL_mem_free_lock@4", m_mem_free_lock},
    {"MSS32.dll", "_AIL_get_preference@4", m_get_preference},
    {"MSS32.dll", "_AIL_enumerate_3D_providers@12", m_enumerate_3D_providers},
    {"MSS32.dll", "_AIL_open_3D_provider@4", m_open_3D_provider},
    {"MSS32.dll", "_AIL_close_3D_provider@4", m_close_3D_provider},
    {"MSS32.dll", "_AIL_set_3D_provider_preference@12", m_set_3D_provider_preference},
    {"MSS32.dll", "_AIL_allocate_3D_sample_handle@4", m_allocate_3D_sample_handle},
    {"MSS32.dll", "_AIL_release_3D_sample_handle@4", m_release_sample_handle},
    {"MSS32.dll", "_AIL_set_3D_sample_file@8", m_set_3D_sample_file},
    {"MSS32.dll", "_AIL_start_3D_sample@4", m_start_sample},
    {"MSS32.dll", "_AIL_stop_3D_sample@4", m_stop_sample},
    {"MSS32.dll", "_AIL_resume_3D_sample@4", m_resume_sample},
    {"MSS32.dll", "_AIL_end_3D_sample@4", m_end_sample},
    {"MSS32.dll", "_AIL_3D_sample_status@4", m_sample_status},
    {"MSS32.dll", "_AIL_set_3D_sample_volume@8", m_set_sample_volume},
    {"MSS32.dll", "_AIL_set_3D_sample_loop_count@8", m_set_sample_loop_count},
    {"MSS32.dll", "_AIL_set_3D_sample_float_distances@20", m_set_3D_sample_float_distances},
    {"MSS32.dll", "_AIL_set_3D_position@16", m_set_3D_position},
    {"MSS32.dll", "_AIL_set_3D_orientation@28", m_set_3D_orientation},
    {"MSS32.dll", "_AIL_open_3D_listener@4", m_open_3D_listener},
    {"MSS32.dll", "_AIL_close_3D_listener@4", m_close_3D_listener},
};

void hle_mss_init(void)
{
    memset(g_s, 0, sizeof g_s);
    g_trace = getenv("TPM_TRACE_MSS") != NULL;
    g_dig = g_listener = g_provider_name = g_err = 0;
    hle_register(defs, (int)(sizeof defs / sizeof defs[0]));
}

void hle_mss_process_init(void)
{
    static const char name[] = "Miles Fast 2D Positional Audio";
    g_provider_name = gheap_alloc(sizeof name);
    hle_str_out(g_provider_name, sizeof name, name);
    g_err = gheap_alloc(4);
}
