/* Software rasteriser for the Direct3D HLE: renders straight into the guest's
 * render target and z-buffer surfaces, so Lock/Blt/Flip see the result exactly
 * as with a real device. Reference renderer for the host build and the
 * fallback on 3DS until the GPU backend covers a feature.
 *
 * Fixed function subset used by DX6 games: FVF XYZ (world/view/proj) or
 * XYZRHW, diffuse/specular, one or two texture coordinate sets (stage 0 is
 * sampled), Gouraud/flat, perspective-correct texturing, point/bilinear,
 * wrap/clamp, colour key, alpha test, alpha blend, 16-bit z, vertex fog,
 * culling. Lighting (FVF_NORMAL with LIGHTING on) is not implemented and is
 * reported once. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/render/r3d.h"
#include "core/render/r3d_soft.h"
#include "pal/pal.h"

/* D3DRENDERSTATETYPE */
enum {
    RS_TEXTUREPERSPECTIVE = 4, RS_ZENABLE = 7, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15,
    RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25,
    RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28, RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35,
    RS_FOGTABLESTART = 36, RS_FOGTABLEEND = 37, RS_FOGTABLEDENSITY = 38, RS_COLORKEYENABLE = 41,
    RS_LIGHTING = 137,
};
/* D3DTEXTURESTAGESTATETYPE */
enum { TSS_COLOROP = 1, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG1, TSS_ALPHAARG2,
       TSS_ADDRESS = 12, TSS_ADDRESSU, TSS_ADDRESSV, TSS_MAGFILTER = 16, TSS_MINFILTER };

typedef struct { float x, y, z, w; float c[4], s[4]; float u, v; } vtx;   /* c/s: r,g,b,a in 0..1 */

/* ---- 16-bit pixel formats described by masks ---- */
typedef struct { int sh[4], bits[4]; uint32_t mask[4]; } fmt16;

static void fmt_from(fmt16 *f, const pixfmt *pf)
{
    uint32_t m[4] = {pf->r, pf->g, pf->b, (pf->flags & DDPF_ALPHAPIXELS) ? pf->a : 0};
    for (int k = 0; k < 4; k++) {
        f->mask[k] = m[k];
        f->sh[k] = 0; f->bits[k] = 0;
        if (!m[k]) continue;
        while (!((m[k] >> f->sh[k]) & 1u)) f->sh[k]++;
        while ((m[k] >> (f->sh[k] + f->bits[k])) & 1u) f->bits[k]++;
    }
}

static inline float chan(const fmt16 *f, uint32_t p, int k)
{
    if (!f->bits[k]) return 1.0f;
    return (float)((p & f->mask[k]) >> f->sh[k]) / (float)((1u << f->bits[k]) - 1u);
}

static inline uint16_t pack(const fmt16 *f, const float c[4])
{
    uint32_t p = 0;
    for (int k = 0; k < 4; k++) {
        if (!f->bits[k]) continue;
        float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
        p |= ((uint32_t)(v * (float)((1u << f->bits[k]) - 1u) + 0.5f) << f->sh[k]) & f->mask[k];
    }
    return (uint16_t)p;
}

static float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static void argb(uint32_t v, float c[4])
{
    c[0] = (float)((v >> 16) & 255) / 255.0f; c[1] = (float)((v >> 8) & 255) / 255.0f;
    c[2] = (float)(v & 255) / 255.0f;         c[3] = (float)(v >> 24) / 255.0f;
}

/* ---- render target ---- */
typedef struct {
    uint8_t *px, *zb;
    uint32_t w, h, pitch, zpitch;
    fmt16    f;
} target;

static int bind(target *t)
{
    dd_surface *s = g_r3d.target;
    if (!s || s->pf.bits != 16) return 0;
    t->w = s->w; t->h = s->h; t->pitch = s->pitch;
    t->px = gmem_span(s->pixels, s->pitch * s->h, 1);
    if (!t->px) return 0;
    fmt_from(&t->f, &s->pf);
    t->zb = NULL;
    dd_surface *z = g_r3d.zbuf;
    if (z && z->w >= s->w && z->h >= s->h && z->pf.bits == 16) {
        t->zb = gmem_span(z->pixels, z->pitch * z->h, 1);
        t->zpitch = z->pitch;
    }
    return 1;
}

/* ---- textures ---- */
typedef struct {
    const uint8_t *px;
    uint32_t w, h, pitch;
    fmt16    f;
    int      ck;
    uint32_t ck_lo, ck_hi;
    int      wrap_u, wrap_v, bilinear;
} tex;

static int tex_bind(tex *t)
{
    dd_surface *s = g_r3d.tex[0];
    if (!s || s->pf.bits != 16 || !s->w || !s->h) return 0;
    t->px = gmem_span(s->pixels, s->pitch * s->h, 0);
    if (!t->px) return 0;
    t->w = s->w; t->h = s->h; t->pitch = s->pitch;
    fmt_from(&t->f, &s->pf);
    t->ck = g_r3d.rs[RS_COLORKEYENABLE] && s->has_ck;
    t->ck_lo = s->ck_lo; t->ck_hi = s->ck_hi;
    uint32_t au = g_r3d.tss[0][TSS_ADDRESSU] ? g_r3d.tss[0][TSS_ADDRESSU] : 1;
    uint32_t av = g_r3d.tss[0][TSS_ADDRESSV] ? g_r3d.tss[0][TSS_ADDRESSV] : 1;
    t->wrap_u = au != 3; t->wrap_v = av != 3;
    t->bilinear = g_r3d.tss[0][TSS_MAGFILTER] == 2;
    return 1;
}

static inline int coord(int i, uint32_t n, int wrap)
{
    if (wrap) { i %= (int)n; return i < 0 ? i + (int)n : i; }
    return i < 0 ? 0 : i >= (int)n ? (int)n - 1 : i;
}

/* Returns 0 if the texel is colour-keyed out. */
static inline int texel(const tex *t, int x, int y, float out[4])
{
    uint16_t p;
    memcpy(&p, t->px + (uint32_t)y * t->pitch + (uint32_t)x * 2u, 2);
    if (t->ck && p >= t->ck_lo && p <= t->ck_hi) return 0;
    for (int k = 0; k < 4; k++) out[k] = chan(&t->f, p, k);
    return 1;
}

static int sample(const tex *t, float u, float v, float out[4])
{
    if (!t->bilinear) {
        int x = coord((int)floorf(u * (float)t->w), t->w, t->wrap_u);
        int y = coord((int)floorf(v * (float)t->h), t->h, t->wrap_v);
        return texel(t, x, y, out);
    }
    float fx = u * (float)t->w - 0.5f, fy = v * (float)t->h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float ax = fx - (float)x0, ay = fy - (float)y0, acc[4] = {0, 0, 0, 0}, wsum = 0;
    for (int k = 0; k < 4; k++) {
        int dx = k & 1, dy = k >> 1;
        float w = (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay), c[4];
        if (w <= 0) continue;
        if (!texel(t, coord(x0 + dx, t->w, t->wrap_u), coord(y0 + dy, t->h, t->wrap_v), c)) continue;
        for (int j = 0; j < 4; j++) acc[j] += c[j] * w;
        wsum += w;
    }
    if (wsum < 0.5f) return 0;                     /* mostly keyed out */
    for (int j = 0; j < 4; j++) out[j] = acc[j] / wsum;
    return 1;
}

/* ---- fixed-function pipeline pieces ---- */
static int cmp(uint32_t func, float a, float b)
{
    switch (func) {
    case 1: return 0;
    case 2: return a < b;
    case 3: return a == b;
    case 4: return a <= b;
    case 5: return a > b;
    case 6: return a != b;
    case 7: return a >= b;
    default: return 1;
    }
}

static void factor(uint32_t f, const float s[4], const float d[4], float out[4])
{
    for (int k = 0; k < 4; k++) {
        float v;
        switch (f) {
        case 1: v = 0; break;
        case 3: v = s[k]; break;
        case 4: v = 1 - s[k]; break;
        case 5: v = s[3]; break;
        case 6: v = 1 - s[3]; break;
        case 7: v = d[3]; break;
        case 8: v = 1 - d[3]; break;
        case 9: v = d[k]; break;
        case 10: v = 1 - d[k]; break;
        case 11: { float m = 1 - d[3]; v = k == 3 ? 1 : (s[3] < m ? s[3] : m); break; }   /* SRCALPHASAT */
        default: v = 1; break;
        }
        out[k] = v;
    }
}

static void combine(uint32_t op, const float a1[4], const float a2[4], int k, float *out)
{
    switch (op) {
    case 2: *out = a1[k]; break;                          /* SELECTARG1 */
    case 3: *out = a2[k]; break;                          /* SELECTARG2 */
    case 5: *out = a1[k] * a2[k] * 2; break;              /* MODULATE2X */
    case 6: *out = a1[k] * a2[k] * 4; break;              /* MODULATE4X */
    case 7: *out = a1[k] + a2[k]; break;                  /* ADD */
    case 8: *out = a1[k] + a2[k] - 0.5f; break;           /* ADDSIGNED */
    default: *out = a1[k] * a2[k]; break;                 /* MODULATE */
    }
}

static const float *arg(uint32_t a, const float *texc, const float *diff)
{
    return (a & 0xf) == 2 ? texc : diff;                  /* D3DTA_TEXTURE, else DIFFUSE/CURRENT */
}

/* ---- debugging: TPM_R3D_PROBE=x,y,scene logs every write to that pixel in that scene ---- */
static int g_probe_x = -2, g_probe_y = -2;
int g_r3d_probe_scene = -1;
static void probe_init(void)
{
    if (g_probe_x != -2) return;
    g_probe_x = g_probe_y = -1;
    const char *e = getenv("TPM_R3D_PROBE");
    if (e) sscanf(e, "%d,%d,%d", &g_probe_x, &g_probe_y, &g_r3d_probe_scene);
}

/* ---- triangle setup and scan ---- */
typedef struct { target t; tex x; int textured; } ctx;

static void tri(ctx *k, const vtx *a, const vtx *b, const vtx *c)
{
    const uint32_t *rs = g_r3d.rs;
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    uint32_t cull = rs[RS_CULLMODE];
    if (cull == 2 && area < 0) return;                    /* CW culled (y down: area < 0 is CW on screen) */
    if (cull == 3 && area > 0) return;
    g_r3d.tris_drawn++;

    r3d_viewport *vp = &g_r3d.vp;
    int x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    int cx0 = 0, cy0 = 0, cx1 = (int)k->t.w, cy1 = (int)k->t.h;
    if (vp->w && vp->h) {
        if ((int)vp->x > cx0) cx0 = (int)vp->x;
        if ((int)vp->y > cy0) cy0 = (int)vp->y;
        if ((int)(vp->x + vp->w) < cx1) cx1 = (int)(vp->x + vp->w);
        if ((int)(vp->y + vp->h) < cy1) cy1 = (int)(vp->y + vp->h);
    }
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) return;

    int flat = rs[RS_SHADEMODE] == 1, ztest = rs[RS_ZENABLE] && k->t.zb, zwrite = ztest && rs[RS_ZWRITEENABLE];
    int blend = rs[RS_ALPHABLENDENABLE], atest = rs[RS_ALPHATESTENABLE];
    int fog = rs[RS_FOGENABLE], spec = rs[RS_SPECULARENABLE];
    float aref = (float)(rs[RS_ALPHAREF] & 255) / 255.0f, fogc[4];
    uint32_t fog_table = rs[RS_FOGTABLEMODE];
    float fog_start = bits_f(rs[RS_FOGTABLESTART]), fog_end = bits_f(rs[RS_FOGTABLEEND]), fog_density = bits_f(rs[RS_FOGTABLEDENSITY]);
    argb(rs[RS_FOGCOLOR], fogc);
    uint32_t cop = g_r3d.tss[0][TSS_COLOROP] ? g_r3d.tss[0][TSS_COLOROP] : 4;
    uint32_t aop = g_r3d.tss[0][TSS_ALPHAOP] ? g_r3d.tss[0][TSS_ALPHAOP] : 2;
    uint32_t ca1 = g_r3d.tss[0][TSS_COLORARG1], ca2 = g_r3d.tss[0][TSS_COLORARG2];
    uint32_t aa1 = g_r3d.tss[0][TSS_ALPHAARG1], aa2 = g_r3d.tss[0][TSS_ALPHAARG2];
    float inv = 1.0f / area;

    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        uint8_t *row = k->t.px + (uint32_t)y * k->t.pitch;
        uint8_t *zrow = k->t.zb ? k->t.zb + (uint32_t)y * k->t.zpitch : NULL;
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (c->x - px) * (b->y - py)) * inv;
            float w1 = ((c->x - px) * (a->y - py) - (a->x - px) * (c->y - py)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) {
                /* top-left fill rule approximation: accept exact edges only on one side */
                if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f) continue;
            }
            float z = w0 * a->z + w1 * b->z + w2 * c->z;
            uint16_t zq = (uint16_t)(z <= 0 ? 0 : z >= 1 ? 65535 : z * 65535.0f);
            if (ztest) {
                uint16_t old;
                memcpy(&old, zrow + (uint32_t)x * 2u, 2);
                if (!cmp(rs[RS_ZFUNC] ? rs[RS_ZFUNC] : 4, (float)zq, (float)old)) continue;
            }
            /* perspective-correct weights */
            float q0 = w0 * a->w, q1 = w1 * b->w, q2 = w2 * c->w, qs = q0 + q1 + q2;
            if (qs != 0) { q0 /= qs; q1 /= qs; q2 /= qs; }
            float diff[4], sp[4], col[4];
            for (int j = 0; j < 4; j++) {
                diff[j] = flat ? a->c[j] : q0 * a->c[j] + q1 * b->c[j] + q2 * c->c[j];
                sp[j] = flat ? a->s[j] : q0 * a->s[j] + q1 * b->s[j] + q2 * c->s[j];
            }
            if (k->textured) {
                float u = q0 * a->u + q1 * b->u + q2 * c->u, v = q0 * a->v + q1 * b->v + q2 * c->v, tc[4];
                if (!sample(&k->x, u, v, tc)) continue;
                for (int j = 0; j < 3; j++) combine(cop, arg(ca1, tc, diff), arg(ca2, tc, diff), j, &col[j]);
                combine(aop, arg(aa1, tc, diff), arg(aa2, tc, diff), 3, &col[3]);
            } else memcpy(col, diff, sizeof col);
            if (spec) for (int j = 0; j < 3; j++) col[j] += sp[j];
            if (atest && !cmp(rs[RS_ALPHAFUNC] ? rs[RS_ALPHAFUNC] : 8, col[3], aref)) continue;
            if (fog) {
                float f = sp[3];                          /* vertex fog: specular alpha, 1 = no fog */
                if (fog_table) {                          /* pixel fog on eye depth (W fog) */
                    float rhw = w0 * a->w + w1 * b->w + w2 * c->w, d = rhw > 0 ? 1.0f / rhw : 1e30f;
                    if (fog_table == 3) f = fog_end > fog_start ? (fog_end - d) / (fog_end - fog_start) : 1.0f;
                    else { float e = d * fog_density; f = expf(-(fog_table == 2 ? e * e : e)); }
                    f = f < 0 ? 0 : f > 1 ? 1 : f;
                }
                for (int j = 0; j < 3; j++) col[j] = col[j] * f + fogc[j] * (1 - f);
            }
            uint8_t *dp = row + (uint32_t)x * 2u;
            if (blend) {
                uint16_t dpx;
                memcpy(&dpx, dp, 2);
                float d[4], fs[4], fd[4];
                for (int j = 0; j < 4; j++) d[j] = chan(&k->t.f, dpx, j);
                factor(rs[RS_SRCBLEND] ? rs[RS_SRCBLEND] : 2, col, d, fs);
                factor(rs[RS_DESTBLEND] ? rs[RS_DESTBLEND] : 1, col, d, fd);
                for (int j = 0; j < 4; j++) col[j] = col[j] * fs[j] + d[j] * fd[j];
            }
            uint16_t out = pack(&k->t.f, col);
            if (x == g_probe_x && y == g_probe_y && g_r3d.probe_budget > 0) {
                g_r3d.probe_budget--;
                dd_surface *ts = g_r3d.tex[0];
                pal_log("r3d: probe scene %lu draw %lu: diff %.2f %.2f %.2f %.2f spec %.2f %.2f %.2f %.2f -> %.2f %.2f %.2f %.2f "
                        "tex %lux%lu pf %lx r%lx g%lx b%lx a%lx uv %.3f %.3f fog %lu col %08lx table %lu %08lx %08lx %08lx blend %lu %lu %lu",
                        (unsigned long)g_r3d.scenes, (unsigned long)g_r3d.draws,
                        diff[0], diff[1], diff[2], diff[3], sp[0], sp[1], sp[2], sp[3], col[0], col[1], col[2], col[3],
                        ts ? (unsigned long)ts->w : 0ul, ts ? (unsigned long)ts->h : 0ul, ts ? (unsigned long)ts->pf.flags : 0ul,
                        ts ? (unsigned long)ts->pf.r : 0ul, ts ? (unsigned long)ts->pf.g : 0ul, ts ? (unsigned long)ts->pf.b : 0ul,
                        ts ? (unsigned long)ts->pf.a : 0ul,
                        q0 * a->u + q1 * b->u + q2 * c->u, q0 * a->v + q1 * b->v + q2 * c->v,
                        (unsigned long)rs[RS_FOGENABLE], (unsigned long)rs[RS_FOGCOLOR], (unsigned long)rs[35], (unsigned long)rs[36], (unsigned long)rs[37], (unsigned long)rs[38], (unsigned long)blend,
                        (unsigned long)rs[RS_SRCBLEND], (unsigned long)rs[RS_DESTBLEND]);
            }
            memcpy(dp, &out, 2);
            if (zwrite) memcpy(zrow + (uint32_t)x * 2u, &zq, 2);
        }
    }
}

/* ---- vertex fetch and transform ---- */
static void mat_mul(const float *a, const float *b, float *o)          /* row vectors: v * a * b */
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

static float rdf(gptr p) { uint32_t u = gmem_rd32(p); float f; memcpy(&f, &u, 4); return f; }

/* Clip-space vertex (x,y,z,w) for untransformed input; screen-space for RHW. */
static void fetch(uint32_t fvf, gptr p, const float *mvp, vtx *v)
{
    gptr q = p;
    if (fvf & FVF_XYZRHW) {
        v->x = rdf(q); v->y = rdf(q + 4); v->z = rdf(q + 8); v->w = rdf(q + 12);
        q += 16;
    } else {
        float x = rdf(q), y = rdf(q + 4), z = rdf(q + 8);
        v->x = x * mvp[0] + y * mvp[4] + z * mvp[8] + mvp[12];
        v->y = x * mvp[1] + y * mvp[5] + z * mvp[9] + mvp[13];
        v->z = x * mvp[2] + y * mvp[6] + z * mvp[10] + mvp[14];
        v->w = x * mvp[3] + y * mvp[7] + z * mvp[11] + mvp[15];
        q += 12;
    }
    if (fvf & FVF_NORMAL) q += 12;
    if (fvf & 0x020u) q += 4;
    if (fvf & FVF_DIFFUSE) { argb(gmem_rd32(q), v->c); q += 4; }
    else { v->c[0] = v->c[1] = v->c[2] = v->c[3] = 1.0f; }
    if (fvf & FVF_SPECULAR) { argb(gmem_rd32(q), v->s); q += 4; }
    else { v->s[0] = v->s[1] = v->s[2] = 0; v->s[3] = 1.0f; }
    if (FVF_TEXCOUNT(fvf)) { v->u = rdf(q); v->v = rdf(q + 4); }
    else v->u = v->v = 0;
}

/* Clip-space -> screen: perspective divide and viewport; w becomes 1/w. */
static void to_screen(vtx *v)
{
    r3d_viewport *vp = &g_r3d.vp;
    float iw = 1.0f / v->w;
    float vw = vp->w ? (float)vp->w : (float)(g_r3d.target ? g_r3d.target->w : 640);
    float vh = vp->h ? (float)vp->h : (float)(g_r3d.target ? g_r3d.target->h : 480);
    float minz = vp->minz, maxz = (vp->maxz > vp->minz) ? vp->maxz : 1.0f;
    v->x = (float)vp->x + (v->x * iw + 1.0f) * vw * 0.5f;
    v->y = (float)vp->y + (1.0f - v->y * iw) * vh * 0.5f;
    v->z = minz + v->z * iw * (maxz - minz);
    v->w = iw;
}

#define NEAR_W 1e-4f

static void lerp(const vtx *a, const vtx *b, float t, vtx *o)
{
    o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t;
    o->z = a->z + (b->z - a->z) * t; o->w = a->w + (b->w - a->w) * t;
    for (int j = 0; j < 4; j++) { o->c[j] = a->c[j] + (b->c[j] - a->c[j]) * t; o->s[j] = a->s[j] + (b->s[j] - a->s[j]) * t; }
    o->u = a->u + (b->u - a->u) * t; o->v = a->v + (b->v - a->v) * t;
}

/* Untransformed triangle: clip against the near plane (z >= 0, D3D) and w > 0, then rasterise. */
static void tri_clip(ctx *k, const vtx *a, const vtx *b, const vtx *c)
{
    vtx in[8], out[8];
    int n = 3, m;
    in[0] = *a; in[1] = *b; in[2] = *c;
    for (int plane = 0; plane < 2; plane++) {
        m = 0;
        for (int i = 0; i < n; i++) {
            const vtx *p = &in[i], *q = &in[(i + 1) % n];
            float dp = plane == 0 ? p->z : p->w - NEAR_W, dq = plane == 0 ? q->z : q->w - NEAR_W;
            if (dp >= 0) out[m++] = *p;
            if ((dp >= 0) != (dq >= 0) && m < 8) lerp(p, q, dp / (dp - dq), &out[m++]);
        }
        if (m < 3) return;
        memcpy(in, out, sizeof(vtx) * (size_t)m);
        n = m;
    }
    for (int i = 0; i < n; i++) to_screen(&in[i]);
    for (int i = 1; i + 1 < n; i++) tri(k, &in[0], &in[i], &in[i + 1]);
}

void r3d_soft_draw(uint32_t prim, uint32_t fvf, gptr verts, uint32_t nverts, gptr indices, uint32_t nindices)
{
    ctx k;
    probe_init();
    if (!bind(&k.t)) return;
    k.textured = FVF_TEXCOUNT(fvf) && tex_bind(&k.x);
    {   /* one log line per distinct state combination (diagnostics) */
        static uint32_t seen[64][4];
        static int nseen;
        dd_surface *ts = g_r3d.tex[0];
        uint32_t key[4] = {fvf | (k.textured ? 0x10000u : 0) | (ts ? 0x20000u : 0),
                           g_r3d.tss[0][1] | g_r3d.tss[0][2] << 8 | g_r3d.tss[0][3] << 16 | g_r3d.tss[0][4] << 24,
                           g_r3d.rs[27] | g_r3d.rs[19] << 8 | g_r3d.rs[20] << 16 | g_r3d.rs[15] << 24,
                           ts ? ts->pf.bits | ts->pf.flags << 8 : 0};
        int found = 0;
        for (int i = 0; i < nseen; i++) if (!memcmp(seen[i], key, sizeof key)) found = 1;
        if (!found && nseen < 64) {
            memcpy(seen[nseen++], key, sizeof key);
            pal_log("r3d: combo fvf %03lx tex %s%s (pf %lu bits flags %lx a %lx) colorop %lu a1 %lu a2 %lu alphaop %lu "
                    "blend %lu src %lu dst %lu atest %lu ck %lu tss1op %lu",
                    (unsigned long)fvf, ts ? "set" : "none", ts && !k.textured ? " NOT BOUND" : "",
                    ts ? (unsigned long)ts->pf.bits : 0ul, ts ? (unsigned long)ts->pf.flags : 0ul, ts ? (unsigned long)ts->pf.a : 0ul,
                    (unsigned long)g_r3d.tss[0][1], (unsigned long)g_r3d.tss[0][2], (unsigned long)g_r3d.tss[0][3],
                    (unsigned long)g_r3d.tss[0][4], (unsigned long)g_r3d.rs[27], (unsigned long)g_r3d.rs[19],
                    (unsigned long)g_r3d.rs[20], (unsigned long)g_r3d.rs[15], (unsigned long)g_r3d.rs[41],
                    (unsigned long)g_r3d.tss[1][1]);
        }
    }
    if ((fvf & FVF_NORMAL) && g_r3d.rs[RS_LIGHTING] && !g_r3d.warned_lighting) {
        g_r3d.warned_lighting = 1;
        pal_log("r3d: fixed-function lighting not implemented (fvf %03lx)", (unsigned long)fvf);
    }
    if (prim < PT_TRIANGLELIST) {
        if (!g_r3d.warned_lines) { g_r3d.warned_lines = 1; pal_log("r3d: points/lines (type %lu) not drawn", (unsigned long)prim); }
        return;
    }
    uint32_t stride = r3d_fvf_stride(fvf), n = indices ? nindices : nverts;
    float wv[16], mvp[16];
    int pre = (fvf & FVF_XYZRHW) != 0;
    if (!pre) { mat_mul(g_r3d.world, g_r3d.view, wv); mat_mul(wv, g_r3d.proj, mvp); }

#define VTX(i, out) do {                                                           \
        uint32_t ix_ = indices ? gmem_rd16(indices + 2u * (i)) : (i);               \
        if (ix_ >= nverts) return;                                                  \
        fetch(fvf, verts + ix_ * stride, mvp, &(out));                              \
    } while (0)

    vtx v0, v1, v2;
    for (uint32_t i = 0; i + 2 < n; ) {
        uint32_t a, b, c;
        if (prim == PT_TRIANGLELIST) { a = i; b = i + 1; c = i + 2; i += 3; }
        else if (prim == PT_TRIANGLESTRIP) { a = i; b = i + 1; c = i + 2; if (i & 1) { a = i + 1; b = i; } i += 1; }
        else { a = 0; b = i + 1; c = i + 2; i += 1; }                     /* fan */
        if (c >= n) break;
        VTX(a, v0); VTX(b, v1); VTX(c, v2);
        if (pre) tri(&k, &v0, &v1, &v2);
        else tri_clip(&k, &v0, &v1, &v2);
    }
#undef VTX
}

void r3d_soft_clear(uint32_t nrects, gptr rects, uint32_t flags, uint32_t color, float z)
{
    target t;
    if (!bind(&t)) return;
    float cc[4];
    argb(color, cc);
    uint16_t px = pack(&t.f, cc), zq = (uint16_t)(z <= 0 ? 0 : z >= 1 ? 65535 : z * 65535.0f);
    for (uint32_t r = 0; r < (nrects ? nrects : 1); r++) {
        int32_t x0 = 0, y0 = 0, x1 = (int32_t)t.w, y1 = (int32_t)t.h;
        if (nrects && rects) {
            x0 = (int32_t)gmem_rd32(rects + 16 * r); y0 = (int32_t)gmem_rd32(rects + 16 * r + 4);
            x1 = (int32_t)gmem_rd32(rects + 16 * r + 8); y1 = (int32_t)gmem_rd32(rects + 16 * r + 12);
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)t.w) x1 = (int32_t)t.w;
        if (y1 > (int32_t)t.h) y1 = (int32_t)t.h;
        for (int32_t y = y0; y < y1; y++)
            for (int32_t x = x0; x < x1; x++) {
                if (flags & 1u) memcpy(t.px + (uint32_t)y * t.pitch + (uint32_t)x * 2u, &px, 2);
                if ((flags & 2u) && t.zb) memcpy(t.zb + (uint32_t)y * t.zpitch + (uint32_t)x * 2u, &zq, 2);
            }
    }
}
