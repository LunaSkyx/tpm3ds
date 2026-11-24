/* Guest process: memory layout, image loading, entry, stop/exit.
 *
 * Arena layout (guest address == arena offset):
 *   0x00000000-0x0000ffff  null guard (always faults)
 *   0x00010000-0x0001ffff  TIB/TEB, HLE scratch (command line, env, ...)
 *   0x00020000-0x000fffff  HLE static data (strings, handle tables)
 *   0x00100000-0x002fffff  main thread stack (2 MB, grows down)
 *   0x00400000-0x008cffff  WMAIN.EXE image (its preferred base)
 *   0x008e0000-0x00911fff  BINKW32.DLL image (rebased, lifted)
 *   0x00920000-0x00971fff  IMUSE.DLL image (rebased, lifted)
 *   (bases come from tools/lifter/lift_all.py via g_lifted_modules)
 *   0x00a00000-HEAP_END    guest heap (HeapAlloc & co.)
 *   HEAP_END  -arena end   VirtualAlloc regions, guest thread stacks (HEAP_END = halfway)                        */
#ifndef GUEST_H
#define GUEST_H

#include <stdint.h>
#include "core/cpu.h"

#define GUEST_TIB        0x00010000u
#define GUEST_SCRATCH    0x00011000u
#define GUEST_HLEDATA    0x00020000u
#define GUEST_STACK_LO   0x00100000u
#define GUEST_STACK_HI   0x00300000u
#define GUEST_EXE_BASE   0x00400000u
#define GUEST_HEAP_LO    0x00a00000u
#define GUEST_RET_SENTINEL 0xffeffff0u   /* return address meaning "back to host" */

typedef enum {
    GUEST_RUNNING = 0, GUEST_STOP_EXIT, GUEST_STOP_MISSING_HLE, GUEST_STOP_TRAP,
    GUEST_STOP_WATCHDOG, GUEST_STOP_FAULTS, GUEST_STOP_REQUESTED
} guest_stop_reason;

/* A statically recompiled module (generated list: source/gen/lifted_modules.c). */
typedef struct {
    const char *file;            /* in GAMEDATA/BIN */
    const char *import_name;     /* as other modules import it */
    uint32_t    base;            /* load base, 0 = preferred */
    const guest_fn_entry *tab;   /* sorted dispatch table */
    const uint32_t *count;
} guest_module;
extern const guest_module g_lifted_modules[];
extern const uint32_t g_lifted_module_count;

/* Loaded lifted module by import name (case-insensitive) or by base; NULL if none. */
struct pe_image;
const struct pe_image *guest_module_by_name(const char *name);
const struct pe_image *guest_module_by_base(uint32_t base);

typedef struct {
    const char *data_dir;       /* host directory holding the game's files (gobs/, level/, ...) */
    const char *save_dir;       /* writable overlay for files the game creates or changes */
    const char *install_dir;    /* what the PC installer would have produced: big.lab unpacked
                                   from BIG.Z (3DS: data/gobs, filled on first boot) */
    uint32_t    arena_size;
    uint32_t    max_ms;         /* wall-clock limit, 0 = none */
    int         trace;          /* 0 quiet, 1 log every import call */
    int         hz60;           /* the game's own 60 fps mode (core/game_options.c) */
    int         nolimit;        /* frame limiter off (benchmarks and profiling) */
} guest_config;

int  guest_init(const guest_config *cfg);   /* arena, images, IAT, TIB, stack */
int  guest_run(void);                        /* runs the exe entry; returns the stop reason */
void guest_fini(void);

cpu_t      *guest_cpu(void);
uint32_t    guest_exit_code(void);
const char *guest_stop_text(void);
int         guest_trace_level(void);

/* Stop the guest from anywhere inside lifted code or HLE (longjmp to guest_run). */
void guest_stop(cpu_t *c, guest_stop_reason r, const char *why) __attribute__((noreturn));
void guest_watchdog(cpu_t *c);               /* checks wall clock + fault count */
void guest_request_stop(void);               /* thread-safe flag, honoured at the next HLE call */

/* Guest path ("gobs\\big.lab", ".\\obi.ini", "C:\\...\\GAMEDATA\\x") -> host path. */
void guest_path(char *out, uint32_t n, const char *guest_path);
void guest_path_in(char *out, uint32_t n, const char *base, const char *guest_path);
const char *guest_data_dir(void);
const char *guest_save_dir(void);
const char *guest_install_dir(void);
void guest_backtrace(const char *why);
void guest_stall_monitor_start(void);             /* background thread: logs the guest state when nothing advances for 3 s */            /* logs likely return addresses on the guest stack */
void guest_set_exit_code(uint32_t code);

/* Call a guest function from HLE (callbacks: EnumDevices, WndProc, ...): pushes args
 * right-to-left, a sentinel return address, runs it, returns eax. */
uint32_t guest_callback(cpu_t *c, uint32_t fn, int nargs, const uint32_t *args);

/* Run one guest function from the host (tests, tools): like guest_run, but for fn.
 * *reason gets GUEST_RUNNING if it returned normally. */
uint32_t guest_invoke(uint32_t fn, int nargs, const uint32_t *args, int *reason);

#endif
