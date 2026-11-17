/* PE32 loader into the guest arena (platform-free core).
 * Maps headers + sections at the preferred base or at a given base (applying
 * base relocations), and decodes the import and export tables. Sections are
 * read straight into the arena: no large host buffers. */
#ifndef PE_LOAD_H
#define PE_LOAD_H

#include <stdint.h>
#include "core/gmem.h"

typedef struct {
    char     dll[32];        /* as written in the import table */
    char     name[64];       /* empty if imported by ordinal */
    uint16_t ordinal;        /* ordinal, or hint if imported by name */
    int      by_ordinal;
    gptr     iat;            /* guest address of the IAT slot to patch */
} pe_import;

typedef struct {
    char     name[64];       /* empty if exported by ordinal only */
    uint16_t ordinal;
    gptr     addr;           /* guest address (after relocation) */
} pe_export;

typedef struct pe_image {
    gptr       base, pref_base, entry;
    uint32_t   size;         /* SizeOfImage */
    uint32_t   nrelocs;      /* HIGHLOW fixups applied (0 if loaded at pref_base) */
    pe_import *imports;  uint32_t nimports;
    pe_export *exports;  uint32_t nexports;
    char       err[96];
} pe_image;

/* load_base == 0: use the preferred base. Returns 1 on success. */
int  pe_load(const char *path, gptr load_base, pe_image *img);
void pe_free(pe_image *img);
/* Export lookup by (case-sensitive) name; 0 if absent. */
gptr pe_find_export(const pe_image *img, const char *name);

#endif
