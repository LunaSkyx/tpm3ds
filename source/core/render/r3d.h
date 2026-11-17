/* Renderer interface for the Direct3D HLE (DX6 DrawPrimitive model).
 * Vertices stay in guest memory; the backend reads them through the FVF layout. */
#ifndef R3D_H
#define R3D_H

#include <stdint.h>
#include "core/gmem.h"
#include "core/hle/hle_ddraw.h"

/* D3DFVF */
#define FVF_XYZ      0x002u
#define FVF_XYZRHW   0x004u
#define FVF_NORMAL   0x010u
#define FVF_DIFFUSE  0x040u
#define FVF_SPECULAR 0x080u
#define FVF_TEXCOUNT(f) (((f) >> 8) & 0xfu)

/* D3DPRIMITIVETYPE */
enum { PT_POINTLIST = 1, PT_LINELIST, PT_LINESTRIP, PT_TRIANGLELIST, PT_TRIANGLESTRIP, PT_TRIANGLEFAN };

typedef struct { uint32_t x, y, w, h; float minz, maxz; } r3d_viewport;

typedef struct {
    uint32_t rs[256];              /* render states by D3DRENDERSTATETYPE */
    uint32_t tss[2][32];           /* texture stage states */
    dd_surface *tex[2];
    dd_surface *target, *zbuf;
    r3d_viewport vp;
    float    world[16], view[16], proj[16];
    /* statistics for tests and the log */
    uint32_t scenes, draws, tris, clears, tris_drawn;
    int      warned_lighting, warned_lines;
    int      probe_budget;          /* TPM_R3D_PROBE log lines left */
    uint32_t gen;                   /* bumped whenever render/stage/viewport/target state changes (draw state cache) */
    uint32_t xf_gen;                /* bumped by SetTransform: world/view/proj generation (MVP cache) */
    uint32_t legacy_blend;          /* D3DTBLEND_* from RENDERSTATE_TEXTUREMAPBLEND; 0 once stage states are set */
} r3d_state;

extern r3d_state g_r3d;
extern int g_r3d_null;
extern int g_r3d_prof;    /* enables per-draw/per-clear raster timing */

void     r3d_reset(void);
void     r3d_state_dirty(void);
void     r3d_set_target(dd_surface *target, dd_surface *zbuf);
void     r3d_begin_scene(void);
void     r3d_end_scene(void);
void     r3d_clear(uint32_t nrects, gptr rects, uint32_t flags, uint32_t color, float z);
/* indices == 0: non-indexed. stride from the FVF. */
void     r3d_draw(uint32_t prim, uint32_t fvf, gptr verts, uint32_t nverts, gptr indices, uint32_t nindices);
uint32_t r3d_fvf_stride(uint32_t fvf);
/* SetRenderState, including the DX5 texture states that IDirect3DDevice3 maps onto stage 0. */
void     r3d_set_render_state(uint32_t state, uint32_t value);
void     r3d_set_stage_state(uint32_t stage, uint32_t state, uint32_t value);

#endif
