/* Header probe for the original game's archives (platform-free core).
 *   LABN : LucasArts LAB archive (gobs/big.lab, localize.lab, voice.lab)
 *   IS3Z : InstallShield 3 ".Z" container (gobs/BIG.Z on the CD -> big.lab,
 *          payload compressed with PKWARE DCL implode)
 * All fields are decoded with explicit little-endian reads (no struct casts). */
#ifndef ARCPROBE_H
#define ARCPROBE_H

#include <stddef.h>
#include <stdint.h>

typedef enum { ARC_MISSING, ARC_UNKNOWN, ARC_LAB, ARC_IS3Z } arc_kind;

#define ARC_MAX_TYPES 8

typedef struct {
    arc_kind kind;
    int      ok;              /* header and directory validated */
    char     err[96];
    uint64_t file_size;

    /* LAB */
    uint32_t lab_version, lab_entries, lab_names_size;
    uint64_t lab_max_end;     /* max(offset+size) over all entries */
    char     lab_first_name[64];
    uint32_t lab_first_off, lab_first_size;
    int      lab_ntypes;
    char     lab_type[ARC_MAX_TYPES][5];
    uint32_t lab_type_count[ARC_MAX_TYPES];

    /* IS3Z (single-file container as shipped on the CD) */
    uint32_t z_files, z_arc_size, z_orig_total, z_dir_off, z_ftab_off;
    uint32_t z_orig, z_comp, z_data_off;
    uint8_t  z_dcl_lit, z_dcl_dict;
    char     z_name[64];
} arc_info;

/* Returns 1 if the file was recognised and validated. Never aborts. */
int  arc_probe(const char *path, arc_info *out);
/* One-line human summary (for log and debug console). */
void arc_describe(const arc_info *a, char *buf, size_t n);

#endif
