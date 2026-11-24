/* Guest threads (CreateThread, multimedia timers).
 *
 * Each guest thread runs lifted code on its own host thread (lifted code
 * recurses on the host stack) with its own cpu_t, guest stack and TIB. Only
 * the holder of a single run token executes: the token moves only at the
 * blocking points the game itself has (Sleep, waits, PeekMessage/GetMessage,
 * Flip, contended critical sections), in a fixed round-robin order. So guest
 * code never runs concurrently and needs no locking, and thread interleaving
 * is as deterministic as the wall clock that wakes sleepers.
 *
 * Thread 0 is the main thread (guest_run's caller). */
#ifndef GTHREAD_H
#define GTHREAD_H

#include <stdint.h>
#include "core/cpu.h"

#define GT_MAX 8

void     gthread_init(cpu_t *main_cpu);
/* Ends every other thread (they leave at their next blocking point) and joins them. */
void     gthread_shutdown(void);

/* Guest thread at fn(arg) (stdcall, one argument). Returns the thread index, -1 on failure. */
int      gthread_create(uint32_t fn, uint32_t arg, uint32_t stack_size);
/* Host-side thread body that may call into the guest with guest_callback(). */
int      gthread_create_host(void (*fn)(cpu_t *c, void *arg), void *arg);

int      gthread_current(void);
void     gthread_dump(void);
cpu_t   *gthread_cpu(void);                       /* cpu of the running thread */
uint32_t gthread_id(int t);                       /* Win32 thread id */
int      gthread_alive(int t);
uint32_t gthread_exit_code(int t);

/* Blocking points. All may switch threads; they return on the calling thread. */
void     gthread_yield(void);                     /* let ready threads run (Sleep(0)) */
void     gthread_sleep(uint32_t ms);
void     gthread_preempt(void);                   /* yield if another thread is due (every HLE call) */
int      gthread_shutting_down(void);
extern volatile int g_gthread_idle;              /* every thread waits for a wake-up (profiler) */
/* Wait until gthread_wake(key) (contended critical sections, events). */
void     gthread_block_on(uint32_t key);
void     gthread_wake(uint32_t key);

/* Per-thread TLS (TlsAlloc/TlsGetValue). */
#define GT_TLS 64
uint32_t *gthread_tls(void);

/* A non-main thread stopping the guest: record why, end this thread, and make the
 * main thread stop at its next blocking point. */
void     gthread_stop_from_thread(int reason, const char *why) __attribute__((noreturn));
int      gthread_pending_stop(char *why, uint32_t n);   /* main thread: reason or 0 */

#endif
