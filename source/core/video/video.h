/* Presentation: the HLE hands finished frames here; this module converts them
 * to RGB565, keeps the last one (screenshots, tests) and passes it to pal. */
#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>
#include "core/gmem.h"

#define VIDEO_MAX_W 800
#define VIDEO_MAX_H 480

/* The screen the guest sees (display mode, window, metrics): 640x480, or 800x480 in
 * widescreen mode (the 3DS top screen's 5:3 at 2x). Set before guest_init. */
extern uint32_t g_screen_w, g_screen_h;

/* src: guest pixels; bits 16 (RGB565 or 555 per masks) or 8 (pal: 256 x 0x00BBGGRR). */
void     video_present(gptr src, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bits,
                       uint32_t gmask, const uint32_t *pal);
/* A GPU target already in host memory: present it without the guest round trip.
 * Rows run bottom-up (display-transfer order). */
void     video_present_staging(const uint16_t *rgb565, uint32_t w, uint32_t h);
uint16_t *video_direct_begin(void);          /* buffer to receive a copy of the next direct frame, or NULL */
void     video_direct_done(uint32_t w, uint32_t h, int copied);   /* a frame went to the screen without a CPU pass */
/* Keep the first frame at or after this count in g_frame (screenshots). */
void     video_request_capture(uint32_t frame);
uint32_t video_frame_count(void);
const uint16_t *video_last_frame(uint32_t *w, uint32_t *h);
int      video_save_bmp(const char *path);          /* 1 = ok */

#endif
