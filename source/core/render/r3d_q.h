/* Draw thread of the GPU front end (3DS, New 3DS only).
 *
 * r3d_draw()/r3d_clear() on the guest thread only COPY their input (vertex/index bytes, rects) and, when the render state changed
 * since the previous entry, a snapshot of the r3d state into a ring buffer; a worker on core 2 replays the entries through the
 * normal GPU front end (vertex fetch + transform, culling, batching, pal draw calls). Everything else that touches the GPU or a
 * surface the worker may still read (Lock/Blt/Flip/present/free/target switch) first calls r3d_q_fence(), which returns when the
 * worker has processed every entry queued so far. See docs/PLAN.md 46i. */
#ifndef R3D_Q_H
#define R3D_Q_H

#include <stdint.h>
#include "core/render/r3d.h"

extern int g_r3d_q_on;                       /* 1: r3d_draw/r3d_clear are queued */

int  r3d_q_start(void);                      /* 1 on success (worker on core 2); call before the first draw */
void r3d_q_shutdown(void);
void r3d_q_fence(void);                      /* wait until the worker is idle (no-op when the queue is off or empty) */
void r3d_q_flush(void);                      /* r3d_gpu_flush(), executed in order (fence included) */
void r3d_q_state_reset(void);                /* the front state was reset: the next draw sends a full snapshot */
/* enqueue; the caller has checked r3d_gpu_front_ok() */
void r3d_q_draw(uint32_t prim, uint32_t fvf, const uint8_t *verts, uint32_t nverts, const uint8_t *indices, uint32_t nindices);
void r3d_q_clear(uint32_t nrects, const uint32_t *rects4, uint32_t flags, uint32_t color, float z);
int  r3d_q_on_worker(void);                  /* 1 when called from the worker thread */
extern uint32_t g_r3d_q_since_present;       /* draws/clears queued since the last (async) present */
void r3d_q_present(uint32_t pix, uint32_t ver, uint32_t pitch, uint32_t h);   /* worker: r3d_gpu_present_w */
void r3d_q_flip(uint32_t fpix, uint32_t fver, uint32_t bpix, uint32_t bver);  /* worker: r3d_gpu_flip_w */

#endif
