/* Free camera look on the C-Stick (New 3DS / 2DS XL). The game has no camera control of its own: updateCam
 * (FUN_00418544) eases the camera yaw (float DAT_005bb534, degrees) and pitch (cam[0xd], degrees 270..360) back toward
 * the level's scripted values by 4% of the difference per frame (FUN_00419067). Pushing those two values from the
 * outside therefore orbits the camera and it springs back by itself when the stick is released. No guest code is
 * touched, only these two floats, once per pump. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/gmem.h"
#include "pal/pal.h"

#define CAM_PTR   0x008a011cu     /* camera struct pointer */
#define CAM_YAW   0x005bb534u     /* float yaw base, degrees */
#define CAM_PITCH 0x34u           /* camera struct +0x34 (index 0xd): float pitch, degrees */

int g_cam_sens = 100;   /* percent, set by the bottom-screen UI */
extern int pal3ds_cstick(int *x, int *y) __attribute__((weak));

static float rdf(gptr a) { uint32_t u = gmem_rd32(a); float f; memcpy(&f, &u, 4); return f; }
static void  wrf(gptr a, float f) { uint32_t u; memcpy(&u, &f, 4); gmem_wr32(a, u); }

void camera_look_tick(void)
{
    static uint32_t last_ms;
    int x = 0, y = 0;
    if (pal3ds_cstick) pal3ds_cstick(&x, &y);
    else { static int init; static int hx, hy; if (!init) { init = 1; const char *a = getenv("TPM_CAMX"), *b = getenv("TPM_CAMY"); hx = a ? atoi(a) : 0; hy = b ? atoi(b) : 0; } x = hx; y = hy; }
    uint32_t now = pal_time_ms();
    uint32_t dt = last_ms ? now - last_ms : 0;
    last_ms = now;
    if (dt > 50) dt = 50;
    if (abs(x) < 24) x = 0;
    if (abs(y) < 24) y = 0;
    if (!(x | y) || !dt) return;
    gptr cam = gmem_rd32(CAM_PTR);
    if (cam < 0x10000u || cam > g_gmem_size - 0x100u) return;           /* no level camera (menus, loading) */
    float yaw = rdf(CAM_YAW), pitch = rdf(cam + CAM_PITCH);
    if (!(pitch >= 270.0f && pitch <= 360.0f) || !(yaw > -720.0f && yaw < 720.0f)) return;   /* not the level camera */
    float s = (float)dt / 1000.0f * (float)g_cam_sens / 100.0f;
    yaw   += (float)x / 156.0f * 55.0f * s;        /* up to ~55 deg/s (settles near 45 deg: the game pulls back 4%/frame) */
    pitch += (float)y / 156.0f * 35.0f * s;        /* up to ~35 deg/s: look up = larger */
    if (pitch > 340.0f) pitch = 340.0f;   /* a freeze was reported when looking up: stay well away from the extremes */
    if (pitch < 295.0f) pitch = 295.0f;
    if (yaw >= 360.0f) yaw -= 360.0f;
    if (yaw < 0.0f) yaw += 360.0f;
    wrf(CAM_YAW, yaw);
    wrf(cam + CAM_PITCH, pitch);
}
