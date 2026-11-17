/* Software mixer for the HLE sound APIs (Miles, DirectSound, Bink audio).
 * Runs on the guest thread: mixer_service() renders the frames that are due by
 * wall clock and hands them to pal_audio_submit(). Voices read sample data
 * straight from guest memory, so no locking is needed. */
#ifndef MIXER_H
#define MIXER_H

#include <stdint.h>
#include "core/gmem.h"

#define MIX_RATE   22050
#define MIX_VOICES 48

enum { VS_FREE = 1, VS_DONE = 2, VS_PLAYING = 4, VS_STOPPED = 8 };   /* = Miles SMP_* values */

int      mixer_alloc(void);                 /* voice id, -1 if none free */
void     mixer_release(int v);
/* fmt: 1 = PCM, 0x11 = IMA ADPCM. data/len point into guest memory. */
int      mixer_set_data(int v, gptr data, uint32_t len, int fmt, int channels, int bits, int rate, int block_align);
int      mixer_set_wav(int v, gptr wav_image);           /* RIFF WAVE in guest memory; 1 = ok */
void     mixer_start(int v);                 /* from the beginning */
void     mixer_stop(int v);                  /* pause */
void     mixer_resume(int v);
void     mixer_end(int v);                   /* -> DONE */
int      mixer_status(int v);
void     mixer_set_volume(int v, float vol); /* 0..1 */
void     mixer_set_pan(int v, float pan);    /* -1 left .. 1 right */
void     mixer_set_loops(int v, int loops);  /* 0 = forever, n = play n times */
void     mixer_set_master(float vol);
/* Streaming voices (Miles double buffering): PCM in two guest buffers played in turn. */
int      mixer_stream_begin(int v, int channels, int bits, int rate);
void     mixer_stream_load(int v, int n, gptr data, uint32_t len);   /* len 0 = end of stream */
int      mixer_stream_ready(int v);                  /* buffer that can take data, -1 if none */
void     mixer_stream_info(int v, uint32_t pos[2], uint32_t len[2]);
uint32_t mixer_stream_eob(int v);
void     mixer_set_rate(int v, int rate);
/* DirectSound-style buffers: play from the current position, optionally looping. */
uint32_t mixer_get_pos(int v);                       /* frame */
void     mixer_set_pos(int v, uint32_t frame);
void     mixer_play(int v, int looping);                    /* buffers finished so far */
float    mixer_master(void);

void     mixer_service_io(void);             /* from file IO: render 250 ms ahead */
void     mixer_thread_tick(void);            /* mixer thread: render what is due */
void     mixer_set_threaded(int on);         /* 1: mixer_service() from the guest does nothing */
uint32_t mixer_underruns(void);
void     pal_audio_lock(void);               /* provided by the pal (weak no-op in mixer.c) */
void     pal_audio_unlock(void);
void     mixer_service(void);                /* render what is due; call often */
void     mixer_render(int16_t *out, uint32_t frames);    /* stereo interleaved (tests) */

#endif
