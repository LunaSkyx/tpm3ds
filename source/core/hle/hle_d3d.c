/* Direct3D (DX6) HLE: IDirect3D3 (on the DirectDraw object), IDirect3DDevice3,
 * IDirect3DViewport3, IDirect3DTexture2 (on surfaces), IDirect3DMaterial3.
 * One device: "Direct3D HAL", 16-bit colour and z, textures up to 256x256. */
#include <stdlib.h>
#include <string.h>
#include "core/gheap.h"
#include "core/guest.h"
#include "core/hle/hle_com.h"
#include "core/hle/hle_ddraw.h"
#include "core/render/r3d.h"
#include "pal/pal.h"

#define D3D_OK 0u

extern com_iface IF_Direct3D3, IF_Device3, IF_Viewport3, IF_Texture2, IF_Material3;

static const pixfmt TEXFMT[] = {
    {DDPF_RGB, 16, 0xf800, 0x07e0, 0x001f, 0},
    {DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7c00, 0x03e0, 0x001f, 0x8000},
    {DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0f00, 0x00f0, 0x000f, 0xf000},
};
static const pixfmt ZFMT = {DDPF_ZBUFFER, 16, 0, 0xffff, 0, 0};

static float fl(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* ------------------------------------------------------------ device description */
static void write_primcaps(gptr p)
{
    gmem_wr32(p + 0x00, 56);
    gmem_wr32(p + 0x04, 0x00000002u | 0x00000010u | 0x00000020u | 0x00000040u);   /* MASKZ, CULLNONE, CULLCW, CULLCCW */
    gmem_wr32(p + 0x08, 0x00000001u | 0x00000010u | 0x00000080u | 0x00000100u | 0x00001000u | 0x00100000u);   /* + WFOG */   /* DITHER ZTEST FOGVERTEX FOGTABLE ... */
    gmem_wr32(p + 0x0c, 0xffu);
    gmem_wr32(p + 0x10, 0x1fffu);
    gmem_wr32(p + 0x14, 0x1fffu);
    gmem_wr32(p + 0x18, 0xffu);
    gmem_wr32(p + 0x1c, 0x00000008u | 0x00000200u | 0x00004000u | 0x00080000u);
    gmem_wr32(p + 0x20, 0x00000001u | 0x00000002u | 0x00000004u | 0x00000008u);   /* PERSPECTIVE POW2 ALPHA TRANSPARENCY */
    gmem_wr32(p + 0x24, 0x0303033fu);
    gmem_wr32(p + 0x28, 0xffu);
    gmem_wr32(p + 0x2c, 0x17u);
}

static void write_devdesc(gptr p, int hal)
{
    for (uint32_t k = 0; k < 252; k += 4) gmem_wr32(p + k, 0);
    gmem_wr32(p, 252);
    if (!hal) return;
    gmem_wr32(p + 0x04, 0x000007ffu);
    gmem_wr32(p + 0x08, 2);                                   /* D3DCOLOR_RGB */
    gmem_wr32(p + 0x0c, 0x1u | 0x10u | 0x40u | 0x200u | 0x400u | 0x800u | 0x2000u | 0x80000u);
    gmem_wr32(p + 0x10, 8);
    gmem_wr32(p + 0x18, 1);
    gmem_wr32(p + 0x1c, 16);
    write_primcaps(p + 0x2c);
    write_primcaps(p + 0x64);
    gmem_wr32(p + 0x9c, 0x400);                               /* DDBD_16 */
    gmem_wr32(p + 0xa0, 0x400);
    gmem_wr32(p + 0xa8, 0xffff);
    gmem_wr32(p + 0xac, 1);
    gmem_wr32(p + 0xb0, 1);
    gmem_wr32(p + 0xb4, 256);
    gmem_wr32(p + 0xb8, 256);
    gmem_wr32(p + 0xcc, 256);
    gmem_wr32(p + 0xd0, 8);
    gmem_wr32(p + 0xf0, 1);
    gmem_wr32(p + 0xf4, 0x5fu);
    gmem_wr16(p + 0xf8, 2);
    gmem_wr16(p + 0xfa, 1);
}

/* ------------------------------------------------------------ IDirect3D3 */
static gptr g_hal_guid, g_str_desc, g_str_name;

static void d3_EnumDevices(cpu_t *c)
{
    gptr hw = gheap_alloc(252), hel = gheap_alloc(252);
    write_devdesc(hw, 1);
    write_devdesc(hel, 0);
    uint32_t a[6] = {g_hal_guid, g_str_desc, g_str_name, hw, hel, ARG(2)};
    guest_callback(c, ARG(1), 6, a);
    gheap_free(hw);
    gheap_free(hel);
    hle_ret(c, D3D_OK, 3);
}

/* EnumZBufferFormats(this, clsid, cb, ctx) */
static void d3_EnumZBufferFormats(cpu_t *c)
{
    gptr pf = gheap_alloc(32);
    dd_write_pixfmt(pf, &ZFMT);
    uint32_t a[2] = {pf, ARG(3)};
    guest_callback(c, ARG(2), 2, a);
    gheap_free(pf);
    hle_ret(c, D3D_OK, 4);
}

typedef struct { gptr target, viewport, d3d; } d3d_device;
static const com_iface *const device_ifaces[] = {&IF_Device3, NULL};
static gptr g_device;

static void device_dtor(void *p)
{
    d3d_device *d = p;
    if (d->target) com_release(d->target);
    g_device = 0;
    free(d);
}

static void bind_target(d3d_device *d)
{
    dd_surface *t = dd_surface_of(d->target);
    r3d_set_target(t, t && t->zbuf ? dd_surface_of(t->zbuf) : NULL);
}

/* CreateDevice(this, clsid, surface, out, unk) */
static void d3_CreateDevice(cpu_t *c)
{
    if (!dd_surface_of(ARG(2))) { hle_ret(c, E_INVALIDARG, 5); return; }
    d3d_device *d = calloc(1, sizeof *d);
    d->target = ARG(2);
    d->d3d = ARG(0);
    com_addref(d->target);
    r3d_reset();
    bind_target(d);
    g_device = com_create("device", d, device_dtor, device_ifaces);
    pal_log("d3d: CreateDevice on surface %08lx -> %08lx", (unsigned long)ARG(2), (unsigned long)g_device);
    gmem_wr32(ARG(3), g_device);
    hle_ret(c, D3D_OK, 5);
}

typedef struct { r3d_viewport vp; uint32_t bg_material; } d3d_viewport;
static const com_iface *const viewport_ifaces[] = {&IF_Viewport3, NULL};
static void d3_CreateViewport(cpu_t *c)
{
    gmem_wr32(ARG(1), com_create("viewport", calloc(1, sizeof(d3d_viewport)), free, viewport_ifaces));
    hle_ret(c, D3D_OK, 3);
}

typedef struct { uint32_t data[17]; uint32_t handle; } d3d_material;   /* D3DMATERIAL is 68 bytes */
static const com_iface *const material_ifaces[] = {&IF_Material3, NULL};
static uint32_t g_next_handle = 1;
static void d3_CreateMaterial(cpu_t *c)
{
    d3d_material *m = calloc(1, sizeof *m);
    m->handle = g_next_handle++;
    gmem_wr32(ARG(1), com_create("material", m, free, material_ifaces));
    hle_ret(c, D3D_OK, 3);
}
static void d3_EvictManagedTextures(cpu_t *c) { hle_ret(c, D3D_OK, 1); }

/* ------------------------------------------------------------ IDirect3DDevice3 */
#define DEV(var, n) d3d_device *var = com_data(ARG(0), "device"); if (!var) { hle_ret(c, E_INVALIDARG, n); return; }

static void dv_GetCaps(cpu_t *c)
{
    if (ARG(1)) write_devdesc(ARG(1), 1);
    if (ARG(2)) write_devdesc(ARG(2), 0);
    hle_ret(c, D3D_OK, 3);
}

/* EnumTextureFormats(this, cb, ctx) */
static void dv_EnumTextureFormats(cpu_t *c)
{
    gptr pf = gheap_alloc(32);
    for (unsigned k = 0; k < sizeof TEXFMT / sizeof TEXFMT[0]; k++) {
        dd_write_pixfmt(pf, &TEXFMT[k]);
        uint32_t a[2] = {pf, ARG(2)};
        if (guest_callback(c, ARG(1), 2, a) == 0) break;
    }
    gheap_free(pf);
    hle_ret(c, D3D_OK, 3);
}

static void dv_AddViewport(cpu_t *c)    { com_addref(ARG(1)); hle_ret(c, D3D_OK, 2); }
static void dv_DeleteViewport(cpu_t *c) { com_release(ARG(1)); hle_ret(c, D3D_OK, 2); }
static void dv_BeginScene(cpu_t *c)     { bind_target(com_data(ARG(0), "device")); r3d_begin_scene(); hle_ret(c, D3D_OK, 1); }
static void dv_EndScene(cpu_t *c)       { r3d_end_scene(); hle_ret(c, D3D_OK, 1); }

static void dv_SetCurrentViewport(cpu_t *c)
{
    DEV(d, 2);
    d3d_viewport *v = com_data(ARG(1), "viewport");
    if (v) { d->viewport = ARG(1); g_r3d.vp = v->vp; r3d_state_dirty(); }
    hle_ret(c, D3D_OK, 2);
}
static void dv_GetCurrentViewport(cpu_t *c)
{
    DEV(d, 2);
    if (d->viewport) com_addref(d->viewport);
    gmem_wr32(ARG(1), d->viewport);
    hle_ret(c, d->viewport ? D3D_OK : E_FAIL, 2);
}
static void dv_SetRenderTarget(cpu_t *c)
{
    DEV(d, 3);
    if (!dd_surface_of(ARG(1))) { hle_ret(c, E_INVALIDARG, 3); return; }
    com_addref(ARG(1));
    com_release(d->target);
    d->target = ARG(1);
    bind_target(d);
    hle_ret(c, D3D_OK, 3);
}
static void dv_GetRenderTarget(cpu_t *c)
{
    DEV(d, 2);
    com_addref(d->target);
    gmem_wr32(ARG(1), d->target);
    hle_ret(c, D3D_OK, 2);
}
static void dv_GetDirect3D(cpu_t *c)
{
    DEV(d, 2);
    com_addref(d->d3d);
    gmem_wr32(ARG(1), d->d3d);
    hle_ret(c, D3D_OK, 2);
}
static void dv_SetRenderState(cpu_t *c)
{
    uint32_t s = ARG(1);
    r3d_set_render_state(s, ARG(2));
    {
        extern int g_r3d_diag;
        static uint8_t seen[256][8];
        if (g_r3d_diag && s < 256 && ARG(2) < 8 ? !seen[s][ARG(2)] : (g_r3d_diag && s < 256 && !seen[s][7])) {
            if (ARG(2) < 8) seen[s][ARG(2)] = 1; else seen[s][7] = 1;
            pal_log("d3d diag: SetRenderState(%lu, %08lx)", (unsigned long)s, (unsigned long)ARG(2));
        }
    }
    hle_ret(c, D3D_OK, 3);
}
static void dv_GetRenderState(cpu_t *c)
{
    uint32_t s = ARG(1);
    gmem_wr32(ARG(2), s < 256 ? g_r3d.rs[s] : 0);
    hle_ret(c, D3D_OK, 3);
}
static void dv_SetLightState(cpu_t *c) { hle_ret(c, D3D_OK, 3); }

static float *transform(uint32_t type)
{
    switch (type) {
    case 1: return g_r3d.world;
    case 2: return g_r3d.view;
    case 3: return g_r3d.proj;
    default: return NULL;
    }
}
static void dv_SetTransform(cpu_t *c)
{
    float *m = transform(ARG(1));
    for (uint32_t k = 0; m && k < 16; k++) m[k] = fl(gmem_rd32(ARG(2) + 4 * k));
    if (m) g_r3d.xf_gen++;                 /* the GPU front end's world*view*proj cache key */
    if (m && ARG(1) == 3 && getenv("TPM_TRACE_VP")) {
        static int n;
        if (n++ % 2000 == 0) pal_log("d3d: proj %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
    hle_ret(c, m ? D3D_OK : E_INVALIDARG, 3);
}
static void dv_GetTransform(cpu_t *c)
{
    float *m = transform(ARG(1));
    for (uint32_t k = 0; m && k < 16; k++) { uint32_t u; memcpy(&u, &m[k], 4); gmem_wr32(ARG(2) + 4 * k, u); }
    hle_ret(c, m ? D3D_OK : E_INVALIDARG, 3);
}

/* DrawPrimitive(this, type, fvf, verts, count, flags) */
static void dv_DrawPrimitive(cpu_t *c)
{
    r3d_draw(ARG(1), ARG(2), ARG(3), ARG(4), 0, 0);
    hle_ret(c, D3D_OK, 6);
}
/* DrawIndexedPrimitive(this, type, fvf, verts, nverts, indices, nindices, flags) */
static void dv_DrawIndexedPrimitive(cpu_t *c)
{
    r3d_draw(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6));
    hle_ret(c, D3D_OK, 8);
}

/* SetTexture(this, stage, texture2) */
static void dv_SetTexture(cpu_t *c)
{
    uint32_t st = ARG(1);
    if (st < 2) g_r3d.tex[st] = ARG(2) ? dd_surface_of(ARG(2)) : NULL;
    hle_ret(c, D3D_OK, 3);
}
static void dv_SetTextureStageState(cpu_t *c)
{
    r3d_set_stage_state(ARG(1), ARG(2), ARG(3));
    hle_ret(c, D3D_OK, 4);
}
static void dv_GetTextureStageState(cpu_t *c)
{
    gmem_wr32(ARG(3), ARG(1) < 2 && ARG(2) < 32 ? g_r3d.tss[ARG(1)][ARG(2)] : 0);
    hle_ret(c, D3D_OK, 4);
}
static void dv_ValidateDevice(cpu_t *c) { if (ARG(1)) gmem_wr32(ARG(1), 1); hle_ret(c, D3D_OK, 2); }
static void dv_GetStats(cpu_t *c)
{
    gptr p = ARG(1);
    gmem_wr32(p + 4, g_r3d.tris);
    hle_ret(c, D3D_OK, 2);
}

/* ------------------------------------------------------------ IDirect3DViewport3 */
#define VP(var, n) d3d_viewport *var = com_data(ARG(0), "viewport"); if (!var) { hle_ret(c, E_INVALIDARG, n); return; }

static void vp_apply(gptr self, d3d_viewport *v)
{
    d3d_device *d = g_device ? com_data(g_device, "device") : NULL;
    if (d && d->viewport == self) { g_r3d.vp = v->vp; r3d_state_dirty(); }
}
static void vp_SetViewport(cpu_t *c)        /* D3DVIEWPORT */
{
    VP(v, 2);
    gptr p = ARG(1);
    v->vp = (r3d_viewport){gmem_rd32(p + 4), gmem_rd32(p + 8), gmem_rd32(p + 12), gmem_rd32(p + 16),
                           fl(gmem_rd32(p + 36)), fl(gmem_rd32(p + 40))};
    vp_apply(ARG(0), v);
    hle_ret(c, D3D_OK, 2);
}
static void vp_SetViewport2(cpu_t *c)       /* D3DVIEWPORT2 */
{
    VP(v, 2);
    gptr p = ARG(1);
    if (getenv("TPM_TRACE_VP")) {
        static int n;
        if (n++ < 12) pal_log("d3d: viewport2 %lu,%lu %lux%lu clip %.3f %.3f %.3f %.3f z %.2f..%.2f", (unsigned long)gmem_rd32(p + 4),
                              (unsigned long)gmem_rd32(p + 8), (unsigned long)gmem_rd32(p + 12), (unsigned long)gmem_rd32(p + 16),
                              fl(gmem_rd32(p + 20)), fl(gmem_rd32(p + 24)), fl(gmem_rd32(p + 28)), fl(gmem_rd32(p + 32)),
                              fl(gmem_rd32(p + 36)), fl(gmem_rd32(p + 40)));
    }
    v->vp = (r3d_viewport){gmem_rd32(p + 4), gmem_rd32(p + 8), gmem_rd32(p + 12), gmem_rd32(p + 16),
                           fl(gmem_rd32(p + 36)), fl(gmem_rd32(p + 40))};
    vp_apply(ARG(0), v);
    hle_ret(c, D3D_OK, 2);
}
static void vp_GetViewport2(cpu_t *c)
{
    VP(v, 2);
    gptr p = ARG(1);
    uint32_t u;
    gmem_wr32(p + 4, v->vp.x); gmem_wr32(p + 8, v->vp.y); gmem_wr32(p + 12, v->vp.w); gmem_wr32(p + 16, v->vp.h);
    memcpy(&u, &v->vp.minz, 4); gmem_wr32(p + 36, u);
    memcpy(&u, &v->vp.maxz, 4); gmem_wr32(p + 40, u);
    hle_ret(c, D3D_OK, 2);
}
static void vp_SetBackground(cpu_t *c) { VP(v, 2); v->bg_material = ARG(1); hle_ret(c, D3D_OK, 2); }

static uint32_t material_color(uint32_t handle)
{
    (void)handle;
    return 0;            /* background material colour: black until a game material says otherwise */
}
/* Clear(this, count, rects, flags) */
static void vp_Clear(cpu_t *c)
{
    VP(v, 4);
    r3d_clear(ARG(1), ARG(2), ARG(3), material_color(v->bg_material), 1.0f);
    hle_ret(c, D3D_OK, 4);
}
/* Clear2(this, count, rects, flags, color, z, stencil) */
static void vp_Clear2(cpu_t *c)
{
    r3d_clear(ARG(1), ARG(2), ARG(3), ARG(4), fl(ARG(5)));
    hle_ret(c, D3D_OK, 7);
}

/* ------------------------------------------------------------ IDirect3DTexture2 (surfaces) */
/* GetHandle(this, device, handle*) */
static void tx_GetHandle(cpu_t *c)
{
    gmem_wr32(ARG(2), ARG(0));                 /* the texture handle is the interface pointer */
    hle_ret(c, D3D_OK, 3);
}
static void tx_PaletteChanged(cpu_t *c)
{
    dd_surface *s = dd_surface_of(ARG(0));
    if (s) s->version++;
    hle_ret(c, D3D_OK, 3);
}
/* Load(this, src): copy the source texture's texels (and mips) into this one. */
static void copy_surface(dd_surface *d, dd_surface *s)
{
    if (d->w != s->w || d->h != s->h || d->pf.bits != s->pf.bits) {
        pal_log("d3d: Texture2::Load size/format mismatch %ux%u/%u <- %ux%u/%u", (unsigned)d->w, (unsigned)d->h,
                (unsigned)d->pf.bits, (unsigned)s->w, (unsigned)s->h, (unsigned)s->pf.bits);
        return;
    }
    for (uint32_t y = 0; y < d->h; y++) {
        uint8_t *dp = gmem_span(d->pixels + y * d->pitch, d->pitch, 1);
        const uint8_t *sp = gmem_span(s->pixels + y * s->pitch, d->pitch, 0);
        if (dp && sp) memcpy(dp, sp, d->w * d->pf.bits / 8);
    }
    d->has_ck = s->has_ck;
    d->ck_lo = s->ck_lo;
    d->ck_hi = s->ck_hi;
    if (s->palette) { com_addref(s->palette); if (d->palette) com_release(d->palette); d->palette = s->palette; }
    d->version++;
}
static void tx_Load(cpu_t *c)
{
    dd_surface *d = dd_surface_of(ARG(0)), *s = dd_surface_of(ARG(1));
    if (!d || !s) { hle_ret(c, E_INVALIDARG, 2); return; }
    while (d && s) {
        copy_surface(d, s);
        d = d->mip ? dd_surface_of(d->mip) : NULL;
        s = s->mip ? dd_surface_of(s->mip) : NULL;
    }
    hle_ret(c, D3D_OK, 2);
}

/* ------------------------------------------------------------ IDirect3DMaterial3 */
static void mt_SetMaterial(cpu_t *c)
{
    d3d_material *m = com_data(ARG(0), "material");
    for (uint32_t k = 0; m && k < 17; k++) m->data[k] = gmem_rd32(ARG(1) + 4 * k);
    hle_ret(c, m ? D3D_OK : E_INVALIDARG, 2);
}
static void mt_GetHandle(cpu_t *c)
{
    d3d_material *m = com_data(ARG(0), "material");
    gmem_wr32(ARG(2), m ? m->handle : 0);
    hle_ret(c, m ? D3D_OK : E_INVALIDARG, 3);
}

/* ------------------------------------------------------------ vtables */
static const com_method m_d3d3[] = {
    COM_IUNKNOWN, {"EnumDevices", d3_EnumDevices}, {"CreateLight", NULL}, {"CreateMaterial", d3_CreateMaterial},
    {"CreateViewport", d3_CreateViewport}, {"FindDevice", NULL}, {"CreateDevice", d3_CreateDevice},
    {"CreateVertexBuffer", NULL}, {"EnumZBufferFormats", d3_EnumZBufferFormats},
    {"EvictManagedTextures", d3_EvictManagedTextures},
};
static const com_method m_dev3[] = {
    COM_IUNKNOWN, {"GetCaps", dv_GetCaps}, {"GetStats", dv_GetStats}, {"AddViewport", dv_AddViewport},
    {"DeleteViewport", dv_DeleteViewport}, {"NextViewport", NULL}, {"EnumTextureFormats", dv_EnumTextureFormats},
    {"BeginScene", dv_BeginScene}, {"EndScene", dv_EndScene}, {"GetDirect3D", dv_GetDirect3D},
    {"SetCurrentViewport", dv_SetCurrentViewport}, {"GetCurrentViewport", dv_GetCurrentViewport},
    {"SetRenderTarget", dv_SetRenderTarget}, {"GetRenderTarget", dv_GetRenderTarget}, {"Begin", NULL},
    {"BeginIndexed", NULL}, {"Vertex", NULL}, {"Index", NULL}, {"End", NULL},
    {"GetRenderState", dv_GetRenderState}, {"SetRenderState", dv_SetRenderState}, {"GetLightState", NULL},
    {"SetLightState", dv_SetLightState}, {"SetTransform", dv_SetTransform}, {"GetTransform", dv_GetTransform},
    {"MultiplyTransform", NULL}, {"DrawPrimitive", dv_DrawPrimitive},
    {"DrawIndexedPrimitive", dv_DrawIndexedPrimitive}, {"SetClipStatus", NULL}, {"GetClipStatus", NULL},
    {"DrawPrimitiveStrided", NULL}, {"DrawIndexedPrimitiveStrided", NULL}, {"DrawPrimitiveVB", NULL},
    {"DrawIndexedPrimitiveVB", NULL}, {"ComputeSphereVisibility", NULL}, {"GetTexture", NULL},
    {"SetTexture", dv_SetTexture}, {"GetTextureStageState", dv_GetTextureStageState},
    {"SetTextureStageState", dv_SetTextureStageState}, {"ValidateDevice", dv_ValidateDevice},
};
static const com_method m_vp3[] = {
    COM_IUNKNOWN, {"Initialize", NULL}, {"GetViewport", NULL}, {"SetViewport", vp_SetViewport},
    {"TransformVertices", NULL}, {"LightElements", NULL}, {"SetBackground", vp_SetBackground},
    {"GetBackground", NULL}, {"SetBackgroundDepth", NULL}, {"GetBackgroundDepth", NULL}, {"Clear", vp_Clear},
    {"AddLight", NULL}, {"DeleteLight", NULL}, {"NextLight", NULL}, {"GetViewport2", vp_GetViewport2},
    {"SetViewport2", vp_SetViewport2}, {"SetBackgroundDepth2", NULL}, {"GetBackgroundDepth2", NULL},
    {"Clear2", vp_Clear2},
};
static const com_method m_tex2[] = {
    COM_IUNKNOWN, {"GetHandle", tx_GetHandle}, {"PaletteChanged", tx_PaletteChanged}, {"Load", tx_Load},
};
static const com_method m_mat3[] = {
    COM_IUNKNOWN, {"SetMaterial", mt_SetMaterial}, {"GetMaterial", NULL}, {"GetHandle", mt_GetHandle},
};

#define IFACE(var, name, iid, m) com_iface var = {name, iid, m, (int)(sizeof m / sizeof m[0]), 0}
IFACE(IF_Direct3D3, "IDirect3D3", 0xbb223240u, m_d3d3);
IFACE(IF_Device3, "IDirect3DDevice3", 0xb0ab3b60u, m_dev3);
IFACE(IF_Viewport3, "IDirect3DViewport3", 0xb0ab3b61u, m_vp3);
IFACE(IF_Texture2, "IDirect3DTexture2", 0x93281502u, m_tex2);
IFACE(IF_Material3, "IDirect3DMaterial3", 0xca9c46f4u, m_mat3);

void hle_d3d_init(void)
{
    g_device = 0;
    g_next_handle = 1;
    r3d_reset();
    com_register(&IF_Direct3D3);
    com_register(&IF_Device3);
    com_register(&IF_Viewport3);
    com_register(&IF_Texture2);
    com_register(&IF_Material3);
}

void hle_d3d_process_init(void)
{
    /* IID_IDirect3DHALDevice {84E63DE0-46AA-11CF-816F-0000C020156E} */
    static const uint8_t hal[16] = {0xe0, 0x3d, 0xe6, 0x84, 0xaa, 0x46, 0xcf, 0x11,
                                    0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e};
    g_hal_guid = gheap_alloc(16);
    for (uint32_t k = 0; k < 16; k++) gmem_wr8(g_hal_guid + k, hal[k]);
    static const char desc[] = "Direct3D HAL", name[] = "Direct3D HAL";
    g_str_desc = gheap_alloc(sizeof desc);
    hle_str_out(g_str_desc, sizeof desc, desc);
    g_str_name = gheap_alloc(sizeof name);
    hle_str_out(g_str_name, sizeof name, name);
}
