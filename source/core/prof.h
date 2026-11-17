/* Sampling profiler (diagnostics): the platform runs prof_sample() about every ms
 * on a separate thread; prof_report() logs the hottest entries. A sample is the
 * HLE call running now, else the most recently entered lifted function
 * (needs CPU_FN_TRACE), or "idle" when every guest thread waits.
 *
 * The sampler cannot see inside a syscall-free C function; the renderer
 * therefore tags the section it is in (g_prof_sub, plain stores, no locks) and
 * such samples are charged to 0x80000000|tag instead. Zero on every exit path. */
#ifndef PROF_H
#define PROF_H
#include <stdint.h>

enum {
    PROF_SUB_NONE = 0,
    PROF_SUB_R3D_TARGET,      /* bind the GPU target for this draw */
    PROF_SUB_R3D_STATE,       /* D3D render/stage state -> pal_gpu_state */
    PROF_SUB_R3D_TEX,         /* texture format conversion + upload */
    PROF_SUB_R3D_MATRIX,      /* fetch_ctx_init: world*view*proj, viewport mapping */
    PROF_SUB_R3D_FETCH,       /* per-vertex fetch + transform */
    PROF_SUB_R3D_CULL,        /* per-triangle cull + append to the batch */
    PROF_SUB_R3D_BATCH,       /* state compare, capacity grow, flush glue */
    PROF_SUB_PAL_FRAME,       /* begin_frame / frame rollover (may wait for the GPU) */
    PROF_SUB_PAL_VCOPY,       /* vertex copy into the linear ring + v flip */
    PROF_SUB_PAL_BIND,        /* texture bind/filter/wrap */
    PROF_SUB_PAL_TEV,         /* TEV + fixed-function setters */
    PROF_SUB_PAL_CACHE,       /* GSPGPU_FlushDataCache of the vertices */
    PROF_SUB_PAL_DRAW,        /* C3D_DrawArrays */
    PROF_SUB_FLIP_STAGE,      /* GPU target -> staging display transfer */
    PROF_SUB_FLIP_BLIT,       /* staging -> top screen blit + buffer swap */
    PROF_SUB_COUNT
};

extern volatile uint32_t g_prof_sub;    /* section the guest thread is in (0: none) */
extern volatile uint32_t g_prof_epoch;  /* bumped by prof_reset (host PC sampler) */
const char *prof_sub_name(uint32_t sub);

static inline void prof_sub(uint32_t sub) { g_prof_sub = sub; }

void prof_sample(void);
void prof_report(int top);
void prof_reset(void);

#endif
