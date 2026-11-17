/* Native 0x00471c5c: "extract resource <name> of a LAB into save/cache/<name><ext>" (cdecl, args: lab, prefix, name, ext; 0 = success).
 * The game's level load deletes every file of save/cache and then extracts the voice line files of the level again (529 in
 * the Espa level): find the entry in the LAB, read it, decode it, create and write the file, ~250 ms each on the card, and a
 * play session opens only a sixth of them. The function also has a side effect the game relies on later: when the resource is
 * not found in the mounted LABs it mounts the voice LAB of that prefix (FUN_00472112 -> FUN_004719e0), which is where the
 * dialogue code finds its lines afterwards ("res_allocLocalFile"). So this override
 *   - always does that lookup / mount part and the release after it (the resource is loaded and freed again, as in the original),
 *   - then only notes the arguments (hle_voice_lazy_defer): the first read open of the file runs the original function for
 *     that one line into the RAM voice store (hle_kernel32.c lz_get; hle_voice_lazy_busy() is set then and the lifted body
 *     runs), or skips the work when the very same file is still on the card (hle_voice_cache_have), or runs the lifted body. */
#include "core/cpu.h"
#include "core/gmem.h"
#include "core/gheap.h"
#include "core/guest.h"
#include "pal/pal.h"

void wmain_00471c5c_ref(cpu_t *c);
int hle_voice_cache_have(const char *gn);
int hle_voice_lazy_busy(void);
int hle_voice_lazy_defer(const char *gn, uint32_t lab, const char *prefix, const char *name, const char *ext);

static int gstr(char *out, int n, gptr a)
{
    int k = 0;
    if (a) for (; k < n - 1; k++) { uint8_t ch = gmem_rd8(a + (uint32_t)k); if (!ch) break; out[k] = (char)ch; }
    out[k] = 0;
    return k;
}

#define SC_PATH 0u      /* FUN_00471f61 result, 128 bytes */
#define SC_LABN 512u    /* LAB file name (FUN_00471fcb), 128 bytes */

void wmain_00471c5c(cpu_t *cg)
{
    static gptr scratch;
    if (hle_voice_lazy_busy()) { wmain_00471c5c_ref(cg); return; }
    if (!scratch) scratch = gheap_alloc(1024);
    if (scratch) {
        uint32_t sp = cg->r[ESP];                                                    /* [sp] return, +4 lab, +8 prefix, +12 name, +16 ext */
        uint32_t lab = gmem_rd32(sp + 4u), pfx = gmem_rd32(sp + 8u), nm = gmem_rd32(sp + 12u), ex = gmem_rd32(sp + 16u);
        if (gmem_rd32(0x00881348u) == 0) { cg->r[EAX] = 1u; cg->r[ESP] += 4u; return; }      /* the original: LAB system not up -> failure */
        /* the lookup / mount part of the original, then its release, without decoding or writing the file */
        uint32_t a1[2] = { lab, nm };
        uint32_t res = guest_callback(cg, 0x00472040u, 2, a1);                       /* find (and load) the resource in the mounted LABs */
        if (res == 0) {                                                              /* not mounted yet: mount this prefix's LAB */
            uint32_t a3[3] = { scratch + SC_LABN, pfx, nm };
            guest_callback(cg, 0x00471fcbu, 3, a3);
            uint32_t a4[2] = { lab, scratch + SC_LABN };
            res = guest_callback(cg, 0x00472112u, 2, a4);
            if (res == 0) { cg->r[EAX] = 1u; cg->r[ESP] += 4u; return; }              /* still not found: the original fails here, too */
        }
        uint32_t a5[2] = { res, 1u }, a6[1] = { res };
        guest_callback(cg, 0x004722f4u, 2, a5);                                      /* what the original does after the extraction: */
        guest_callback(cg, 0x0047221fu, 1, a6);                                      /* mark it purgeable and drop the reference */
        uint32_t a7[3] = { scratch + SC_PATH, nm, ex };
        guest_callback(cg, 0x00471f61u, 3, a7);                                      /* the game's own path builder: ".\cache\<name><ext>" */
        char path[300], prefix[64], name[64], ext[24];
        int k = gstr(path, (int)sizeof path, scratch + SC_PATH);
        gstr(prefix, (int)sizeof prefix, pfx);
        gstr(name, (int)sizeof name, nm);
        gstr(ext, (int)sizeof ext, ex);
        if (k && k < 200) {
            if (hle_voice_lazy_defer(path, lab, prefix, name, ext) || hle_voice_cache_have(path)) {
                cg->r[EAX] = 0u; cg->r[ESP] += 4u; return;
            }
        }
    }
    wmain_00471c5c_ref(cg);
}
