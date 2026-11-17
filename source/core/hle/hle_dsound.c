/* DirectSound (DSOUND.DLL, DX5-level) HLE on the software mixer.
 * Used by the lifted IMUSE.DLL, which gets the IDirectSound object from Miles
 * (AIL_get_DirectSound_info) and streams music through looping secondary
 * buffers, and by the exe's DirectSoundCreate import. A secondary buffer is
 * guest memory played by a mixer voice; Lock hands out pointers into it, so the
 * guest writes sample data in place exactly as with a real buffer. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "core/audio/mixer.h"
#include "core/gheap.h"
#include "core/hle/hle_com.h"
#include "core/hle/hle_dsound.h"
#include "pal/pal.h"

#define DS_OK              0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_BADFORMAT    0x88780064u
#define DSBCAPS_PRIMARYBUFFER 0x1u
#define DSBPLAY_LOOPING    0x1u
#define DSBSTATUS_PLAYING  0x1u
#define DSBSTATUS_LOOPING  0x4u

extern com_iface IF_DSound, IF_DSBuffer;
static const com_iface *const ds_ifaces[] = {&IF_DSound, NULL};
static const com_iface *const buf_ifaces[] = {&IF_DSBuffer, NULL};

typedef struct {
    int      primary, voice, looping;
    gptr     data;
    uint32_t bytes, block, rate, flags;
    int32_t  volume, pan;              /* hundredths of dB */
} ds_buffer;

static gptr g_ds;                      /* the one IDirectSound object */

static void buf_free(void *p)
{
    ds_buffer *b = p;
    if (b->voice >= 0) mixer_release(b->voice);
    if (b->data) gheap_free(b->data);
    free(b);
}

static float db_to_lin(int32_t hdb) { return hdb <= -10000 ? 0.0f : powf(10.0f, (float)hdb / 2000.0f); }

static void apply_gain(ds_buffer *b)
{
    if (b->voice < 0) return;
    mixer_set_volume(b->voice, db_to_lin(b->volume));
    float att = 1.0f - db_to_lin(-abs(b->pan));          /* the far side is attenuated by |pan| dB */
    mixer_set_pan(b->voice, b->pan > 0 ? att : -att);
}

gptr hle_dsound_object(void)
{
    if (!g_ds) g_ds = com_create("dsound", NULL, NULL, ds_ifaces);
    return g_ds;
}

/* DirectSoundCreate(guid, out, unk) - DSOUND ordinal 1 */
static void d_DirectSoundCreate(cpu_t *c)
{
    gptr ds = hle_dsound_object();
    com_addref(ds);
    gmem_wr32(ARG(1), ds);
    hle_ret(c, DS_OK, 3);
}

/* ---- IDirectSound ---- */
/* CreateSoundBuffer(this, DSBUFFERDESC*, out, unk) */
static void ds_CreateSoundBuffer(cpu_t *c)
{
    gptr d = ARG(1);
    uint32_t flags = gmem_rd32(d + 4), bytes = gmem_rd32(d + 8);
    gptr wfx = gmem_rd32(d + 16);
    ds_buffer *b = calloc(1, sizeof *b);
    b->voice = -1;
    b->flags = flags;
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        b->primary = 1;
    } else {
        if (!wfx || !bytes) { free(b); hle_ret(c, DSERR_INVALIDPARAM, 4); return; }
        uint32_t tag = gmem_rd16(wfx), ch = gmem_rd16(wfx + 2), rate = gmem_rd32(wfx + 4), bits = gmem_rd16(wfx + 14);
        b->block = gmem_rd16(wfx + 12);
        b->rate = rate;
        b->bytes = bytes;
        b->data = gheap_alloc(bytes);
        b->voice = mixer_alloc();
        if (tag != 1 || !b->data || b->voice < 0 ||
            !mixer_set_data(b->voice, b->data, bytes, 1, (int)ch, (int)bits, (int)rate, (int)b->block)) {
            pal_log("dsound: CreateSoundBuffer format %lu %lu ch %lu Hz %lu bit not supported", (unsigned long)tag,
                    (unsigned long)ch, (unsigned long)rate, (unsigned long)bits);
            buf_free(b);
            hle_ret(c, DSERR_BADFORMAT, 4);
            return;
        }
    }
    gmem_wr32(ARG(2), com_create("dsbuffer", b, buf_free, buf_ifaces));
    pal_log("dsound: CreateSoundBuffer %s %lu bytes flags %lx", b->primary ? "primary" : "secondary",
            (unsigned long)b->bytes, (unsigned long)flags);
    hle_ret(c, DS_OK, 4);
}
static void ds_GetCaps(cpu_t *c)
{
    gptr p = ARG(1);
    uint32_t size = gmem_rd32(p);
    for (uint32_t k = 4; k < size && k < 0x60; k += 4) gmem_wr32(p + k, 0);
    gmem_wr32(p + 4, 0x00000100u | 0x00000200u | 0x00000400u | 0x00000800u | 0x00000004u | 0x00000008u);  /* SECONDARY MONO/STEREO/8/16, CONTINUOUSRATE */
    gmem_wr32(p + 8, 100);              /* min secondary rate */
    gmem_wr32(p + 12, 100000);          /* max secondary rate */
    hle_ret(c, DS_OK, 2);
}
static void ds_SetCooperativeLevel(cpu_t *c) { hle_ret(c, DS_OK, 3); }
static void ds_GetSpeakerConfig(cpu_t *c) { gmem_wr32(ARG(1), 4); hle_ret(c, DS_OK, 2); }   /* STEREO */
static void ds_ok2(cpu_t *c) { hle_ret(c, DS_OK, 2); }
static void ds_ok1(cpu_t *c) { hle_ret(c, DS_OK, 1); }

/* ---- IDirectSoundBuffer ---- */
#define BUF(var, n) ds_buffer *var = com_data(ARG(0), "dsbuffer"); if (!var) { hle_ret(c, DSERR_INVALIDPARAM, n); return; }

static void db_GetCaps(cpu_t *c)
{
    BUF(b, 2);
    gmem_wr32(ARG(1) + 4, b->flags);
    gmem_wr32(ARG(1) + 8, b->bytes);
    gmem_wr32(ARG(1) + 12, 0);
    gmem_wr32(ARG(1) + 16, 0);
    hle_ret(c, DS_OK, 2);
}
/* GetCurrentPosition(this, *play, *write) */
static void db_GetCurrentPosition(cpu_t *c)
{
    BUF(b, 3);
    uint32_t play = 0, write = 0;
    if (!b->primary && b->bytes) {
        play = mixer_get_pos(b->voice) * b->block % b->bytes;
        write = (play + (b->rate / 100u) * b->block) % b->bytes;          /* 10 ms ahead */
    }
    if (ARG(1)) gmem_wr32(ARG(1), play);
    if (ARG(2)) gmem_wr32(ARG(2), write);
    hle_ret(c, DS_OK, 3);
}
static void db_GetStatus(cpu_t *c)
{
    BUF(b, 2);
    uint32_t st = 0;
    if (!b->primary && mixer_status(b->voice) == VS_PLAYING) st = DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0);
    gmem_wr32(ARG(1), st);
    hle_ret(c, DS_OK, 2);
}
/* Lock(this, offset, bytes, *p1, *n1, *p2, *n2, flags) */
static void db_Lock(cpu_t *c)
{
    BUF(b, 8);
    if (b->primary) { hle_ret(c, DSERR_INVALIDPARAM, 8); return; }
    uint32_t off = ARG(1), n = ARG(2), flags = ARG(7);
    if (flags & 1u) { uint32_t w; mixer_service(); w = (mixer_get_pos(b->voice) * b->block + (b->rate / 100u) * b->block) % b->bytes; off = w; }
    if (flags & 2u) { off = 0; n = b->bytes; }
    if (off >= b->bytes || n > b->bytes) { hle_ret(c, DSERR_INVALIDPARAM, 8); return; }
    uint32_t n1 = off + n <= b->bytes ? n : b->bytes - off, n2 = n - n1;
    gmem_wr32(ARG(3), b->data + off);
    gmem_wr32(ARG(4), n1);
    if (ARG(5)) gmem_wr32(ARG(5), n2 ? b->data : 0);
    if (ARG(6)) gmem_wr32(ARG(6), n2);
    hle_ret(c, DS_OK, 8);
}
static void db_Unlock(cpu_t *c) { hle_ret(c, DS_OK, 5); }
/* Play(this, reserved, priority, flags) */
static void db_Play(cpu_t *c)
{
    BUF(b, 4);
    if (!b->primary) {
        b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
        apply_gain(b);
        mixer_play(b->voice, b->looping);
    }
    hle_ret(c, DS_OK, 4);
}
static void db_Stop(cpu_t *c)
{
    BUF(b, 1);
    if (!b->primary) mixer_stop(b->voice);
    hle_ret(c, DS_OK, 1);
}
static void db_SetCurrentPosition(cpu_t *c)
{
    BUF(b, 2);
    if (!b->primary && b->block) mixer_set_pos(b->voice, ARG(1) / b->block);
    hle_ret(c, DS_OK, 2);
}
static void db_SetVolume(cpu_t *c) { BUF(b, 2); b->volume = (int32_t)ARG(1); apply_gain(b); hle_ret(c, DS_OK, 2); }
static void db_GetVolume(cpu_t *c) { BUF(b, 2); gmem_wr32(ARG(1), (uint32_t)b->volume); hle_ret(c, DS_OK, 2); }
static void db_SetPan(cpu_t *c)    { BUF(b, 2); b->pan = (int32_t)ARG(1); apply_gain(b); hle_ret(c, DS_OK, 2); }
static void db_GetPan(cpu_t *c)    { BUF(b, 2); gmem_wr32(ARG(1), (uint32_t)b->pan); hle_ret(c, DS_OK, 2); }
static void db_SetFrequency(cpu_t *c)
{
    BUF(b, 2);
    if (!b->primary && ARG(1)) { b->rate = ARG(1); mixer_set_rate(b->voice, (int)ARG(1)); }
    hle_ret(c, DS_OK, 2);
}
static void db_GetFrequency(cpu_t *c) { BUF(b, 2); gmem_wr32(ARG(1), b->rate); hle_ret(c, DS_OK, 2); }
static void db_SetFormat(cpu_t *c) { hle_ret(c, DS_OK, 2); }      /* primary format: the mixer converts */
static void db_Restore(cpu_t *c) { hle_ret(c, DS_OK, 1); }

static const com_method m_ds[] = {
    COM_IUNKNOWN, {"CreateSoundBuffer", ds_CreateSoundBuffer}, {"GetCaps", ds_GetCaps}, {"DuplicateSoundBuffer", NULL},
    {"SetCooperativeLevel", ds_SetCooperativeLevel}, {"Compact", ds_ok1}, {"GetSpeakerConfig", ds_GetSpeakerConfig},
    {"SetSpeakerConfig", ds_ok2}, {"Initialize", ds_ok2},
};
static const com_method m_buf[] = {
    COM_IUNKNOWN, {"GetCaps", db_GetCaps}, {"GetCurrentPosition", db_GetCurrentPosition}, {"GetFormat", NULL},
    {"GetVolume", db_GetVolume}, {"GetPan", db_GetPan}, {"GetFrequency", db_GetFrequency}, {"GetStatus", db_GetStatus},
    {"Initialize", NULL}, {"Lock", db_Lock}, {"Play", db_Play}, {"SetCurrentPosition", db_SetCurrentPosition},
    {"SetFormat", db_SetFormat}, {"SetVolume", db_SetVolume}, {"SetPan", db_SetPan}, {"SetFrequency", db_SetFrequency},
    {"Stop", db_Stop}, {"Unlock", db_Unlock}, {"Restore", db_Restore},
};
#define IFACE(var, name, iid, m) com_iface var = {name, iid, m, (int)(sizeof m / sizeof m[0]), 0}
IFACE(IF_DSound, "IDirectSound", 0x279afa83u, m_ds);
IFACE(IF_DSBuffer, "IDirectSoundBuffer", 0x279afa85u, m_buf);

static const hle_def defs[] = {{"DSOUND.dll", "#1", d_DirectSoundCreate}};

void hle_dsound_init(void)
{
    g_ds = 0;
    hle_register(defs, 1);
    com_register(&IF_DSound);
    com_register(&IF_DSBuffer);
}
