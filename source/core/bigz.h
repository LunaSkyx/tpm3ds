/* One-time unpack of the CD's InstallShield 3 container (gobs/BIG.Z) into the
 * archive the game opens (gobs/big.lab). The original BIG.Z is only read.
 * Output goes to "<dst>.tmp" and is renamed to <dst> only after it is
 * validated (exact size + LAB directory check), so an interrupted run never
 * leaves a corrupt big.lab. */
#ifndef BIGZ_H
#define BIGZ_H

#include <stdint.h>

typedef struct {
    volatile uint32_t done_bytes;    /* written so far (for a progress bar) */
    volatile uint32_t total_bytes;   /* expected size, set once the header is read */
    int (*should_abort)(void);       /* polled for every 4 KB; may be NULL */
} bigz_progress;

/* Returns 0 on success, otherwise a negative code; 'err' gets a message. */
/* Not reentrant (uses one static 32 KB context); run one unpack at a time. */
int bigz_unpack(const char *src_z, const char *dst_lab, bigz_progress *prog,
                char *err, unsigned err_size);

#endif
