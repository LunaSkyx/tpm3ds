/* GPU front end of the Direct3D HLE (see r3d_gpu.c). */
#ifndef R3D_GPU_H
#define R3D_GPU_H

#include "core/render/r3d.h"

int  r3d_gpu_init(uint32_t w, uint32_t h);       /* 1: the GPU renders targets of this size */
int  r3d_gpu_active(void);
/* 0: not handled here (the caller uses the software renderer) */
int  r3d_gpu_draw(uint32_t prim, uint32_t fvf, gptr verts, uint32_t nverts, gptr indices, uint32_t nindices);
int  r3d_gpu_clear(uint32_t nrects, gptr rects, uint32_t flags, uint32_t color, float z);
/* Before the CPU reads or writes a surface: bring GPU results into guest memory. */
void r3d_gpu_sync(dd_surface *s);
/* After a Flip exchanged the two surface pointers: adopt the new versions. */
void r3d_gpu_flip(dd_surface *f, dd_surface *b);
/* 1: presented straight from the GPU target; 0: fall back to the guest copy. */
int  r3d_gpu_present(dd_surface *s);
void r3d_gpu_surface_freed(dd_surface *s);
void r3d_gpu_flush(void);                        /* submit the pending batch */
extern uint32_t g_r3d_gpu_presents, g_r3d_gpu_present_fail, g_r3d_gpu_loads, g_r3d_gpu_stores;
extern uint32_t g_r3d_gpu_batches;        /* pal_gpu_draw calls (batches drawn) */
extern uint32_t g_r3d_gpu_tex_uploads;    /* texture conversions uploaded to the GPU */

#endif
