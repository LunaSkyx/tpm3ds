/* GPU backend self-test (3DS; run when sdmc:/3ds/tpm/gputest.txt exists).
 * Uploads textures whose texels encode their own coordinates (R = x, G = y),
 * draws them 1:1 through the same pal_gpu API the game uses, reads the target
 * back and checks every pixel; logs PASS/FAIL and, on failure, which texel
 * landed at a few probe pixels. */
#include <stdlib.h>
#include <string.h>
#include "pal/pal.h"

int gpu_selftest(uint32_t tw, uint32_t th);

static int run(uint32_t W, uint32_t H, uint32_t w, uint32_t h, uint32_t ph, const char *name)
{
    uint16_t *tex = malloc(32 * ph * 2);
    uint8_t *out = malloc(W * H * 2);
    if (!tex || !out) { free(tex); free(out); return 0; }
    for (uint32_t y = 0; y < ph; y++)
        for (uint32_t x = 0; x < 32; x++) tex[y * 32 + x] = (uint16_t)((x << 11) | ((y < h ? y : h - 1) << 5));
    uint32_t id = pal_gpu_tex_create(32, ph, PAL_TEX_RGB565);
    pal_gpu_tex_upload(id, tex);
    /* quad covering pixels [0,w) x [0,h), texels 1:1 (v scaled for padding as the core does) */
    float sv = (float)h / (float)ph;
    float X0 = -1.0f, X1 = (float)w * 2.0f / (float)W - 1.0f, Y0 = 1.0f, Y1 = 1.0f - (float)h * 2.0f / (float)H;
    const float q[6][4] = {{X0, Y0, 0, 0}, {X1, Y0, 1, 0}, {X0, Y1, 0, 1}, {X1, Y0, 1, 0}, {X1, Y1, 1, 1}, {X0, Y1, 0, 1}};
    pal_gpu_vertex v[6];
    for (int k = 0; k < 6; k++) {
        memset(&v[k], 0, sizeof v[k]);
        v[k].x = q[k][0]; v[k].y = q[k][1]; v[k].z = 0.5f; v[k].w = 1.0f;
        v[k].u = q[k][2] * (float)w / 32.0f; v[k].v = q[k][3] * sv; v[k].fog = 1.0f;
        v[k].rgba[0] = v[k].rgba[1] = v[k].rgba[2] = v[k].rgba[3] = 255;
    }
    pal_gpu_state st;
    memset(&st, 0, sizeof st);
    st.tex = id; st.color_op = 2; st.color_a1 = PAL_TA_TEXTURE; st.alpha_op = 2; st.alpha_a1 = PAL_TA_TEXTURE;
    st.alpha_func = 8; st.color_write = 1; st.z_func = 8;
    st.clip_x0 = 0; st.clip_y0 = 0; st.clip_x1 = (uint16_t)W; st.clip_y1 = (uint16_t)H;
    st.clamp_u = st.clamp_v = 1;
    pal_gpu_draw(&st, v, 6);
    pal_gpu_target_store(0, out, W * 2);
    uint32_t bad = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint16_t p;
            memcpy(&p, out + y * W * 2 + x * 2, 2);
            if ((p >> 11) != x || ((p >> 5) & 63) != y) bad++;
        }
    pal_log("gpu selftest %s: %lu of %lu pixels wrong -> %s", name, (unsigned long)bad, (unsigned long)(w * h), bad ? "FAIL" : "PASS");
    if (bad) {
        static const uint32_t probe[][2] = {{0, 0}, {1, 0}, {0, 1}, {7, 0}, {0, 7}, {8, 0}, {0, 8}, {3, 5}, {31, 0}, {0, 39}};
        for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
            uint32_t x = probe[i][0], y = probe[i][1];
            if (x >= w || y >= h) continue;
            uint16_t p;
            memcpy(&p, out + y * W * 2 + x * 2, 2);
            pal_log("gpu selftest %s: pixel (%lu,%lu) shows texel (%u,%u)", name, (unsigned long)x, (unsigned long)y,
                    (unsigned)(p >> 11), (unsigned)((p >> 5) & 63));
        }
    }
    pal_gpu_tex_free(id);
    free(tex);
    free(out);
    return bad == 0;
}

int gpu_selftest(uint32_t W, uint32_t H)
{
    if (!pal_gpu_init(W, H)) { pal_log("gpu selftest: no GPU"); return 0; }
    int ok = run(W, H, 32, 64, 64, "32x64");
    ok &= run(W, H, 32, 40, 64, "32x40 padded");
    return ok;
}
