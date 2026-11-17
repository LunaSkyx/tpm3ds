/* Software rasteriser backend of r3d (see r3d_soft.c). */
#ifndef R3D_SOFT_H
#define R3D_SOFT_H

#include <stdint.h>
#include "core/gmem.h"

void r3d_soft_draw(uint32_t prim, uint32_t fvf, gptr verts, uint32_t nverts, gptr indices, uint32_t nindices);
void r3d_soft_clear(uint32_t nrects, gptr rects, uint32_t flags, uint32_t color, float z);

#endif
