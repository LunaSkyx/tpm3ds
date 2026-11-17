/* Runs lifted WMAIN code on the host. Regression tests for lifter bugs:
 *  - switch-table case labels were emitted as "jump to non-code" traps
 *    (CRT _output, reached through _vsnprintf 0x49b220)
 *  - EFLAGS returned from CRT asm helpers were dropped at "ret"
 *    (_CIsin 0x49c7f0 gave NaN)
 *  - functions with code below their entry started at the lowest address
 *    (strchr 0x49adb0 shares a tail at 0x49ada0)
 *  - jump tables not starting at index 0 / bounded by "jb" became calls to
 *    unknown addresses (memmove 0x49bd30) */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "core/gheap.h"
#include "core/guest.h"
#include "pal/pal.h"

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static gptr gstr(const char *s)
{
    gptr p = gheap_alloc((uint32_t)strlen(s) + 1);
    for (uint32_t k = 0; s[k] || (gmem_wr8(p + k, 0), 0); k++) gmem_wr8(p + k, (uint8_t)s[k]);
    return p;
}

int main(int argc, char **argv)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/GAMEDATA", argc > 1 ? argv[1] : "game_files");
    guest_config cfg = {0};
    cfg.data_dir = dir;
    cfg.save_dir = "build-host/save-test";
    cfg.max_ms = 5000;
    if (!guest_init(&cfg)) { printf("SKIP test_guest: WMAIN.EXE not found under %s\n", dir); return 0; }
    cpu_t *c = guest_cpu();
    int why;

    /* vsnprintf(buf, 64, "%d-%s-%x|%c", va). (%f needs the CRT's _cinit, not run here.) */
    gptr buf = gheap_alloc(64), va = gheap_alloc(32), ab = gstr("ab");
    gmem_wr32(va, (uint32_t)-42);
    gmem_wr32(va + 4, ab);
    gmem_wr32(va + 8, 0xff);
    gmem_wr32(va + 12, 'Z');
    uint32_t args[4] = {buf, 64, gstr("%d-%s-%x|%c"), va};
    uint32_t n = guest_invoke(0x0049b220, 4, args, &why);
    char out[64];
    gmem_strcpy_out(out, sizeof out, buf);
    CHECK(why == GUEST_RUNNING, "vsnprintf stopped the guest (%d: %s)", why, guest_stop_text());
    CHECK(!strcmp(out, "-42-ab-ff|Z") && n == 11, "vsnprintf gave '%s' (%u)", out, (unsigned)n);

    /* _CIsin: argument and result in ST(0) */
    double xs[] = {0.0, 0.5, -2.0, 10.0};
    for (unsigned k = 0; k < sizeof xs / sizeof xs[0]; k++) {
        FPUSH(c, xs[k]);
        guest_invoke(0x0049c7f0, 0, NULL, &why);
        double r = ST(0);
        FPOP(c);
        CHECK(why == GUEST_RUNNING && fabs(r - sin(xs[k])) < 1e-12, "sin(%g) = %g (stop %d)", xs[k], r, why);
    }
    CHECK(c->ftop == 0, "x87 stack unbalanced: top %u", (unsigned)c->ftop);

    /* strchr: both the unaligned head loop and the word loop */
    gptr hay = gstr("abcdefghijklmnop");
    const char needles[] = {'a', 'c', 'h', 'p', 'z', 0};
    for (unsigned k = 0; k < sizeof needles; k++) {
        uint32_t sargs[2] = {hay + 1, (uint32_t)(uint8_t)needles[k]};
        uint32_t r = guest_invoke(0x0049adb0, 2, sargs, &why);
        const char *h = "abcdefghijklmnop", *e = strchr(h + 1, needles[k]);
        uint32_t want = e ? hay + (uint32_t)(e - h) : 0;
        CHECK(why == GUEST_RUNNING && r == want, "strchr('%c') = %08x, want %08x", needles[k] ? needles[k] : '0',
              (unsigned)r, (unsigned)want);
    }

    /* memmove: every length 0..40 and both directions (forward and overlapping backward) */
    gptr mb = gheap_alloc(128);
    int mm_ok = 1;
    for (uint32_t len = 0; len <= 40 && mm_ok; len++)
        for (int dir = 0; dir < 2 && mm_ok; dir++) {
            uint8_t ref[128];
            for (uint32_t k = 0; k < 128; k++) { ref[k] = (uint8_t)(k * 7 + 1); gmem_wr8(mb + k, ref[k]); }
            uint32_t dst = dir ? 13 : 3, src = dir ? 3 : 13;
            memmove(ref + dst, ref + src, len);
            uint32_t margs[3] = {mb + dst, mb + src, len};
            uint32_t r = guest_invoke(0x0049bd30, 3, margs, &why);
            for (uint32_t k = 0; k < 128; k++) if (gmem_rd8(mb + k) != ref[k]) mm_ok = 0;
            if (why != GUEST_RUNNING || r != mb + dst) mm_ok = 0;
            if (!mm_ok) printf("  memmove len %u dir %d failed (stop %d)\n", (unsigned)len, dir, why);
        }
    CHECK(mm_ok, "memmove");

    guest_fini();
    printf("%s test_guest: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail != 0;
}
