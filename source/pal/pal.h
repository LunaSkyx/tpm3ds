/* Platform abstraction layer. The core includes only this header:
 * no 3DS, Win32 or host-OS types may appear here. */
#ifndef PAL_H
#define PAL_H

#include <stddef.h>
#include <stdint.h>

/* ---- files (read-only access to the player's original data) ---- */
typedef struct pal_file pal_file;
pal_file *pal_file_open(const char *path);          /* NULL if missing */
int64_t   pal_file_size(pal_file *f);
size_t    pal_file_read_at(pal_file *f, uint64_t off, void *dst, size_t n);
void      pal_file_close(pal_file *f);

/* ---- write side: only for files the port creates (caches, unpacked
 * archives, saves) - never for the player's original files ---- */
typedef struct pal_wfile pal_wfile;
pal_wfile *pal_wfile_create(const char *path);       /* truncates */
int        pal_wfile_write(pal_wfile *f, const void *src, size_t n);  /* 1 = ok */
int        pal_wfile_close(pal_wfile *f);            /* 1 = ok (all data flushed) */
int        pal_file_rename(const char *from, const char *to);  /* replaces 'to' */
int        pal_file_remove(const char *path);

/* ---- general file handles (HLE CreateFile & co.): stdio semantics ---- */
typedef struct pal_fh pal_fh;
enum { PAL_FH_READ, PAL_FH_RW_EXISTING, PAL_FH_RW_CREATE, PAL_FH_RW_TRUNCATE };
pal_fh  *pal_fh_open(const char *path, int mode);      /* NULL on failure */
int64_t  pal_fh_read(pal_fh *f, void *dst, size_t n);
int64_t  pal_fh_write(pal_fh *f, const void *src, size_t n);
int64_t  pal_fh_seek(pal_fh *f, int64_t off, int whence);   /* returns new position or -1 */
void     pal_fh_close(pal_fh *f);
int      pal_mkdir(const char *path);
int      pal_is_dir(const char *path);

/* ---- directory listing ---- */
typedef struct pal_dir pal_dir;
pal_dir *pal_dir_open(const char *path);
int      pal_dir_next(pal_dir *d, char *name, size_t n, int *is_dir, uint32_t *size);  /* 1 = got one */
void     pal_dir_close(pal_dir *d);

/* ---- time ---- */
/* Stall diagnostics: a ring of the last markers set by pal_where(); the guest stall monitor prints it. */
void     pal_where(const char *tag);
void     pal_where_dump(void);
uint32_t pal_time_ms(void);                         /* ms since pal_log_open */
uint64_t pal_time_us(void);                         /* us since pal_log_open */
void     pal_sleep_ms(uint32_t ms);                 /* only for the guest's own Sleep() */

/* ---- video: finished RGB565 frames (w x h, tightly packed) ---- */
void pal_video_present(const uint16_t *rgb565, uint32_t w, uint32_t h);
/* Same, but source rows run bottom-up (GPU display transfer output). */
void pal_video_present_flip(const uint16_t *rgb565, uint32_t w, uint32_t h);
/* A movie frame written straight into the locked primary. The guest frame is wider than the
 * video: the game copies the decoded picture at x = 0 (`BinkCopyToBuffer(..., dstx = 0,
 * dsty = 0, ...)`, 0x497beb) and leaves the rest of the surface black, which on a widescreen
 * screen shows as a picture on the left. Backends that can, present the left-aligned 4:3
 * picture filling the screen instead. */
void pal_video_present_movie(const uint16_t *rgb565, uint32_t w, uint32_t h);

/* ---- threads: used only by core/gthread (one guest thread runs at a time) ---- */
typedef struct pal_thread pal_thread;
typedef struct pal_sem pal_sem;
pal_thread *pal_thread_spawn(void (*fn)(void *), void *arg, uint32_t stack_bytes);   /* NULL on failure */
pal_thread *pal_thread_spawn_bg(void (*fn)(void *), void *arg, uint32_t stack_bytes);   /* background work: second core on a New 3DS, else lowest priority */
void        pal_thread_wait(pal_thread *t);                                             /* and free */
pal_sem    *pal_sem_new(int initial);
void        pal_sem_wait(pal_sem *s);
void        pal_sem_post(pal_sem *s);
void        pal_sem_free(pal_sem *s);

/* ---- GPU (optional): the core hands over clip-space triangles and a small
 * fixed-function state; pal_gpu_init returns 0 where the core renders in
 * software (host build). One render target of the size given to init. ---- */
typedef struct {
    float   x, y, z, w;            /* clip space, Direct3D conventions: x right, y up, 0 <= z <= w */
    float   u, v;                  /* texture 0, v = 0 at the top row */
    float   fog;                   /* 1 = no fog, 0 = fog colour */
    uint8_t rgba[4];               /* diffuse */
} pal_gpu_vertex;
enum { PAL_TEX_RGB565, PAL_TEX_RGBA5551, PAL_TEX_RGBA4444 };
enum { PAL_TA_DIFFUSE, PAL_TA_TEXTURE };
typedef struct {
    uint32_t tex;                  /* 0: untextured */
    uint8_t  linear, clamp_u, clamp_v;
    uint8_t  color_op, color_a1, color_a2, alpha_op, alpha_a1, alpha_a2;   /* D3DTEXTUREOP, PAL_TA_* */
    uint8_t  blend, src_blend, dst_blend;      /* D3DBLEND */
    uint8_t  alpha_test, alpha_func, alpha_ref; /* D3DCMPFUNC, 0..255 */
    uint8_t  z_test, z_write, z_func, color_write;
    uint8_t  fog;
    uint32_t fog_rgb;              /* 0x00RRGGBB */
    uint16_t clip_x0, clip_y0, clip_x1, clip_y1;   /* scissor in target pixels, y down */
} pal_gpu_state;
int      pal_gpu_init(uint32_t w, uint32_t h);
#define PAL_GPU_MAX_TARGETS 2
int      pal_gpu_target_count(void);                              /* persistent render targets created */
void     pal_gpu_target_select(int slot);                          /* target for subsequent draws */
int      pal_gpu_target_stage(int slot);                           /* target -> staging, no guest copy */
int      pal_gpu_present_direct(int slot, uint16_t *copy);                          /* rotated target -> top screen, 1 if presented */
const uint16_t *pal_gpu_staging(void);                              /* bottom-up rows, w*h */
uint32_t pal_gpu_tex_create(uint32_t w, uint32_t h, int fmt);   /* powers of two 8..1024; 0 on failure */
void     pal_gpu_tex_upload(uint32_t id, const uint16_t *texels);   /* w*h, row 0 = top */
void     pal_gpu_tex_free(uint32_t id);
/* Free bytes in the 3DS linear (VRAM-backed) heap, where the GPU targets and textures live.
 * 0 on backends without one. Used by the per-scene RAM trace (core/render/r3d.c). */
uint32_t pal_linear_free(void);
uint32_t pal_host_free(void);   /* free bytes in the host (newlib) heap; 0 where unknown */
void     pal_gpu_target_load(int slot, const uint8_t *rgb565, uint32_t pitch);   /* guest pixels -> GPU target */
void     pal_gpu_target_store(int slot, uint8_t *rgb565, uint32_t pitch);        /* finish drawing, GPU -> guest pixels */
void     pal_gpu_draw(const pal_gpu_state *st, const pal_gpu_vertex *v, uint32_t n);   /* triangle list */

/* ---- input: events in DirectInput terms (DIK codes, mouse deltas) ---- */
enum { PAL_IN_KEY = 1, PAL_IN_MOUSE_MOVE, PAL_IN_MOUSE_BUTTON, PAL_IN_QUIT };
typedef struct { int type; uint32_t code; int32_t a, b; } pal_input_event;   /* key: code=DIK, a=down */
int pal_input_next(pal_input_event *e);             /* 1 = got one */

/* ---- audio: stereo s16 at 22050 Hz, pushed from the guest thread ---- */
void pal_audio_submit(const int16_t *frames, uint32_t n);

/* ---- log: one file per run, previous run kept as prev_path ---- */
int  pal_log_open(const char *path, const char *prev_path);
void pal_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void pal_log_flush(void);                           /* call on important steps */
void pal_log_close(void);

/* ---- threads: every thread is registered and joined before exit ---- */
typedef void (*pal_thread_fn)(void *arg);
int  pal_thread_start(const char *name, pal_thread_fn fn, void *arg, unsigned stack_bytes);
void pal_request_quit(void);
int  pal_quit_requested(void);                      /* workers poll this */
int  pal_threads_join_all(void);                    /* returns number NOT joined */

#endif
