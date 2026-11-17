/* DirectDraw HLE: DirectDrawCreate/Enumerate, IDirectDraw/2/4, IDirectDrawSurface4,
 * IDirectDrawPalette, IDirectDrawClipper. One display mode: 640x480, 16-bit RGB565. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/gheap.h"
#include "core/guest.h"
#include "core/gthread.h"
#include "core/hle/hle_com.h"
#include "core/hle/hle_ddraw.h"
#include "core/render/r3d_gpu.h"
#include "core/video/video.h"
#include "pal/pal.h"

#define DD_OK 0u
#define DDERR_INVALIDPARAMS   E_INVALIDARG
#define DDERR_NOTLOCKED       0x887602e6u
#define DDERR_SURFACEBUSY     0x887601aeu
#define DDERR_NOPALETTEATTACHED 0x887601b8u
#define DDERR_NOCOLORKEY      0x887600d7u

extern com_iface IF_DirectDraw, IF_DirectDraw2, IF_DirectDraw4, IF_Direct3D3;
extern com_iface IF_Surface4, IF_Texture2, IF_Palette, IF_Clipper;

static const pixfmt PF565 = {DDPF_RGB, 16, 0xf800, 0x07e0, 0x001f, 0};
static gptr g_primary;
static uint32_t g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;

/* ------------------------------------------------------------ helpers */
static void pf_read(gptr p, pixfmt *pf)
{
    pf->flags = gmem_rd32(p + 4);
    pf->bits = gmem_rd32(p + 12);
    pf->r = gmem_rd32(p + 16);
    pf->g = gmem_rd32(p + 20);
    pf->b = gmem_rd32(p + 24);
    pf->a = gmem_rd32(p + 28);
}

void dd_write_pixfmt(gptr p, const pixfmt *pf)
{
    gmem_wr32(p, 32);
    gmem_wr32(p + 4, pf->flags);
    gmem_wr32(p + 8, 0);
    gmem_wr32(p + 12, pf->bits);
    gmem_wr32(p + 16, pf->r);
    gmem_wr32(p + 20, pf->g);
    gmem_wr32(p + 24, pf->b);
    gmem_wr32(p + 28, pf->a);
}

dd_surface *dd_surface_of(gptr p) { return com_data(p, "surface"); }
dd_palette *dd_palette_of(gptr p) { return com_data(p, "palette"); }
gptr dd_primary(void) { return g_primary; }

/* DDSURFACEDESC2 (124 bytes); DDSURFACEDESC (108) has the same layout up to ddsCaps.dwCaps. */
static void write_desc(gptr d, const dd_surface *s, uint32_t size)
{
    for (uint32_t k = 0; k < size; k += 4) gmem_wr32(d + k, 0);
    gmem_wr32(d, size);
    gmem_wr32(d + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (s->has_ck ? 0x10000u : 0u));
    gmem_wr32(d + 8, s->h);
    gmem_wr32(d + 12, s->w);
    gmem_wr32(d + 16, s->pitch);
    gmem_wr32(d + 36, s->pixels);
    if (s->has_ck) { gmem_wr32(d + 64, s->ck_lo); gmem_wr32(d + 68, s->ck_hi); }
    dd_write_pixfmt(d + 72, &s->pf);
    gmem_wr32(d + 104, s->caps);
    if (size >= 124) gmem_wr32(d + 108, s->caps2);
}

static void surface_dtor(void *data)
{
    dd_surface *s = data;
    r3d_gpu_surface_freed(s);
    if (s->pixels) { if (s->in_virt) gvirt_release(s->pixels); else gheap_free(s->pixels); }
    if (s->back) com_release(s->back);
    if (s->zbuf) com_release(s->zbuf);
    if (s->mip) com_release(s->mip);
    if (s->palette) com_release(s->palette);
    if (s->self == g_primary) g_primary = 0;
    free(s);
}

static const com_iface *const surface_ifaces[] = {&IF_Surface4, &IF_Texture2, NULL};

static gptr new_surface(uint32_t w, uint32_t h, const pixfmt *pf, uint32_t caps, uint32_t caps2)
{
    dd_surface *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->w = w; s->h = h; s->pf = *pf; s->caps = caps; s->caps2 = caps2;
    uint32_t bpp = pf->bits ? pf->bits : 16;
    s->pitch = ((w * bpp / 8) + 3) & ~3u;
    s->pixels = gheap_alloc(s->pitch * h);
    if (!s->pixels && (s->pixels = gvirt_reserve(0, s->pitch * h)) != 0) s->in_virt = 1;   /* big 800x480 buffers */
    if (!s->pixels) { pal_log("ddraw: no memory for %ux%u surface", (unsigned)w, (unsigned)h); free(s); return 0; }
    gptr p = com_create("surface", s, surface_dtor, surface_ifaces);
    s->self = p;
    return p;
}

/* ------------------------------------------------------------ DirectDrawCreate & enumerate */
typedef struct { int coop; } dd_state;
static const com_iface *const ddraw_ifaces[] = {&IF_DirectDraw, &IF_DirectDraw2, &IF_DirectDraw4, &IF_Direct3D3, NULL};
static gptr g_str_desc, g_str_name;

static void d_DirectDrawCreate(cpu_t *c)
{
    gptr p = com_create("ddraw", calloc(1, sizeof(dd_state)), free, ddraw_ifaces);
    gmem_wr32(ARG(1), p);
    hle_ret(c, p ? DD_OK : E_OUTOFMEMORY, 3);
}

static void d_DirectDrawEnumerateA(cpu_t *c)
{
    uint32_t a[4] = {0, g_str_desc, g_str_name, ARG(1)};
    guest_callback(c, ARG(0), 4, a);
    hle_ret(c, DD_OK, 2);
}

/* ------------------------------------------------------------ IDirectDraw* */
static void dd_SetCooperativeLevel(cpu_t *c)
{
    dd_state *d = com_data(ARG(0), "ddraw");
    if (d) d->coop = (int)ARG(2);
    hle_ret(c, DD_OK, 3);
}

static void fill_caps(gptr p)
{
    uint32_t size = gmem_rd32(p) ? gmem_rd32(p) : 380;
    for (uint32_t k = 4; k < size; k += 4) gmem_wr32(p + k, 0);
    gmem_wr32(p + 0x04, 0x00000001u | 0x00000040u | 0x04000000u | 0x00400000u);  /* 3D, BLT, COLORFILL, COLORKEY */
    gmem_wr32(p + 0x0c, 0x00000200u);                       /* CKEYCAPS: SRCBLT */
    gmem_wr32(p + 0x38, 0x00000400u);                       /* z-buffer: 16 bit */
    gmem_wr32(p + 0x3c, 8u << 20);
    gmem_wr32(p + 0x40, 8u << 20);
    if (size >= 0x6c) gmem_wr32(p + 0x68, DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | DDSCAPS_ZBUFFER |
                                          DDSCAPS_FLIP | DDSCAPS_PRIMARYSURFACE | DDSCAPS_VIDEOMEMORY);
}
static void dd_GetCaps(cpu_t *c)
{
    if (ARG(1)) fill_caps(ARG(1));
    if (ARG(2)) fill_caps(ARG(2));
    hle_ret(c, DD_OK, 3);
}

/* EnumDisplayModes(this, flags, desc, ctx, cb) */
static void enum_modes(cpu_t *c, uint32_t size)
{
    /* 640x480 always comes first: the game's mode lookup (FUN_0046bbb5) returns the index
     * and its caller (FUN_0046be3d) treats index 0 as "not found" for any other size. */
    const uint32_t modes[][2] = {{640, 480}, {g_screen_w, g_screen_h}};
    unsigned nmodes = g_screen_w == 640 && g_screen_h == 480 ? 1u : 2u;
    gptr d = gheap_alloc(size);
    for (unsigned m = 0; m < nmodes; m++) {
        for (uint32_t k = 0; k < size; k += 4) gmem_wr32(d + k, 0);
        gmem_wr32(d, size);
        gmem_wr32(d + 4, 0x2 | 0x4 | 0x8 | 0x1000 | 0x40000);    /* +REFRESHRATE */
        gmem_wr32(d + 8, modes[m][1]);
        gmem_wr32(d + 12, modes[m][0]);
        gmem_wr32(d + 16, modes[m][0] * 2);
        gmem_wr32(d + 24, 60);
        dd_write_pixfmt(d + 72, &PF565);
        uint32_t a[2] = {d, ARG(3)};
        if (guest_callback(c, ARG(4), 2, a) == 0) break;     /* DDENUMRET_CANCEL */
    }
    gheap_free(d);
}
static void dd2_EnumDisplayModes(cpu_t *c) { enum_modes(c, 108); hle_ret(c, DD_OK, 5); }
static void dd4_EnumDisplayModes(cpu_t *c) { enum_modes(c, 124); hle_ret(c, DD_OK, 5); }

static void set_mode(cpu_t *c, int nargs)
{
    uint32_t w = ARG(1), h = ARG(2), bpp = ARG(3);
    pal_log("ddraw: SetDisplayMode %lux%lux%lu", (unsigned long)w, (unsigned long)h, (unsigned long)bpp);
    if (getenv("TPM_TRACE_OPEN")) {   /* debug: code addresses on the guest stack */
        char bt[200]; int bl = 0, k = 0;
        for (uint32_t o = 0; o < 0x600 && k < 12; o += 4) {
            uint32_t v = gmem_rd32(c->r[ESP] + o);
            if (v >= 0x401000u && v < 0x4a8000u) { bl += snprintf(bt + bl, sizeof bt - (size_t)bl, " %08lx", (unsigned long)v); k++; }
        }
        pal_log("ddraw:   stack%s", bt);
#ifdef CPU_FN_TRACE
        bl = 0;
        for (uint32_t k2 = 24; k2 >= 1 && bl < 190; k2--)
            bl += snprintf(bt + bl, sizeof bt - (size_t)bl, " %06lx", (unsigned long)(g_fn_ring[(g_fn_ring_pos - k2) & 63u] & 0xffffff));
        pal_log("ddraw:   fns%s", bt);
#endif
    }
    if (bpp != 16 || w > VIDEO_MAX_W || h > VIDEO_MAX_H) { hle_ret(c, DDERR_UNSUPPORTED, nargs); return; }
    g_mode_w = w; g_mode_h = h; g_mode_bpp = bpp;
    hle_ret(c, DD_OK, nargs);
}
static void dd1_SetDisplayMode(cpu_t *c) { set_mode(c, 4); }
static void dd2_SetDisplayMode(cpu_t *c) { set_mode(c, 6); }
static void dd_RestoreDisplayMode(cpu_t *c) { hle_ret(c, DD_OK, 1); }
static void dd_FlipToGDISurface(cpu_t *c) { hle_ret(c, DD_OK, 1); }
static void dd_WaitForVerticalBlank(cpu_t *c) { hle_ret(c, DD_OK, 3); }
static void dd_TestCooperativeLevel(cpu_t *c) { hle_ret(c, DD_OK, 1); }
static void dd_RestoreAllSurfaces(cpu_t *c) { hle_ret(c, DD_OK, 1); }

static void get_mode(cpu_t *c, uint32_t size)
{
    dd_surface tmp = {0};
    tmp.w = g_mode_w; tmp.h = g_mode_h; tmp.pitch = g_mode_w * 2; tmp.pf = PF565;
    write_desc(ARG(1), &tmp, size);
    gmem_wr32(ARG(1) + 4, 0x2 | 0x4 | 0x8 | 0x1000);
    hle_ret(c, DD_OK, 2);
}
static void dd2_GetDisplayMode(cpu_t *c) { get_mode(c, 108); }
static void dd4_GetDisplayMode(cpu_t *c) { get_mode(c, 124); }

/* GetAvailableVidMem(this, caps, total, free) */
static void dd_GetAvailableVidMem(cpu_t *c)
{
    if (ARG(2)) gmem_wr32(ARG(2), 8u << 20);
    if (ARG(3)) gmem_wr32(ARG(3), 8u << 20);
    hle_ret(c, DD_OK, 4);
}

/* GetDeviceIdentifier(this, DDDEVICEIDENTIFIER*, flags) */
static void dd_GetDeviceIdentifier(cpu_t *c)
{
    gptr p = ARG(1);
    for (uint32_t k = 0; k < 1064; k += 4) gmem_wr32(p + k, 0);
    hle_str_out(p, 512, "tpm3ds.drv");
    hle_str_out(p + 512, 512, "Phantom Menace HLE renderer");
    hle_ret(c, DD_OK, 3);
}

/* CreateSurface(this, desc, out, unk) */
static void create_surface(cpu_t *c, uint32_t size)
{
    gptr d = ARG(1);
    uint32_t flags = gmem_rd32(d + 4), caps = gmem_rd32(d + 104), caps2 = size >= 124 ? gmem_rd32(d + 108) : 0;
    uint32_t w = (flags & 4) ? gmem_rd32(d + 12) : g_mode_w, h = (flags & 2) ? gmem_rd32(d + 8) : g_mode_h;
    pixfmt pf = PF565;
    if (flags & 0x1000) pf_read(d + 72, &pf);
    if (caps & DDSCAPS_ZBUFFER) {
        uint32_t zb = (flags & 0x40) ? gmem_rd32(d + 24) : pf.bits;     /* DDSD_ZBUFFERBITDEPTH */
        pf = (pixfmt){DDPF_ZBUFFER, zb ? zb : 16, 0, 0, 0, 0};
        pf.g = (pf.bits == 16) ? 0xffff : 0xffffff;                        /* z mask lives in dwZBitMask (+20) */
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) { w = g_mode_w; h = g_mode_h; pf = PF565; }
    gptr s = new_surface(w, h, &pf, caps | ((caps & DDSCAPS_SYSTEMMEMORY) ? 0 : DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM), caps2);
    if (!s) { hle_ret(c, E_OUTOFMEMORY, 4); return; }
    dd_surface *sd = dd_surface_of(s);
    if (flags & 0x10000) { sd->has_ck = 1; sd->ck_lo = gmem_rd32(d + 64); sd->ck_hi = gmem_rd32(d + 68); }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        g_primary = s;
        sd->caps |= DDSCAPS_FRONTBUFFER;
        if ((flags & 0x20) && gmem_rd32(d + 20) >= 1) {             /* back buffers */
            uint32_t bc = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_COMPLEX)) | DDSCAPS_BACKBUFFER;
            sd->back = new_surface(w, h, &pf, bc | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM, 0);
        }
    }
    if ((caps & DDSCAPS_MIPMAP) && (flags & 0x20000)) {                /* mip chain */
        uint32_t levels = gmem_rd32(d + 24);
        dd_surface *cur = sd;
        for (uint32_t l = 1; l < levels && (w > 1 || h > 1); l++) {
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
            cur->mip = new_surface(w, h, &pf, sd->caps, caps2);
            if (!cur->mip) break;
            cur = dd_surface_of(cur->mip);
        }
    }
    pal_log("ddraw: CreateSurface %lux%lu %lu-bit caps %08lx -> %08lx", (unsigned long)sd->w, (unsigned long)sd->h,
            (unsigned long)sd->pf.bits, (unsigned long)sd->caps, (unsigned long)s);
    gmem_wr32(ARG(2), s);
    hle_ret(c, DD_OK, 4);
}
static void dd2_CreateSurface(cpu_t *c) { create_surface(c, 108); }
static void dd4_CreateSurface(cpu_t *c) { create_surface(c, 124); }

/* CreatePalette(this, flags, entries[], out, unk) */
static const com_iface *const palette_ifaces[] = {&IF_Palette, NULL};
static void dd_CreatePalette(cpu_t *c)
{
    dd_palette *p = calloc(1, sizeof *p);
    p->caps = ARG(1);
    uint32_t n = (ARG(1) & 0x4u) ? 256 : (ARG(1) & 0x2u) ? 16 : (ARG(1) & 0x1u) ? 4 : 256;   /* DDPCAPS_8BIT/4BIT/2BIT */
    for (uint32_t k = 0; k < n && ARG(2); k++) p->entries[k] = gmem_rd32(ARG(2) + 4 * k);
    gptr o = com_create("palette", p, free, palette_ifaces);
    gmem_wr32(ARG(3), o);
    hle_ret(c, DD_OK, 4);
}

static const com_iface *const clipper_ifaces[] = {&IF_Clipper, NULL};
static void dd_CreateClipper(cpu_t *c)
{
    gmem_wr32(ARG(2), com_create("clipper", NULL, NULL, clipper_ifaces));
    hle_ret(c, DD_OK, 4);
}

/* ------------------------------------------------------------ IDirectDrawSurface4 */
#define SURF(var) dd_surface *var = dd_surface_of(ARG(0)); if (!var) { hle_ret(c, DDERR_INVALIDPARAMS, NARGS); return; }

static void sf_GetSurfaceDesc(cpu_t *c)
{
#define NARGS 2
    SURF(s);
    uint32_t size = gmem_rd32(ARG(1));
    write_desc(ARG(1), s, size == 108 ? 108 : 124);
    hle_ret(c, DD_OK, 2);
#undef NARGS
}

/* Lock(this, rect, desc, flags, event) */
static void sf_Lock(cpu_t *c)
{
#define NARGS 5
    SURF(s);
    r3d_gpu_sync(s);
    uint32_t size = gmem_rd32(ARG(2));
    write_desc(ARG(2), s, size == 108 ? 108 : 124);
    gptr base = s->pixels;
    if (ARG(1)) {
        uint32_t x = gmem_rd32(ARG(1)), y = gmem_rd32(ARG(1) + 4);
        base += y * s->pitch + x * (s->pf.bits / 8);
    }
    gmem_wr32(ARG(2) + 36, base);
    gmem_wr32(ARG(2) + 4, gmem_rd32(ARG(2) + 4) | 0x800);       /* DDSD_LPSURFACE */
    s->locked++;
    hle_ret(c, DD_OK, 5);
#undef NARGS
}

static void present(dd_surface *s);
static uint32_t g_prim_presented;

static void sf_Unlock(cpu_t *c)
{
#define NARGS 2
    SURF(s);
    {
        static int n;   /* diagnostic: does the presented movie picture change? */
        if (n < 600 && (s->caps & DDSCAPS_PRIMARYSURFACE)) {
            uint32_t fs = 0, i;
            const uint8_t *q = gmem_span(s->pixels, s->pitch * s->h, 0);
            if (q) for (i = 0; i < s->pitch * s->h; i += 1024u) fs += q[i];
            n++;
            { static uint32_t lastfs; if (n < 40 || fs != lastfs) pal_log("pf %d sum=%u", n, (unsigned)fs); lastfs = fs; }
        }
    }
    /* A polling point, like Flip: the movie player only ever locks and unlocks the primary, so
     * without this the guest thread that decodes the frame never gets the CPU and Bink's wait
     * never comes due - the intro movies sat frozen on their first frame. */
    gthread_yield();
    if (s->locked) s->locked--;
    s->version++;
    /* A locked primary is scanned out directly on the PC, so the video player writes
     * its frames straight into it and never flips. Present it here or those frames
     * stay in guest memory and the intro movies show nothing. */
    if ((s->caps & DDSCAPS_PRIMARYSURFACE) && s->version != g_prim_presented) {
        extern int video_present_silent;
        g_prim_presented = s->version;
        video_present_silent = 1;          /* show the pixels, but do not count a frame or
                                            * advance the clock: the game's own loop does that */
        /* Always the CPU blit: the decoder wrote the frame into guest memory, and a movie's
         * left-aligned picture has to be cropped and scaled to fill the screen - the GPU
         * transfer would move the whole 800x480 surface unchanged. video.c decides from the
         * frame itself whether it is a movie (a wide black right margin). */
        /* Present through the GPU when it can (the intro movies are drawn as textures into the
         * render target and reach the screen that way). If the GPU has nothing, only blit the CPU
         * copy when this surface actually holds something: during a movie it is an 800x480 buffer of
         * zeros that the game never locks, and blitting that on top of the movie frame is what made
         * the intro black. */
        if (!r3d_gpu_present(s)) {
            int has = 0; uint32_t i; const uint8_t *q = gmem_span(s->pixels, s->pitch * s->h, 0);
            if (q) for (i = 0; i < s->pitch * s->h; i += 512u) if (q[i]) { has = 1; break; }
            if (has) present(s);
        }
        video_present_silent = 0;
    }
    hle_ret(c, DD_OK, 2);
#undef NARGS
}

static uint32_t px_rd(const dd_surface *s, uint32_t x, uint32_t y)
{
    gptr a = s->pixels + y * s->pitch;
    switch (s->pf.bits) {
    case 8:  return gmem_rd8(a + x);
    case 32: return gmem_rd32(a + 4 * x);
    case 24: return gmem_rd16(a + 3 * x) | (uint32_t)gmem_rd8(a + 3 * x + 2) << 16;
    default: return gmem_rd16(a + 2 * x);
    }
}
static void px_wr(dd_surface *s, uint32_t x, uint32_t y, uint32_t v)
{
    gptr a = s->pixels + y * s->pitch;
    switch (s->pf.bits) {
    case 8:  gmem_wr8(a + x, (uint8_t)v); break;
    case 32: gmem_wr32(a + 4 * x, v); break;
    case 24: gmem_wr16(a + 3 * x, (uint16_t)v); gmem_wr8(a + 3 * x + 2, (uint8_t)(v >> 16)); break;
    default: gmem_wr16(a + 2 * x, (uint16_t)v); break;
    }
}

typedef struct { int32_t l, t, r, b; } rect;
static rect rd_rect(gptr p, const dd_surface *s)
{
    if (!p) return (rect){0, 0, (int32_t)s->w, (int32_t)s->h};
    return (rect){(int32_t)gmem_rd32(p), (int32_t)gmem_rd32(p + 4), (int32_t)gmem_rd32(p + 8), (int32_t)gmem_rd32(p + 12)};
}

static void fill(dd_surface *d, rect r, uint32_t v)
{
    r3d_gpu_sync(d);
    if (r.l < 0) r.l = 0;
    if (r.t < 0) r.t = 0;
    if (r.r > (int32_t)d->w) r.r = (int32_t)d->w;
    if (r.b > (int32_t)d->h) r.b = (int32_t)d->h;
    for (int32_t y = r.t; y < r.b; y++) {
        if (d->pf.bits == 16) {
            uint16_t *row = (uint16_t *)gmem_span(d->pixels + (uint32_t)y * d->pitch + 2u * (uint32_t)r.l, 2u * (uint32_t)(r.r - r.l), 1);
            if (row) for (int32_t x = 0; x < r.r - r.l; x++) row[x] = (uint16_t)v;
        } else {
            for (int32_t x = r.l; x < r.r; x++) px_wr(d, (uint32_t)x, (uint32_t)y, v);
        }
    }
    d->version++;
}

/* Stretching copy with optional source colour key; clipped to both surfaces. */
static void blit(dd_surface *d, rect dr, dd_surface *s, rect sr, int use_ck)
{
    r3d_gpu_sync(d);
    r3d_gpu_sync(s);
    int32_t dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, sh = sr.b - sr.t;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int same = dw == sw && dh == sh && d->pf.bits == 16 && s->pf.bits == 16 && !use_ck;
    for (int32_t y = 0; y < dh; y++) {
        int32_t dy = dr.t + y, sy = sr.t + (int32_t)((int64_t)y * sh / dh);
        if (dy < 0 || dy >= (int32_t)d->h || sy < 0 || sy >= (int32_t)s->h) continue;
        if (same && dr.l >= 0 && dr.r <= (int32_t)d->w && sr.l >= 0 && sr.r <= (int32_t)s->w) {
            uint8_t *dp = gmem_span(d->pixels + (uint32_t)dy * d->pitch + 2u * (uint32_t)dr.l, 2u * (uint32_t)dw, 1);
            uint8_t *sp = gmem_span(s->pixels + (uint32_t)sy * s->pitch + 2u * (uint32_t)sr.l, 2u * (uint32_t)sw, 0);
            if (dp && sp) memmove(dp, sp, 2u * (uint32_t)dw);
            continue;
        }
        for (int32_t x = 0; x < dw; x++) {
            int32_t dx = dr.l + x, sx = sr.l + (int32_t)((int64_t)x * sw / dw);
            if (dx < 0 || dx >= (int32_t)d->w || sx < 0 || sx >= (int32_t)s->w) continue;
            uint32_t v = px_rd(s, (uint32_t)sx, (uint32_t)sy);
            if (use_ck && v >= s->ck_lo && v <= s->ck_hi) continue;
            px_wr(d, (uint32_t)dx, (uint32_t)dy, v);
        }
    }
    d->version++;
}

/* Blt(this, destrect, src, srcrect, flags, fx) */
static void sf_Blt(cpu_t *c)
{
#define NARGS 6
    SURF(d);
    uint32_t flags = ARG(4);
    rect dr = rd_rect(ARG(1), d);
    if (flags & (0x400u | 0x2000000u)) {                    /* COLORFILL / DEPTHFILL */
        fill(d, dr, gmem_rd32(ARG(5) + 0x50));
        hle_ret(c, DD_OK, 6);
        return;
    }
    dd_surface *s = dd_surface_of(ARG(2));
    if (!s) { pal_log("ddraw: Blt flags %08lx without source", (unsigned long)flags); hle_ret(c, DDERR_INVALIDPARAMS, 6); return; }
    blit(d, dr, s, rd_rect(ARG(3), s), (flags & 0x8000u) && s->has_ck);
    hle_ret(c, DD_OK, 6);
#undef NARGS
}

/* BltFast(this, x, y, src, srcrect, trans) */
static void sf_BltFast(cpu_t *c)
{
#define NARGS 6
    SURF(d);
    dd_surface *s = dd_surface_of(ARG(3));
    if (!s) { hle_ret(c, DDERR_INVALIDPARAMS, 6); return; }
    rect sr = rd_rect(ARG(4), s);
    rect dr = {(int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(1) + sr.r - sr.l, (int32_t)ARG(2) + sr.b - sr.t};
    blit(d, dr, s, sr, (ARG(5) & 1u) && s->has_ck);
    hle_ret(c, DD_OK, 6);
#undef NARGS
}

static void present(dd_surface *s)
{
    dd_palette *pal = s->palette ? dd_palette_of(s->palette) : NULL;
    video_present(s->pixels, s->w, s->h, s->pitch, s->pf.bits, s->pf.g, pal ? pal->entries : NULL);
}

/* Flip(this, override, flags): front and back exchange their memory. */
static void sf_Flip(cpu_t *c)
{
    gthread_yield();                /* a polling point: other guest threads run here */
#define NARGS 3
    SURF(f);
    dd_surface *b = dd_surface_of(ARG(1) ? ARG(1) : f->back);
    int gpu = 0;
    if (b) {
        /* present from the GPU target (staging) and leave the guest copy pending */
        gpu = r3d_gpu_present(b);
        gptr t = f->pixels; f->pixels = b->pixels; b->pixels = t;
        f->version++; b->version++;
        r3d_gpu_flip(f, b);
    }
    if (!gpu) {
        r3d_gpu_sync(f);
        present(f);
    }
    guest_watchdog(c);
    hle_ret(c, DD_OK, 3);
#undef NARGS
}

/* GetAttachedSurface(this, DDSCAPS2*, out) */
static void sf_GetAttachedSurface(cpu_t *c)
{
#define NARGS 3
    SURF(s);
    uint32_t want = gmem_rd32(ARG(1));
    gptr r = 0;
    if ((want & DDSCAPS_BACKBUFFER) && s->back) r = s->back;
    else if ((want & DDSCAPS_ZBUFFER) && s->zbuf) r = s->zbuf;
    else if ((want & DDSCAPS_MIPMAP) && s->mip) r = s->mip;
    if (!r) { pal_log("ddraw: GetAttachedSurface caps %08lx -> not found", (unsigned long)want); hle_ret(c, DDERR_NOTFOUND, 3); return; }
    com_addref(r);
    gmem_wr32(ARG(2), r);
    hle_ret(c, DD_OK, 3);
#undef NARGS
}

static void sf_AddAttachedSurface(cpu_t *c)
{
#define NARGS 2
    SURF(s);
    dd_surface *a = dd_surface_of(ARG(1));
    if (!a) { hle_ret(c, DDERR_INVALIDPARAMS, 2); return; }
    com_addref(ARG(1));
    if (a->caps & DDSCAPS_ZBUFFER) { if (s->zbuf) com_release(s->zbuf); s->zbuf = ARG(1); }
    else { if (s->back) com_release(s->back); s->back = ARG(1); }
    hle_ret(c, DD_OK, 2);
#undef NARGS
}

static void sf_DeleteAttachedSurface(cpu_t *c)
{
#define NARGS 3
    SURF(s);
    gptr a = ARG(2);
    if (s->zbuf && (!a || a == s->zbuf)) { com_release(s->zbuf); s->zbuf = 0; }
    else if (s->back && (!a || a == s->back)) { com_release(s->back); s->back = 0; }
    hle_ret(c, DD_OK, 3);
#undef NARGS
}

/* SetColorKey(this, flags, DDCOLORKEY*) */
static void sf_SetColorKey(cpu_t *c)
{
#define NARGS 3
    SURF(s);
    if (ARG(1) & 0x8u) {                                     /* DDCKEY_SRCBLT */
        s->has_ck = ARG(2) != 0;
        if (ARG(2)) { s->ck_lo = gmem_rd32(ARG(2)); s->ck_hi = gmem_rd32(ARG(2) + 4); }
        s->version++;
    }
    hle_ret(c, DD_OK, 3);
#undef NARGS
}
static void sf_GetColorKey(cpu_t *c)
{
#define NARGS 3
    SURF(s);
    if (!s->has_ck) { hle_ret(c, DDERR_NOCOLORKEY, 3); return; }
    gmem_wr32(ARG(2), s->ck_lo);
    gmem_wr32(ARG(2) + 4, s->ck_hi);
    hle_ret(c, DD_OK, 3);
#undef NARGS
}
static void sf_SetPalette(cpu_t *c)
{
#define NARGS 2
    SURF(s);
    if (ARG(1)) com_addref(ARG(1));
    if (s->palette) com_release(s->palette);
    s->palette = ARG(1);
    s->version++;
    hle_ret(c, DD_OK, 2);
#undef NARGS
}
static void sf_GetPalette(cpu_t *c)
{
#define NARGS 2
    SURF(s);
    if (!s->palette) { hle_ret(c, DDERR_NOPALETTEATTACHED, 2); return; }
    com_addref(s->palette);
    gmem_wr32(ARG(1), s->palette);
    hle_ret(c, DD_OK, 2);
#undef NARGS
}
static void sf_GetPixelFormat(cpu_t *c)
{
#define NARGS 2
    SURF(s);
    dd_write_pixfmt(ARG(1), &s->pf);
    hle_ret(c, DD_OK, 2);
#undef NARGS
}
static void sf_GetCaps(cpu_t *c)
{
#define NARGS 2
    SURF(s);
    gmem_wr32(ARG(1), s->caps);
    gmem_wr32(ARG(1) + 4, s->caps2);
    gmem_wr32(ARG(1) + 8, 0);
    gmem_wr32(ARG(1) + 12, 0);
    hle_ret(c, DD_OK, 2);
#undef NARGS
}
static void sf_ok1(cpu_t *c) { hle_ret(c, DD_OK, 1); }
static void sf_ok2(cpu_t *c) { hle_ret(c, DD_OK, 2); }
static void sf_SetClipper(cpu_t *c) { hle_ret(c, DD_OK, 2); }
static void sf_GetDDInterface(cpu_t *c)
{
    pal_log("ddraw: GetDDInterface not supported");
    hle_ret(c, DDERR_UNSUPPORTED, 2);
}

/* ------------------------------------------------------------ palette */
/* GetEntries/SetEntries(this, flags, start, count, entries) */
static void pl_GetEntries(cpu_t *c)
{
    dd_palette *p = dd_palette_of(ARG(0));
    for (uint32_t k = 0; p && k < ARG(3) && ARG(2) + k < 256; k++) gmem_wr32(ARG(4) + 4 * k, p->entries[ARG(2) + k]);
    hle_ret(c, p ? DD_OK : DDERR_INVALIDPARAMS, 5);
}
static void pl_SetEntries(cpu_t *c)
{
    dd_palette *p = dd_palette_of(ARG(0));
    for (uint32_t k = 0; p && k < ARG(3) && ARG(2) + k < 256; k++) p->entries[ARG(2) + k] = gmem_rd32(ARG(4) + 4 * k);
    if (p && g_primary) {
        dd_surface *ps = dd_surface_of(g_primary);
        if (ps && ps->palette == ARG(0) && ps->pf.bits == 8) present(ps);
    }
    hle_ret(c, p ? DD_OK : DDERR_INVALIDPARAMS, 5);
}

/* ------------------------------------------------------------ vtables */
#define DD1_METHODS(SetDisplayMode, CreateSurface, EnumDisplayModes, GetDisplayMode) \
    COM_IUNKNOWN, {"Compact", NULL}, {"CreateClipper", dd_CreateClipper}, {"CreatePalette", dd_CreatePalette}, \
    {"CreateSurface", CreateSurface}, {"DuplicateSurface", NULL}, {"EnumDisplayModes", EnumDisplayModes}, \
    {"EnumSurfaces", NULL}, {"FlipToGDISurface", dd_FlipToGDISurface}, {"GetCaps", dd_GetCaps}, \
    {"GetDisplayMode", GetDisplayMode}, {"GetFourCCCodes", NULL}, {"GetGDISurface", NULL}, \
    {"GetMonitorFrequency", NULL}, {"GetScanLine", NULL}, {"GetVerticalBlankStatus", NULL}, {"Initialize", NULL}, \
    {"RestoreDisplayMode", dd_RestoreDisplayMode}, {"SetCooperativeLevel", dd_SetCooperativeLevel}, \
    {"SetDisplayMode", SetDisplayMode}, {"WaitForVerticalBlank", dd_WaitForVerticalBlank}

static const com_method m_dd1[] = {DD1_METHODS(dd1_SetDisplayMode, dd2_CreateSurface, dd2_EnumDisplayModes, dd2_GetDisplayMode)};
static const com_method m_dd2[] = {DD1_METHODS(dd2_SetDisplayMode, dd2_CreateSurface, dd2_EnumDisplayModes, dd2_GetDisplayMode),
                                   {"GetAvailableVidMem", dd_GetAvailableVidMem}};
static const com_method m_dd4[] = {DD1_METHODS(dd2_SetDisplayMode, dd4_CreateSurface, dd4_EnumDisplayModes, dd4_GetDisplayMode),
                                   {"GetAvailableVidMem", dd_GetAvailableVidMem}, {"GetSurfaceFromDC", NULL},
                                   {"RestoreAllSurfaces", dd_RestoreAllSurfaces}, {"TestCooperativeLevel", dd_TestCooperativeLevel},
                                   {"GetDeviceIdentifier", dd_GetDeviceIdentifier}};

static const com_method m_surf4[] = {
    COM_IUNKNOWN, {"AddAttachedSurface", sf_AddAttachedSurface}, {"AddOverlayDirtyRect", NULL}, {"Blt", sf_Blt},
    {"BltBatch", NULL}, {"BltFast", sf_BltFast}, {"DeleteAttachedSurface", sf_DeleteAttachedSurface},
    {"EnumAttachedSurfaces", NULL}, {"EnumOverlayZOrders", NULL}, {"Flip", sf_Flip},
    {"GetAttachedSurface", sf_GetAttachedSurface}, {"GetBltStatus", sf_ok2}, {"GetCaps", sf_GetCaps},
    {"GetClipper", NULL}, {"GetColorKey", sf_GetColorKey}, {"GetDC", NULL}, {"GetFlipStatus", sf_ok2},
    {"GetOverlayPosition", NULL}, {"GetPalette", sf_GetPalette}, {"GetPixelFormat", sf_GetPixelFormat},
    {"GetSurfaceDesc", sf_GetSurfaceDesc}, {"Initialize", NULL}, {"IsLost", sf_ok1}, {"Lock", sf_Lock},
    {"ReleaseDC", NULL}, {"Restore", sf_ok1}, {"SetClipper", sf_SetClipper}, {"SetColorKey", sf_SetColorKey},
    {"SetOverlayPosition", NULL}, {"SetPalette", sf_SetPalette}, {"Unlock", sf_Unlock}, {"UpdateOverlay", NULL},
    {"UpdateOverlayDisplay", NULL}, {"UpdateOverlayZOrder", NULL}, {"GetDDInterface", sf_GetDDInterface},
    {"PageLock", sf_ok2}, {"PageUnlock", sf_ok2}, {"SetSurfaceDesc", NULL}, {"SetPrivateData", NULL},
    {"GetPrivateData", NULL}, {"FreePrivateData", NULL}, {"GetUniquenessValue", NULL}, {"ChangeUniquenessValue", NULL},
};
static const com_method m_palette[] = {
    COM_IUNKNOWN, {"GetCaps", NULL}, {"GetEntries", pl_GetEntries}, {"Initialize", NULL}, {"SetEntries", pl_SetEntries},
};
static void cl_SetHWnd(cpu_t *c) { hle_ret(c, DD_OK, 3); }
static const com_method m_clipper[] = {
    COM_IUNKNOWN, {"GetClipList", NULL}, {"GetHWnd", NULL}, {"Initialize", NULL}, {"IsClipListChanged", NULL},
    {"SetClipList", NULL}, {"SetHWnd", cl_SetHWnd},
};

#define IFACE(var, name, iid, m) com_iface var = {name, iid, m, (int)(sizeof m / sizeof m[0]), 0}
IFACE(IF_DirectDraw, "IDirectDraw", 0x6c14db80u, m_dd1);
IFACE(IF_DirectDraw2, "IDirectDraw2", 0xb3a6f3e0u, m_dd2);
IFACE(IF_DirectDraw4, "IDirectDraw4", 0x9c59509au, m_dd4);
IFACE(IF_Surface4, "IDirectDrawSurface4", 0x0b2b8630u, m_surf4);
IFACE(IF_Palette, "IDirectDrawPalette", 0x6c14db84u, m_palette);
IFACE(IF_Clipper, "IDirectDrawClipper", 0x6c14db85u, m_clipper);

static const hle_def defs[] = {
    {"DDRAW.dll", "DirectDrawCreate", d_DirectDrawCreate},
    {"DDRAW.dll", "DirectDrawEnumerateA", d_DirectDrawEnumerateA},
};

void hle_ddraw_init(void)
{
    g_primary = 0;
    g_mode_w = g_screen_w; g_mode_h = g_screen_h; g_mode_bpp = 16;
    hle_register(defs, (int)(sizeof defs / sizeof defs[0]));
    com_register(&IF_DirectDraw);
    com_register(&IF_DirectDraw2);
    com_register(&IF_DirectDraw4);
    com_register(&IF_Surface4);
    com_register(&IF_Palette);
    com_register(&IF_Clipper);
}

void hle_ddraw_process_init(void)
{
    static const char desc[] = "Primary Display Driver", name[] = "display";
    g_str_desc = gheap_alloc(sizeof desc);
    hle_str_out(g_str_desc, sizeof desc, desc);
    g_str_name = gheap_alloc(sizeof name);
    hle_str_out(g_str_name, sizeof name, name);
}
