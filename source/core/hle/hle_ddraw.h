/* DirectDraw state shared between the DirectDraw and Direct3D HLE. */
#ifndef HLE_DDRAW_H
#define HLE_DDRAW_H

#include <stdint.h>
#include "core/gmem.h"

/* DDSCAPS */
#define DDSCAPS_BACKBUFFER     0x00000004u
#define DDSCAPS_COMPLEX        0x00000008u
#define DDSCAPS_FLIP           0x00000010u
#define DDSCAPS_FRONTBUFFER    0x00000020u
#define DDSCAPS_OFFSCREENPLAIN 0x00000040u
#define DDSCAPS_PRIMARYSURFACE 0x00000200u
#define DDSCAPS_SYSTEMMEMORY   0x00000800u
#define DDSCAPS_TEXTURE        0x00001000u
#define DDSCAPS_3DDEVICE       0x00002000u
#define DDSCAPS_VIDEOMEMORY    0x00004000u
#define DDSCAPS_ZBUFFER        0x00020000u
#define DDSCAPS_MIPMAP         0x00400000u
#define DDSCAPS_LOCALVIDMEM    0x10000000u
/* DDPIXELFORMAT flags */
#define DDPF_ALPHAPIXELS       0x00000001u
#define DDPF_PALETTEINDEXED8   0x00000020u
#define DDPF_RGB               0x00000040u
#define DDPF_ZBUFFER           0x00000400u

typedef struct {
    uint32_t flags, bits, r, g, b, a;          /* bits: 8 (paletted), 16, 32; z: flags has DDPF_ZBUFFER */
} pixfmt;

typedef struct dd_palette dd_palette;
struct dd_palette { uint32_t entries[256]; uint32_t caps; };   /* PALETTEENTRY = R,G,B,flags bytes */

typedef struct dd_surface dd_surface;
struct dd_surface {
    uint32_t    w, h, pitch, caps, caps2;
    pixfmt      pf;
    gptr        pixels;                 /* guest memory (game writes through Lock) */
    uint32_t    ck_lo, ck_hi;           /* source colour key */
    int         has_ck, locked;
    int         in_virt;                /* pixels come from gvirt_reserve (heap was full) */
    gptr        self;                   /* guest IDirectDrawSurface4 pointer */
    gptr        back;                   /* attached back buffer (flip chain) */
    gptr        zbuf;                   /* attached z-buffer */
    gptr        mip;                    /* next mip level */
    gptr        palette;                /* IDirectDrawPalette */
    uint32_t    texture_id;             /* renderer texture, 0 = none */
    uint32_t    version;                /* bumped on every content change (texture cache) */
    uint32_t    gpu_tex_version;        /* version uploaded to texture_id */
    uint8_t     gpu_tex_ck;             /* colour key state of that upload */
};

dd_surface *dd_surface_of(gptr p);                 /* NULL if not a surface */
dd_palette *dd_palette_of(gptr p);
void        dd_write_pixfmt(gptr dst, const pixfmt *pf);
gptr        dd_primary(void);                      /* guest pointer of the visible surface, 0 if none */

#endif
