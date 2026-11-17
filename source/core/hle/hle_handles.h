/* Kernel object handles shared by the HLE modules (files, finds, modules, events). */
#ifndef HLE_HANDLES_H
#define HLE_HANDLES_H

#include <stdint.h>
#include "pal/pal.h"

enum { HT_FREE = 0, HT_FILE, HT_FIND, HT_STD, HT_HEAP, HT_MODULE, HT_MISC, HT_THREAD };

typedef struct {
    int      type;
    pal_fh  *fh;
    pal_dir *dir;
    int      index;          /* thread: gthread index; file: 1 read-only (path known), 2 voice capture, 3 voice RAM copy */
    void    *aux;            /* file index 2/3: the voice store entry */
    uint8_t  ra_virtual;     /* file index 1: the stdio position is behind mpos (read-ahead served reads) */
    uint32_t mpos;           /* file index 3: read position */
    char     text[128];      /* find: directory + pattern; module: dll name; std: which */
} hle_handle;

uint32_t    hh_alloc(int type);                 /* 0 on exhaustion */
hle_handle *hh_get(uint32_t h, int type);       /* NULL if not that type */
void        hh_free(uint32_t h);

/* Last-error value (GetLastError/SetLastError). */
extern uint32_t g_hle_last_error;

/* Host paths for guest file names. Reads look in the save overlay first, then in the
 * player's data dir. Writes always go to the overlay: original files are never modified. */
void hle_path_read(char *out, uint32_t n, const char *guest_name);
void hle_path_write(char *out, uint32_t n, const char *guest_name);   /* creates parent dirs */

#endif
