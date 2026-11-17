#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "core/gheap.h"
#include "core/gthread.h"
#include "core/guest.h"
#include "core/hle/hle.h"
#include "core/pe_load.h"
#include "core/game_options.h"
#include "pal/pal.h"

#define MAX_MODULES 8

static guest_config g_cfg;
static cpu_t        g_cpu;
static pe_image     g_mods[MAX_MODULES];      /* [0] = WMAIN.EXE, then the lifted DLLs */
static uint32_t     g_nmods;
static guest_fn_entry *g_dispatch;           /* all modules' tables merged, sorted */
#define g_exe (g_mods[0])
static jmp_buf      g_stop_jmp;
static int          g_running;
static uint32_t     g_exit_code, g_deadline;
static char         g_stop_text[160];
static volatile int g_stop_requested;

cpu_t      *guest_cpu(void)        { return gthread_cpu(); }
uint32_t    guest_exit_code(void)  { return g_exit_code; }
const char *guest_stop_text(void)  { return g_stop_text; }
int         guest_trace_level(void){ return g_cfg.trace; }
void        guest_request_stop(void) { g_stop_requested = 1; }

void guest_stop(cpu_t *c, guest_stop_reason r, const char *why)
{
    if (g_running && gthread_current() != 0) {
        pal_log("guest: STOP on thread %d reason=%d '%s'", gthread_current(), (int)r, why ? why : "");
        gthread_stop_from_thread((int)r, why);
    }
    snprintf(g_stop_text, sizeof g_stop_text, "%s", why ? why : "");
    pal_log("guest: STOP reason=%d '%s' eip~ret=%08lx esp=%08lx", (int)r, g_stop_text,
            (unsigned long)gmem_rd32(c->r[ESP]), (unsigned long)c->r[ESP]);
    pal_log_flush();
    if (!g_running) { pal_log("guest: stop outside guest_run"); for (;;) {} }
    longjmp(g_stop_jmp, (int)r);
}

void guest_watchdog(cpu_t *c)
{
    if (g_stop_requested) guest_stop(c, GUEST_STOP_REQUESTED, "stop requested");
    if (g_deadline && pal_time_ms() > g_deadline) guest_stop(c, GUEST_STOP_WATCHDOG, "wall-clock limit");
    if (g_gmem_faults > 64) guest_stop(c, GUEST_STOP_FAULTS, "too many memory faults");
}

const char *guest_data_dir(void) { return g_cfg.data_dir; }
const char *guest_save_dir(void) { return g_cfg.save_dir; }
const char *guest_install_dir(void) { return g_cfg.install_dir; }

/* Heuristic backtrace: dwords on the stack that point just behind a call instruction. */
void guest_backtrace(const char *why)
{
    char line[200];
    int w = snprintf(line, sizeof line, "guest: %s; stack:", why);
    uint32_t esp = g_cpu.r[ESP], found = 0;
    for (uint32_t k = 0; k < 1024 && found < 10 && w < (int)sizeof line - 12; k += 4) {
        uint32_t v = gmem_ok(esp + k, 4) ? gmem_rd32(esp + k) : 0;
        if (v < GUEST_EXE_BASE + 0x1000 || v >= GUEST_EXE_BASE + g_exe.size) continue;
        uint8_t b5 = gmem_rd8(v - 5), b6 = gmem_rd8(v - 6), b3 = gmem_rd8(v - 3), b2 = gmem_rd8(v - 2);
        if (b5 == 0xe8 || (b6 == 0xff && (gmem_rd8(v - 5) & 0x38) == 0x10) ||
            (b3 == 0xff && (gmem_rd8(v - 2) & 0x38) == 0x10) || (b2 == 0xff && (gmem_rd8(v - 1) & 0x38) == 0x10)) {
            w += snprintf(line + w, sizeof line - (size_t)w, " %08lx", (unsigned long)v);
            found++;
        }
    }
    pal_log("%s", line);
}

static void on_fault(gptr a, uint32_t n, int write)
{
    (void)a; (void)n; (void)write;
    if (g_gmem_faults <= 4) cpu_log_fn_ring(12);
}

void guest_path(char *out, uint32_t n, const char *gp) { guest_path_in(out, n, g_cfg.data_dir, gp); }

void guest_path_in(char *out, uint32_t n, const char *base, const char *gp)
{
    /* Drop drive letters and absolute install prefixes: everything is resolved
     * relative to the data dir, which mirrors the installed GAMEDATA dir. */
    const char *p = gp;
    if (p[0] && p[1] == ':') p += 2;
    const char *key = NULL;
    for (const char *s = p; *s; s++)
        if ((s[0] == 'G' || s[0] == 'g') && !strncmp(s + 1, "AMEDATA", 7) && (s[8] == '\\' || s[8] == '/')) key = s + 9;
    if (!key) for (const char *s = p; *s; s++)
        if (!strncmp(s, "gamedata", 8) && (s[8] == '\\' || s[8] == '/')) key = s + 9;
    if (key) p = key;
    while (p[0] == '.' && (p[1] == '\\' || p[1] == '/')) p += 2;
    while (*p == '\\' || *p == '/') p++;
    int w = snprintf(out, n, "%s/", base);
    for (; *p && w + 1 < (int)n; p++) out[w++] = *p == '\\' ? '/' : *p;
    out[w] = 0;
}

static void on_extern(cpu_t *c, uint32_t addr)
{
    if (hle_is_thunk(addr)) { hle_dispatch(c, addr); return; }
    char why[64];
    snprintf(why, sizeof why, "call to unknown address %08lx", (unsigned long)addr);
    guest_stop(c, GUEST_STOP_TRAP, why);
}

static void on_trap(cpu_t *c, uint32_t pc)
{
    char why[64];
    snprintf(why, sizeof why, "trap at %08lx", (unsigned long)pc);
    guest_stop(c, GUEST_STOP_TRAP, why);
}

static void setup_tib(void)
{
    gmem_wr32(GUEST_TIB + 0x00, 0xffffffffu);        /* SEH chain end */
    gmem_wr32(GUEST_TIB + 0x04, GUEST_STACK_HI);     /* stack base */
    gmem_wr32(GUEST_TIB + 0x08, GUEST_STACK_LO);     /* stack limit */
    gmem_wr32(GUEST_TIB + 0x18, GUEST_TIB);          /* self */
    gmem_wr32(GUEST_TIB + 0x20, 0x1000);             /* process id */
    gmem_wr32(GUEST_TIB + 0x24, 0x1004);             /* thread id */
    g_cpu.fs_base = GUEST_TIB;
}

const struct pe_image *guest_module_by_name(const char *name)
{
    const char *slash = strrchr(name, '\\');
    if (slash) name = slash + 1;
    for (uint32_t m = 0; m < g_nmods; m++)
        if (!strcasecmp(g_lifted_modules[m].import_name, name) || !strcasecmp(g_lifted_modules[m].file, name))
            return &g_mods[m];
    char with_ext[80];                                  /* LoadLibrary("binkw32") */
    snprintf(with_ext, sizeof with_ext, "%s.dll", name);
    for (uint32_t m = 1; m < g_nmods; m++)
        if (!strcasecmp(g_lifted_modules[m].import_name, with_ext)) return &g_mods[m];
    return NULL;
}

const struct pe_image *guest_module_by_base(uint32_t base)
{
    for (uint32_t m = 0; m < g_nmods; m++)
        if (g_mods[m].base == base) return &g_mods[m];
    return NULL;
}

static int load_modules(void)
{
    g_nmods = 0;
    for (uint32_t m = 0; m < g_lifted_module_count && m < MAX_MODULES; m++) {
        const guest_module *gm = &g_lifted_modules[m];
        char path[512];
        snprintf(path, sizeof path, "%s/BIN/%s", g_cfg.data_dir, gm->file);
        if (!pe_load(path, gm->base, &g_mods[m])) {
            char lower[64];
            snprintf(lower, sizeof lower, "%s", gm->file);
            for (char *q = lower; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
            snprintf(path, sizeof path, "%s/bin/%s", g_cfg.data_dir, lower);
            if (!pe_load(path, gm->base, &g_mods[m])) { pal_log("guest: cannot load %s", gm->file); return 0; }
        }
        g_nmods++;
    }
    if (g_exe.base != GUEST_EXE_BASE) { pal_log("guest: unexpected exe base"); return 0; }
    return 1;
}

/* Imports from a lifted module bind to its export; everything else to an HLE thunk. */
static void resolve_imports(pe_image *img)
{
    for (uint32_t i = 0; i < img->nimports; i++) {
        pe_import *im = &img->imports[i];
        char name[64];
        if (im->by_ordinal) snprintf(name, sizeof name, "#%u", (unsigned)im->ordinal);
        else snprintf(name, sizeof name, "%s", im->name);
        const pe_image *dll = guest_module_by_name(im->dll);
        gptr target = 0;
        if (dll && dll != img) {
            if (im->by_ordinal) {
                for (uint32_t e = 0; e < dll->nexports; e++)
                    if (dll->exports[e].ordinal == im->ordinal) target = dll->exports[e].addr;
            } else target = pe_find_export(dll, name);
            if (!target) pal_log("guest: %s!%s not exported", im->dll, name);
        }
        gmem_wr32(im->iat, target ? target : hle_thunk_for(im->dll, name));
    }
}

static int cmp_entry(const void *a, const void *b)
{
    uint32_t x = ((const guest_fn_entry *)a)->addr, y = ((const guest_fn_entry *)b)->addr;
    return x < y ? -1 : x > y;
}

static int build_dispatch(void)
{
    uint32_t n = 0, k = 0;
    for (uint32_t m = 0; m < g_nmods; m++) n += *g_lifted_modules[m].count;
    free(g_dispatch);
    g_dispatch = malloc((size_t)n * sizeof *g_dispatch);
    if (!g_dispatch) return 0;
    for (uint32_t m = 0; m < g_nmods; m++) {
        memcpy(g_dispatch + k, g_lifted_modules[m].tab, *g_lifted_modules[m].count * sizeof *g_dispatch);
        k += *g_lifted_modules[m].count;
        pal_log("guest: %s: %lu lifted functions", g_lifted_modules[m].file, (unsigned long)*g_lifted_modules[m].count);
    }
    qsort(g_dispatch, n, sizeof *g_dispatch, cmp_entry);
    cpu_set_dispatch(g_dispatch, n);
    return 1;
}

int guest_init(const guest_config *cfg)
{
    g_cfg = *cfg;
    if (!g_cfg.arena_size) g_cfg.arena_size = 64u << 20;   /* fixed: the 3DS build masks guest addresses with this size. Queen level: heap peak 27.6 MB (full), VirtualAlloc peak ~19 MB */
    memset(&g_cpu, 0, sizeof g_cpu);
    cpu_finit(&g_cpu);
    if (!gmem_init(g_cfg.arena_size)) return 0;

    if (!load_modules()) return 0;
    hle_init_all();
    for (uint32_t m = 0; m < g_nmods; m++) resolve_imports(&g_mods[m]);
    pal_log("guest: imports patched, %lu without HLE yet", (unsigned long)hle_missing_count());

    /* heap and VirtualAlloc split the space above the images: three CRTs (exe, Bink,
     * iMUSE) each reserve multi-MB small-block regions through VirtualAlloc */
    uint32_t virt_lo = ((GUEST_HEAP_LO + g_cfg.arena_size) / 2u) & ~0xffffu;
    gheap_init(GUEST_HEAP_LO, virt_lo);
    gvirt_init(virt_lo, g_cfg.arena_size);
    hle_process_init();
    setup_tib();
    gthread_init(&g_cpu);
    if (!build_dispatch()) return 0;
    cpu_set_hooks(on_extern, on_trap);
    g_gmem_fault_hook = on_fault;
    if (g_cfg.hz60) game_option_apply("hz60");
    if (g_cfg.nolimit) game_option_apply("nolimit");
    return 1;
}

int guest_run(void)
{
    g_cpu.r[ESP] = GUEST_STACK_HI - 64;
    g_cpu.r[EBP] = 0;
    g_cpu.eflags = 0x202;
    g_cpu.fop = FL_RAW;
    g_deadline = g_cfg.max_ms ? pal_time_ms() + g_cfg.max_ms : 0;
    g_stop_text[0] = 0;
    g_running = 1;
    int r = setjmp(g_stop_jmp);
    if (r == 0) {
        for (uint32_t m = 1; m < g_nmods; m++) {        /* DllMain(hinst, DLL_PROCESS_ATTACH, 0) */
            uint32_t args[3] = {g_mods[m].base, 1, 0};
            uint32_t ok = guest_callback(&g_cpu, g_mods[m].entry, 3, args);
            pal_log("guest: %s DllMain -> %lu", g_lifted_modules[m].file, (unsigned long)ok);
        }
        PUSH(&g_cpu, GUEST_RET_SENTINEL);
        pal_log("guest: enter %08lx", (unsigned long)g_exe.entry);
        cpu_call(&g_cpu, g_exe.entry);
        r = GUEST_STOP_EXIT;
        snprintf(g_stop_text, sizeof g_stop_text, "entry returned eax=%08lx", (unsigned long)g_cpu.r[EAX]);
    }
    gthread_shutdown();
    g_running = 0;
    pal_log("guest: heap peak %lu KB of %lu KB, VirtualAlloc free %lu KB", (unsigned long)(gheap_peak_bytes() >> 10),
            (unsigned long)((gheap_peak_bytes() + gheap_free_bytes()) >> 10), (unsigned long)(gvirt_free_bytes() >> 10));
    pal_log("guest: stopped (%d) %s; gmem faults %lu", r, g_stop_text, (unsigned long)g_gmem_faults);
    return r;
}

void guest_fini(void)
{
    for (uint32_t m = 0; m < g_nmods; m++) pe_free(&g_mods[m]);
    g_nmods = 0;
    free(g_dispatch);
    g_dispatch = NULL;
    gmem_fini();
}

void guest_set_exit_code(uint32_t code) { g_exit_code = code; }

uint32_t guest_callback(cpu_t *c, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    for (int i = nargs - 1; i >= 0; i--) PUSH(c, args[i]);
    PUSH(c, GUEST_RET_SENTINEL);
    cpu_call(c, fn);
    uint32_t ret = c->r[EAX];
    /* callee-saved registers survive (stdcall/cdecl); esp: callee popped args (stdcall)
     * or not (cdecl) - restoring it covers both. */
    uint32_t eax = ret;
    memcpy(c->r, saved, sizeof saved);
    c->r[EAX] = eax;
    return ret;
}

uint32_t guest_invoke(uint32_t fn, int nargs, const uint32_t *args, int *reason)
{
    g_deadline = g_cfg.max_ms ? pal_time_ms() + g_cfg.max_ms : 0;
    if (!g_cpu.r[ESP]) g_cpu.r[ESP] = GUEST_STACK_HI - 64;
    g_running = 1;
    int r = setjmp(g_stop_jmp);
    uint32_t ret = 0;
    if (r == 0) ret = guest_callback(&g_cpu, fn, nargs, args);
    g_running = 0;
    if (reason) *reason = r;
    return ret;
}
