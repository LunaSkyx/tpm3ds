/* USER32 / GDI32 / WINMM / ADVAPI32 / OLE32 HLE.
 * One top-level window; messages go through a small queue and are delivered to
 * the guest window procedure with guest_callback(). The screen is 640x480. */
#include <stdio.h>
#include <string.h>
#include "core/video/video.h"
#include "core/guest.h"
#include "core/gthread.h"
#include "core/hle/hle.h"
#include "core/hle/hle_handles.h"
#include "core/hle/hle_user32.h"
#include "pal/pal.h"

#define HWND_MAIN   0x00001234u
#define WM_DESTROY  0x0002u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_CLOSE    0x0010u
#define WM_QUIT     0x0012u
#define WM_ACTIVATEAPP 0x001cu

typedef struct { uint32_t hwnd, msg, wp, lp; } qmsg;

#define QCAP 64
static qmsg     g_q[QCAP];
static uint32_t g_qhead, g_qtail;
static gptr     g_wndproc;
static int      g_window_alive, g_quit_posted;
static uint32_t g_quit_code;

void hle_post_message(uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (g_qtail - g_qhead >= QCAP) { pal_log("user32: message queue full, dropping %04lx", (unsigned long)msg); return; }
    g_q[g_qtail++ % QCAP] = (qmsg){HWND_MAIN, msg, wp, lp};
}

static uint32_t send(cpu_t *c, uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (!g_wndproc) return 0;
    uint32_t a[4] = {HWND_MAIN, msg, wp, lp};
    return guest_callback(c, g_wndproc, 4, a);
}

static void write_msg(gptr m, const qmsg *q)
{
    gmem_wr32(m + 0, q->hwnd);
    gmem_wr32(m + 4, q->msg);
    gmem_wr32(m + 8, q->wp);
    gmem_wr32(m + 12, q->lp);
    gmem_wr32(m + 16, pal_time_ms());
    gmem_wr32(m + 20, 0);
    gmem_wr32(m + 24, 0);
}

/* Returns 1 and fills *out if a message is available. WM_QUIT comes last. */
static int take(qmsg *out, int remove)
{
    hle_pump_platform();
    if (g_qhead != g_qtail) {
        *out = g_q[g_qhead % QCAP];
        if (remove) g_qhead++;
        return 1;
    }
    if (g_quit_posted) {
        *out = (qmsg){0, WM_QUIT, g_quit_code, 0};
        if (remove) g_quit_posted = 0;
        return 1;
    }
    return 0;
}

static void u_PeekMessageA(cpu_t *c)
{
    gthread_yield();                /* a polling point: other guest threads run here */
    qmsg q;
    guest_watchdog(c);
    int got = take(&q, (ARG(4) & 1u) != 0);
    if (got) write_msg(ARG(0), &q);
    hle_ret(c, (uint32_t)got, 5);
}

static void u_GetMessageA(cpu_t *c)
{
    qmsg q;
    while (!take(&q, 1)) {          /* blocks like the real call; the watchdog bounds it */
        guest_watchdog(c);
        gthread_sleep(1);
    }
    write_msg(ARG(0), &q);
    hle_ret(c, q.msg != WM_QUIT, 4);
}

static void u_DispatchMessageA(cpu_t *c)
{
    gptr m = ARG(0);
    uint32_t r = gmem_rd32(m) == HWND_MAIN ? send(c, gmem_rd32(m + 4), gmem_rd32(m + 8), gmem_rd32(m + 12)) : 0;
    hle_ret(c, r, 1);
}
static void u_SendMessageA(cpu_t *c)
{
    uint32_t r = ARG(0) == HWND_MAIN ? send(c, ARG(1), ARG(2), ARG(3)) : 0;
    hle_ret(c, r, 4);
}
static void u_PostQuitMessage(cpu_t *c) { g_quit_posted = 1; g_quit_code = ARG(0); hle_ret(c, 0, 1); }
static void u_DefWindowProcA(cpu_t *c)
{
    uint32_t msg = ARG(1);
    if (msg == WM_CLOSE && g_window_alive) { g_window_alive = 0; send(c, WM_DESTROY, 0, 0); }
    hle_ret(c, 0, 4);
}
static void u_DestroyWindow(cpu_t *c)
{
    if (ARG(0) == HWND_MAIN && g_window_alive) { g_window_alive = 0; send(c, WM_DESTROY, 0, 0); }
    hle_ret(c, 1, 1);
}

static void u_FindWindowA(cpu_t *c) { hle_ret(c, 0, 2); }
static void u_RegisterClassA(cpu_t *c)
{
    g_wndproc = gmem_rd32(ARG(0) + 4);
    char name[64];
    hle_str_in(name, sizeof name, gmem_rd32(ARG(0) + 36));
    pal_log("user32: RegisterClassA(%s) wndproc %08lx", name, (unsigned long)g_wndproc);
    hle_ret(c, 0xc001u, 1);
}
static void u_CreateWindowExA(cpu_t *c)
{
    g_window_alive = 1;
    pal_log("user32: CreateWindowExA style %08lx %lux%lu", (unsigned long)ARG(3), (unsigned long)ARG(6),
            (unsigned long)ARG(7));
    hle_ret(c, HWND_MAIN, 12);
}
static void u_ShowWindow(cpu_t *c)
{
    hle_post_message(WM_ACTIVATEAPP, 1, 0);
    hle_post_message(WM_ACTIVATE, 1, 0);
    hle_post_message(WM_SETFOCUS, 0, 0);
    hle_ret(c, 0, 2);
}
static void u_GetSystemMetrics(cpu_t *c)
{
    uint32_t i = ARG(0), v = 0;
    switch (i) {
    case 0: v = g_screen_w; break;   /* SM_CXSCREEN */
    case 1: v = g_screen_h; break;   /* SM_CYSCREEN */
    case 15: v = 19; break;          /* SM_CYMENU */
    case 32: case 33: v = 4; break;  /* SM_CXFRAME, SM_CYFRAME */
    case 4: v = 19; break;           /* SM_CYCAPTION */
    }
    hle_ret(c, v, 1);
}
static void u_MessageBoxA(cpu_t *c)
{
    char text[256], cap[96];
    hle_str_in(text, sizeof text, ARG(1));
    hle_str_in(cap, sizeof cap, ARG(2));
    pal_log("user32: MessageBox '%s': %s", cap, text);
    pal_log_flush();
    hle_ret(c, 1, 4);                    /* IDOK */
}
static void u_GetKeyNameTextA(cpu_t *c)
{
    char name[32];
    hle_key_name((ARG(0) >> 16) & 0x1ffu, name, sizeof name);
    hle_str_out(ARG(1), ARG(2), name);
    hle_ret(c, (uint32_t)strlen(name), 3);
}
static void u_GetKeyboardState(cpu_t *c)
{
    const uint8_t *vk = hle_vk_state();
    for (uint32_t k = 0; k < 256; k++) gmem_wr8(ARG(0) + k, vk[k]);
    hle_ret(c, 1, 1);
}
static void u_GetDC(cpu_t *c) { hle_ret(c, 0x0000dc01u, 1); }
static void u_ret0_0(cpu_t *c) { hle_ret(c, 0, 0); }
static void u_ret0_1(cpu_t *c) { hle_ret(c, 0, 1); }
static void u_ret0_2(cpu_t *c) { hle_ret(c, 0, 2); }
static void u_ret0_3(cpu_t *c) { hle_ret(c, 0, 3); }
static void u_ret0_4(cpu_t *c) { hle_ret(c, 0, 4); }
static void u_ret1_1(cpu_t *c) { hle_ret(c, 1, 1); }
static void u_ret1_2(cpu_t *c) { hle_ret(c, 1, 2); }
static void u_ret1_4(cpu_t *c) { hle_ret(c, 1, 4); }
static void u_ret1_7(cpu_t *c) { hle_ret(c, 1, 7); }
static void u_LoadRes(cpu_t *c) { hle_ret(c, 0x0000a000u + (ARG(1) & 0xfffu), 2); }

/* ---- GDI32: the game draws through DirectDraw; GDI is only used for the
 * palette/DIB helpers. Objects are opaque fake handles. */
static uint32_t g_gdi_next = 0x00006000u;
static void g_newobj(cpu_t *c, int nargs) { hle_ret(c, g_gdi_next++, nargs); }
static void g_GetStockObject(cpu_t *c) { hle_ret(c, 0x00005f00u + ARG(0), 1); }
static void g_CreateCompatibleDC(cpu_t *c) { g_newobj(c, 1); }
static void g_CreatePalette(cpu_t *c) { g_newobj(c, 1); }
static void g_CreateDIBSection(cpu_t *c)
{
    pal_log("gdi32: CreateDIBSection not implemented");
    if (ARG(3)) gmem_wr32(ARG(3), 0);
    hle_ret(c, 0, 6);
}
static void g_GetSystemPaletteEntries(cpu_t *c)
{
    for (uint32_t k = 0; k < ARG(2); k++) gmem_wr32(ARG(3) + 4 * k, 0);
    hle_ret(c, ARG(2), 4);
}
static void g_GetTextExtentPoint32A(cpu_t *c)
{
    gmem_wr32(ARG(3), 8 * ARG(2));
    gmem_wr32(ARG(3) + 4, 16);
    hle_ret(c, 1, 4);
}

/* ---- WINMM: no joystick, no aux/CD audio devices ---- */
static void w_joyGetNumDevs(cpu_t *c) { hle_ret(c, 0, 0); }
static void w_joyGetDevCapsA(cpu_t *c) { hle_ret(c, 165, 3); }   /* JOYERR_UNPLUGGED... */
static void w_joyGetPosEx(cpu_t *c) { hle_ret(c, 167, 2); }       /* JOYERR_UNPLUGGED */
static void w_auxGetNumDevs(cpu_t *c) { hle_ret(c, 0, 0); }
static void w_auxGetDevCapsA(cpu_t *c) { hle_ret(c, 2, 3); }      /* MMSYSERR_BADDEVICEID */
static void w_auxGetVolume(cpu_t *c) { hle_ret(c, 2, 2); }
static void w_auxSetVolume(cpu_t *c) { hle_ret(c, 2, 2); }
static void w_mciSendCommandA(cpu_t *c)
{
    pal_log("winmm: mciSendCommandA(dev %08lx, msg %04lx, flags %08lx) -> no device",
            (unsigned long)ARG(0), (unsigned long)ARG(1), (unsigned long)ARG(2));
    hle_ret(c, 275, 4);                  /* MCIERR_INVALID_DEVICE_NAME */
}

/* ---- ADVAPI32: registry. The exe reads one key, written by the installer; its
 * only consumers are "CD Path" (FUN_0043f020, appends "\gamedata\") and "JoystickID"
 * (FUN_0048d7c2, only with a joystick). guest_path() strips everything up to
 * "gamedata\", so the CD path value only has to be well-formed. */
#define HKEY_TPM 0x0000c0deu
static const char k_tpm_key[] = "Software\\LucasArts Entertainment Company LLC\\The Phantom Menace\\v1.0";
static const struct { const char *name; uint32_t type; const char *sz; } k_tpm_values[] = {
    {"CD Path", 1, "C:\\TPM"},
};
#define N_TPM_VALUES (sizeof k_tpm_values / sizeof k_tpm_values[0])

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20)) return 0;
    return *a == *b;
}

static void a_RegOpenKeyExA(cpu_t *c)
{
    char key[200];
    hle_str_in(key, sizeof key, ARG(1));
    int ok = ARG(0) == 0x80000002u && ci_eq(key, k_tpm_key);
    pal_log("advapi32: RegOpenKeyExA(%08lx, %s) -> %s", (unsigned long)ARG(0), key, ok ? "ok" : "not found");
    if (ok) gmem_wr32(ARG(4), HKEY_TPM);
    hle_ret(c, ok ? 0u : 2u, 5);
}

static uint32_t put_value(uint32_t i, gptr type, gptr data, gptr cbdata)
{
    uint32_t n = (uint32_t)strlen(k_tpm_values[i].sz) + 1;
    if (type) gmem_wr32(type, k_tpm_values[i].type);
    if (cbdata) {
        uint32_t cap = gmem_rd32(cbdata);
        gmem_wr32(cbdata, n);
        if (data && cap < n) return 234;                    /* ERROR_MORE_DATA */
    }
    if (data) hle_str_out(data, n, k_tpm_values[i].sz);
    return 0;
}

/* RegEnumValueA(hkey, index, name, &cbname, reserved, &type, data, &cbdata) */
static void a_RegEnumValueA(cpu_t *c)
{
    uint32_t i = ARG(1);
    if (ARG(0) != HKEY_TPM || i >= N_TPM_VALUES) { hle_ret(c, 259, 8); return; }   /* ERROR_NO_MORE_ITEMS */
    uint32_t nl = (uint32_t)strlen(k_tpm_values[i].name);
    if (gmem_rd32(ARG(3)) <= nl) { hle_ret(c, 234, 8); return; }
    hle_str_out(ARG(2), nl + 1, k_tpm_values[i].name);
    gmem_wr32(ARG(3), nl);
    hle_ret(c, put_value(i, ARG(5), ARG(6), ARG(7)), 8);
}

/* RegQueryValueExA(hkey, name, reserved, &type, data, &cbdata) */
static void a_RegQueryValueExA(cpu_t *c)
{
    char name[128];
    hle_str_in(name, sizeof name, ARG(1));
    for (uint32_t i = 0; ARG(0) == HKEY_TPM && i < N_TPM_VALUES; i++)
        if (ci_eq(name, k_tpm_values[i].name)) { hle_ret(c, put_value(i, ARG(3), ARG(4), ARG(5)), 6); return; }
    pal_log("advapi32: RegQueryValueExA(%s) -> not found", name);
    hle_ret(c, 2, 6);
}
static void a_RegCloseKey(cpu_t *c) { hle_ret(c, 0, 1); }

/* ---- OLE32 ---- */
static void o_CoInitialize(cpu_t *c) { hle_ret(c, 0, 1); }
static void o_CoUninitialize(cpu_t *c) { hle_ret(c, 0, 0); }
static void o_CoCreateInstance(cpu_t *c)
{
    pal_log("ole32: CoCreateInstance clsid %08lx -> class not registered", (unsigned long)gmem_rd32(ARG(0)));
    if (ARG(4)) gmem_wr32(ARG(4), 0);
    hle_ret(c, 0x80040154u, 5);
}

/* ---- window queries (BINKW32 checks where its window is) ---- */
static void put_rect(gptr r, uint32_t l, uint32_t t, uint32_t rr, uint32_t b)
{
    gmem_wr32(r, l); gmem_wr32(r + 4, t); gmem_wr32(r + 8, rr); gmem_wr32(r + 12, b);
}
static void u_GetWindowRect(cpu_t *c) { put_rect(ARG(1), 0, 0, g_screen_w, g_screen_h); hle_ret(c, ARG(0) == HWND_MAIN, 2); }
static void u_GetClientRect(cpu_t *c) { put_rect(ARG(1), 0, 0, g_screen_w, g_screen_h); hle_ret(c, ARG(0) == HWND_MAIN, 2); }
static void u_GetCursorPos(cpu_t *c) { gmem_wr32(ARG(0), g_screen_w / 2); gmem_wr32(ARG(0) + 4, g_screen_h / 2); hle_ret(c, 1, 1); }
static void u_GetActiveWindow(cpu_t *c) { hle_ret(c, g_window_alive ? HWND_MAIN : 0, 0); }
static void u_GetTopWindow(cpu_t *c) { hle_ret(c, g_window_alive ? HWND_MAIN : 0, 1); }
static void u_GetWindowThreadProcessId(cpu_t *c)
{
    if (ARG(1)) gmem_wr32(ARG(1), 0x1000);
    hle_ret(c, 0x1004, 2);
}
static void u_IsWindowVisible(cpu_t *c) { hle_ret(c, ARG(0) == HWND_MAIN && g_window_alive, 1); }
static int g_cursor_count;
static void u_ShowCursor(cpu_t *c) { g_cursor_count += ARG(0) ? 1 : -1; hle_ret(c, (uint32_t)g_cursor_count, 1); }
static void u_BeginPaint(cpu_t *c) { for (uint32_t k = 0; k < 64; k += 4) gmem_wr32(ARG(1) + k, 0); hle_ret(c, 0x2001, 2); }
/* GetDeviceCaps(hdc, index): the 640x480x16 screen */
static void g_GetDeviceCaps(cpu_t *c)
{
    uint32_t v;
    switch (ARG(1)) {
    case 8: v = g_screen_w; break;   /* HORZRES */
    case 10: v = g_screen_h; break;  /* VERTRES */
    case 12: v = 16; break;      /* BITSPIXEL */
    case 14: v = 1; break;       /* PLANES */
    case 24: v = 0xffffffffu; break;   /* NUMCOLORS: more than 256 */
    case 38: v = 0; break;       /* RASTERCAPS: no palette */
    default: v = 0; pal_log("gdi32: GetDeviceCaps(%lu) -> 0", (unsigned long)ARG(1)); break;
    }
    hle_ret(c, v, 2);
}

static const hle_def defs[] = {
    {"USER32.dll", "GetWindowRect", u_GetWindowRect},
    {"USER32.dll", "GetClientRect", u_GetClientRect},
    {"USER32.dll", "GetCursorPos", u_GetCursorPos},
    {"USER32.dll", "GetActiveWindow", u_GetActiveWindow},
    {"USER32.dll", "GetTopWindow", u_GetTopWindow},
    {"USER32.dll", "GetWindow", u_ret0_2},
    {"USER32.dll", "GetWindowLongA", u_ret0_2},
    {"USER32.dll", "GetClassLongA", u_ret0_2},
    {"USER32.dll", "GetWindowThreadProcessId", u_GetWindowThreadProcessId},
    {"USER32.dll", "IsWindowVisible", u_IsWindowVisible},
    {"USER32.dll", "ClientToScreen", u_ret1_2},
    {"USER32.dll", "ScreenToClient", u_ret1_2},
    {"USER32.dll", "ShowCursor", u_ShowCursor},
    {"USER32.dll", "UnregisterClassA", u_ret1_2},
    {"USER32.dll", "BeginPaint", u_BeginPaint},
    {"USER32.dll", "EndPaint", u_ret1_2},
    {"GDI32.dll", "GetDeviceCaps", g_GetDeviceCaps},
    {"GDI32.dll", "GetPixel", u_ret0_3},
    {"GDI32.dll", "SetPixel", u_ret0_4},
    {"USER32.dll", "PeekMessageA", u_PeekMessageA},
    {"USER32.dll", "GetMessageA", u_GetMessageA},
    {"USER32.dll", "DispatchMessageA", u_DispatchMessageA},
    {"USER32.dll", "TranslateMessage", u_ret0_1},
    {"USER32.dll", "IsDialogMessageA", u_ret0_2},
    {"USER32.dll", "SendMessageA", u_SendMessageA},
    {"USER32.dll", "PostQuitMessage", u_PostQuitMessage},
    {"USER32.dll", "DefWindowProcA", u_DefWindowProcA},
    {"USER32.dll", "DestroyWindow", u_DestroyWindow},
    {"USER32.dll", "FindWindowA", u_FindWindowA},
    {"USER32.dll", "RegisterClassA", u_RegisterClassA},
    {"USER32.dll", "CreateWindowExA", u_CreateWindowExA},
    {"USER32.dll", "ShowWindow", u_ShowWindow},
    {"USER32.dll", "UpdateWindow", u_ret1_1},
    {"USER32.dll", "SetWindowLongA", u_ret0_3},
    {"USER32.dll", "SetWindowPos", u_ret1_7},
    {"USER32.dll", "GetSystemMetrics", u_GetSystemMetrics},
    {"USER32.dll", "SystemParametersInfoA", u_ret1_4},
    {"USER32.dll", "MessageBoxA", u_MessageBoxA},
    {"USER32.dll", "GetKeyNameTextA", u_GetKeyNameTextA},
    {"USER32.dll", "GetKeyboardState", u_GetKeyboardState},
    {"USER32.dll", "LoadIconA", u_LoadRes},
    {"USER32.dll", "LoadCursorA", u_LoadRes},
    {"USER32.dll", "SetCursor", u_ret0_1},
    {"USER32.dll", "SetCursorPos", u_ret1_2},
    {"USER32.dll", "SetCapture", u_ret0_1},
    {"USER32.dll", "ReleaseCapture", u_ret0_0},
    {"USER32.dll", "GetDC", u_GetDC},
    {"USER32.dll", "ReleaseDC", u_ret1_2},
    {"GDI32.dll", "GetStockObject", g_GetStockObject},
    {"GDI32.dll", "CreateCompatibleDC", g_CreateCompatibleDC},
    {"GDI32.dll", "CreatePalette", g_CreatePalette},
    {"GDI32.dll", "CreateDIBSection", g_CreateDIBSection},
    {"GDI32.dll", "GetSystemPaletteEntries", g_GetSystemPaletteEntries},
    {"GDI32.dll", "GetTextExtentPoint32A", g_GetTextExtentPoint32A},
    {"GDI32.dll", "SelectObject", u_ret0_2},
    {"GDI32.dll", "DeleteObject", u_ret1_1},
    {"GDI32.dll", "DeleteDC", u_ret1_1},
    {"GDI32.dll", "SelectPalette", u_ret0_3},
    {"GDI32.dll", "RealizePalette", u_ret0_1},
    {"GDI32.dll", "AnimatePalette", u_ret1_4},
    {"GDI32.dll", "SetDIBColorTable", u_ret0_0},
    {"GDI32.dll", "GdiFlush", u_ret1_1},
    {"WINMM.dll", "joyGetNumDevs", w_joyGetNumDevs},
    {"WINMM.dll", "joyGetDevCapsA", w_joyGetDevCapsA},
    {"WINMM.dll", "joyGetPosEx", w_joyGetPosEx},
    {"WINMM.dll", "auxGetNumDevs", w_auxGetNumDevs},
    {"WINMM.dll", "auxGetDevCapsA", w_auxGetDevCapsA},
    {"WINMM.dll", "auxGetVolume", w_auxGetVolume},
    {"WINMM.dll", "auxSetVolume", w_auxSetVolume},
    {"WINMM.dll", "mciSendCommandA", w_mciSendCommandA},
    {"ADVAPI32.dll", "RegOpenKeyExA", a_RegOpenKeyExA},
    {"ADVAPI32.dll", "RegQueryValueExA", a_RegQueryValueExA},
    {"ADVAPI32.dll", "RegEnumValueA", a_RegEnumValueA},
    {"ADVAPI32.dll", "RegCloseKey", a_RegCloseKey},
    {"ole32.dll", "CoInitialize", o_CoInitialize},
    {"ole32.dll", "CoUninitialize", o_CoUninitialize},
    {"ole32.dll", "CoCreateInstance", o_CoCreateInstance},
};

void hle_user32_init(void)
{
    g_qhead = g_qtail = 0;
    g_wndproc = 0;
    g_window_alive = g_quit_posted = 0;
    hle_register(defs, (int)(sizeof defs / sizeof defs[0]));
}
