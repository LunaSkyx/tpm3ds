/* Differential test for source/core/native_imuse.c against the lifted IMUSE
 * decoder (imuse_00922630_ref, compiled with TPM_OVERRIDE_REF).
 *
 * Each case: random decoder state (step indices also outside 0..88, keep or
 * reset flag), random bit stream, 0..3 channels, 0..300 samples, and in some
 * cases the one-time table flag cleared with a garbage table. Both versions
 * run on identical copies; registers, eflags/lazy flags, the stack window
 * (locals, saved registers, argument slots), state, output, source and the
 * table region must match exactly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/gheap.h"
#include "core/guest.h"
#include "core/gmem.h"
#include "pal/pal.h"
#include "gen/imuse_gen.h"

#define BUF_SZ  0x4000u
#define TAB_LO  0x93faf0u
#define TAB_SZ  (89u * 0x80u + 0x100u)
#define FLAG    0x942770u

static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x ? x : 0x2468aceu;
}

static uint8_t g_buf0[BUF_SZ], g_buf1[BUF_SZ], g_tab0[TAB_SZ], g_tab1[TAB_SZ];

int main(int argc, char **argv)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/GAMEDATA", argc > 1 ? argv[1] : "game_files");
    guest_config cfg = {0};
    cfg.data_dir = dir;
    cfg.save_dir = "build-host/save-test";
    cfg.max_ms = 5000;
    if (!guest_init(&cfg)) { printf("SKIP test_imuse: game files not found under %s\n", dir); return 0; }
    gptr buf = gheap_alloc(BUF_SZ);
    uint8_t *b = gmem_span(buf, BUF_SZ, 1), *tab = gmem_span(TAB_LO, TAB_SZ, 1);
    if (!buf || !b || !tab) { printf("FAIL: guest memory\n"); return 1; }
    /* layout in buf: stack 0x0000..0x0fff (esp at 0x0800), state 0x1000, src 0x1100.., out 0x2000.. */
    const gptr st = buf + 0x1000u, src = buf + 0x1100u, out = buf + 0x2000u, esp = buf + 0x0800u;
    uint32_t seed = 0x1234567u, pass = 0, fail = 0;
    for (unsigned it = 0; it < 20000 && fail < 5; it++) {
        for (uint32_t k = 0; k < BUF_SZ; k++) b[k] = (uint8_t)rnd(&seed);
        uint32_t nch = rnd(&seed) % 4u, ns = rnd(&seed) % 301u;
        if (nch == 3u && (rnd(&seed) & 1u)) nch = 2u;
        for (uint32_t c = 0; c < 4u; c++) {
            uint32_t r = rnd(&seed);
            b[0x1000u + c] = (r & 7u) ? (uint8_t)(r % 89u) : (uint8_t)(r >> 8);   /* sometimes out of range */
        }
        int reinit = (rnd(&seed) % 50u) == 0u;
        if (reinit) { gmem_wr32(FLAG, 0); for (uint32_t k = 0; k < 89u * 0x80u; k++) tab[k] = (uint8_t)rnd(&seed); }
        else gmem_wr32(FLAG, 1);
        gmem_wr32(esp, 0xdeadbeefu);                       /* return address */
        gmem_wr32(esp + 4u, st);
        gmem_wr32(esp + 8u, out);
        gmem_wr32(esp + 12u, src);
        gmem_wr32(esp + 16u, ns);
        gmem_wr32(esp + 20u, nch);
        gmem_wr32(esp + 24u, rnd(&seed) & 1u);
        cpu_t c0;
        memset(&c0, 0, sizeof c0);
        for (int r = 0; r < 8; r++) c0.r[r] = rnd(&seed);
        c0.r[ESP] = esp;
        c0.eflags = rnd(&seed) & 0x8d5u;
        c0.fop = (uint8_t)(rnd(&seed) % 4u); c0.fa = rnd(&seed); c0.fb = rnd(&seed); c0.fr = rnd(&seed);
        uint32_t fl0 = gmem_rd32(FLAG);
        memcpy(g_buf0, b, BUF_SZ); memcpy(g_tab0, tab, TAB_SZ);
        cpu_t ca = c0, cb = c0;
        imuse_00922630_ref(&ca);
        memcpy(g_buf1, b, BUF_SZ); memcpy(g_tab1, tab, TAB_SZ);
        uint32_t fl1 = gmem_rd32(FLAG);
        memcpy(b, g_buf0, BUF_SZ); memcpy(tab, g_tab0, TAB_SZ); gmem_wr32(FLAG, fl0);
        imuse_00922630(&cb);
        int bad = 0;
        for (int r = 0; r < 8; r++)
            if (ca.r[r] != cb.r[r]) { printf("FAIL it %u: reg %d %08x/%08x (nch %u ns %u)\n", it, r, ca.r[r], cb.r[r], nch, ns); bad = 1; }
        if (ca.eflags != cb.eflags || ca.fop != cb.fop || ca.fa != cb.fa || ca.fb != cb.fb || ca.fr != cb.fr) {
            printf("FAIL it %u: flags\n", it); bad = 1;
        }
        for (uint32_t k = 0; k < BUF_SZ && !bad; k++)
            if (b[k] != g_buf1[k]) { printf("FAIL it %u: buf+%04x %02x/%02x (nch %u ns %u)\n", it, k, g_buf1[k], b[k], nch, ns); bad = 1; }
        for (uint32_t k = 0; k < TAB_SZ && !bad; k++)
            if (tab[k] != g_tab1[k]) { printf("FAIL it %u: table+%04x\n", it, k); bad = 1; }
        if (gmem_rd32(FLAG) != fl1) { printf("FAIL it %u: init flag\n", it); bad = 1; }
        if (bad) fail++; else pass++;
    }
    guest_fini();
    printf("%s test_imuse: %u passed, %u failed\n", fail ? "FAIL" : "PASS", pass, fail);
    return fail != 0;
}
