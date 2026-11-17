/* DirectInput (DINPUT.DLL, version 0x500) HLE: system keyboard and mouse,
 * immediate (GetDeviceState) and buffered (GetDeviceData) reads, backed by core/input. */
#include <stdlib.h>
#include <string.h>
#include "core/hle/hle_com.h"
#include "core/hle/hle_user32.h"
#include "core/input.h"
#include "pal/pal.h"

#define DI_OK            0u
#define DIERR_NOTACQUIRED 0x8007000cu
#define DIERR_DEVICENOTREG 0x80040154u

extern com_iface IF_DInput, IF_DIDevice, IF_DIDevice2;

typedef struct { int dev; int acquired; } di_device;   /* dev 0 keyboard, 1 mouse */
static const com_iface *const di_ifaces[] = {&IF_DInput, NULL};
static const com_iface *const dev_ifaces[] = {&IF_DIDevice, &IF_DIDevice2, NULL};

/* DirectInputCreateA(hinst, version, out, unk) */
static void d_DirectInputCreateA(cpu_t *c)
{
    gmem_wr32(ARG(2), com_create("dinput", NULL, NULL, di_ifaces));
    hle_ret(c, DI_OK, 4);
}

/* CreateDevice(this, guid, out, unk) */
static void di_CreateDevice(cpu_t *c)
{
    uint32_t g0 = gmem_rd32(ARG(1));
    int dev = g0 == 0x6f1d2b61u ? 0 : g0 == 0x6f1d2b60u ? 1 : -1;     /* GUID_SysKeyboard / GUID_SysMouse */
    if (dev < 0) { pal_log("dinput: CreateDevice guid %08lx not available", (unsigned long)g0); hle_ret(c, DIERR_DEVICENOTREG, 4); return; }
    di_device *d = calloc(1, sizeof *d);
    d->dev = dev;
    gmem_wr32(ARG(2), com_create("didevice", d, free, dev_ifaces));
    hle_ret(c, DI_OK, 4);
}
static void di_EnumDevices(cpu_t *c) { hle_ret(c, DI_OK, 5); }   /* no joysticks */

#define DEV(var, n) di_device *var = com_data(ARG(0), "didevice"); if (!var) { hle_ret(c, E_INVALIDARG, n); return; }

static void dd_SetDataFormat(cpu_t *c) { hle_ret(c, DI_OK, 2); }
static void dd_SetCooperativeLevel(cpu_t *c) { hle_ret(c, DI_OK, 3); }

/* SetProperty(this, guid-or-id, DIPROPHEADER*) */
static void dd_SetProperty(cpu_t *c)
{
    DEV(d, 3);
    if (ARG(1) == 1) input_set_buffer_size(d->dev, gmem_rd32(ARG(2) + 16));   /* DIPROP_BUFFERSIZE */
    hle_ret(c, DI_OK, 3);
}
static void dd_Acquire(cpu_t *c)
{
    DEV(d, 1);
    uint32_t r = d->acquired ? S_FALSE : DI_OK;
    d->acquired = 1;
    hle_ret(c, r, 1);
}
static void dd_Unacquire(cpu_t *c)
{
    DEV(d, 1);
    uint32_t r = d->acquired ? DI_OK : S_FALSE;
    d->acquired = 0;
    hle_ret(c, r, 1);
}

/* GetDeviceState(this, size, data) */
static void dd_GetDeviceState(cpu_t *c)
{
    DEV(d, 3);
    hle_pump_platform();
    if (!d->acquired) { hle_ret(c, DIERR_NOTACQUIRED, 3); return; }
    gptr p = ARG(2);
    uint32_t n = ARG(1);
    if (d->dev == 0) {
        const uint8_t *k = input_keys();
        for (uint32_t i = 0; i < n && i < 256; i++) gmem_wr8(p + i, k[i]);
    } else {
        int32_t dx, dy, dz;
        uint8_t b[4];
        input_mouse_take(&dx, &dy, &dz, b);
        if (n >= 12) { gmem_wr32(p, (uint32_t)dx); gmem_wr32(p + 4, (uint32_t)dy); gmem_wr32(p + 8, (uint32_t)dz); }
        for (uint32_t i = 0; i < 4 && 12 + i < n; i++) gmem_wr8(p + 12 + i, b[i]);
    }
    hle_ret(c, DI_OK, 3);
}

/* GetDeviceData(this, cbObjectData, rgdod, pdwInOut, flags) */
static void dd_GetDeviceData(cpu_t *c)
{
    DEV(d, 5);
    hle_pump_platform();
    if (!d->acquired) { hle_ret(c, DIERR_NOTACQUIRED, 5); return; }
    uint32_t size = ARG(1), max = gmem_rd32(ARG(3)), peek = ARG(4) & 1u;
    static int logged[2];
    if (!logged[d->dev]) { logged[d->dev] = 1; pal_log("dinput: first GetDeviceData on %s (max %lu)", d->dev ? "mouse" : "keyboard", (unsigned long)max); }
    input_event ev[INPUT_BUF];
    if (max > INPUT_BUF) max = INPUT_BUF;
    uint32_t n = input_take(d->dev, ev, max, peek || !ARG(2));
    for (uint32_t i = 0; ARG(2) && i < n; i++) {
        gptr o = ARG(2) + i * size;
        gmem_wr32(o, ev[i].ofs);
        gmem_wr32(o + 4, ev[i].data);
        gmem_wr32(o + 8, ev[i].time);
        gmem_wr32(o + 12, ev[i].seq);
    }
    gmem_wr32(ARG(3), n);
    hle_ret(c, DI_OK, 5);
}

/* GetCapabilities(this, DIDEVCAPS*) */
static void dd_GetCapabilities(cpu_t *c)
{
    DEV(d, 2);
    gptr p = ARG(1);
    gmem_wr32(p + 4, 1);                                     /* DIDC_ATTACHED */
    gmem_wr32(p + 8, d->dev == 0 ? 0x13u : 0x12u);           /* DIDEVTYPE_KEYBOARD / MOUSE */
    gmem_wr32(p + 12, d->dev == 0 ? 0u : 3u);                /* axes */
    gmem_wr32(p + 16, d->dev == 0 ? 128u : 3u);              /* buttons */
    hle_ret(c, DI_OK, 2);
}
static void dd_Poll(cpu_t *c) { hle_ret(c, DI_OK, 1); }

static const com_method m_di[] = {
    COM_IUNKNOWN, {"CreateDevice", di_CreateDevice}, {"EnumDevices", di_EnumDevices}, {"GetDeviceStatus", NULL},
    {"RunControlPanel", NULL}, {"Initialize", NULL},
};
#define DEV_METHODS \
    COM_IUNKNOWN, {"GetCapabilities", dd_GetCapabilities}, {"EnumObjects", NULL}, {"GetProperty", NULL}, \
    {"SetProperty", dd_SetProperty}, {"Acquire", dd_Acquire}, {"Unacquire", dd_Unacquire}, \
    {"GetDeviceState", dd_GetDeviceState}, {"GetDeviceData", dd_GetDeviceData}, {"SetDataFormat", dd_SetDataFormat}, \
    {"SetEventNotification", NULL}, {"SetCooperativeLevel", dd_SetCooperativeLevel}, {"GetObjectInfo", NULL}, \
    {"GetDeviceInfo", NULL}, {"RunControlPanel", NULL}, {"Initialize", NULL}
static const com_method m_dev[] = {DEV_METHODS};
static const com_method m_dev2[] = {DEV_METHODS, {"CreateEffect", NULL}, {"EnumEffects", NULL}, {"GetEffectInfo", NULL},
                                    {"GetForceFeedbackState", NULL}, {"SendForceFeedbackCommand", NULL},
                                    {"EnumCreatedEffectObjects", NULL}, {"Escape", NULL}, {"Poll", dd_Poll},
                                    {"SendDeviceData", NULL}};

#define IFACE(var, name, iid, m) com_iface var = {name, iid, m, (int)(sizeof m / sizeof m[0]), 0}
IFACE(IF_DInput, "IDirectInputA", 0x89521360u, m_di);
IFACE(IF_DIDevice, "IDirectInputDeviceA", 0x5944e680u, m_dev);
IFACE(IF_DIDevice2, "IDirectInputDevice2A", 0x5944e682u, m_dev2);

static const hle_def defs[] = {{"DINPUT.dll", "DirectInputCreateA", d_DirectInputCreateA}};

void hle_dinput_init(void)
{
    input_reset();
    hle_register(defs, 1);
    com_register(&IF_DInput);
    com_register(&IF_DIDevice);
    com_register(&IF_DIDevice2);
}
