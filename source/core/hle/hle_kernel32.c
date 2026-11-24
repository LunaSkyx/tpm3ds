/* KERNEL32 HLE: process/CRT startup, heap, virtual memory, files, find, time,
 * locale (single-byte code page 1252), console stubs, profile (.ini) files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "core/video/video.h"
#include "core/gheap.h"
#include "core/guest.h"
#include "core/gthread.h"
#include "core/hle/hle.h"
#include "core/hle/hle_handles.h"
#include "pal/pal.h"
#include "core/audio/mixer.h"

uint32_t g_hle_last_error;

#define ERROR_FILE_NOT_FOUND      2
#define ERROR_PATH_NOT_FOUND      3
#define ERROR_ACCESS_DENIED       5
#define ERROR_INVALID_HANDLE      6
#define ERROR_NOT_ENOUGH_MEMORY   8
#define ERROR_NO_MORE_FILES       18
#define ERROR_FILE_EXISTS         80
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define INVALID_HANDLE            0xffffffffu

static uint8_t up(uint8_t ch) { return ch >= 'a' && ch <= 'z' ? (uint8_t)(ch - 32) : ch; }
static uint8_t lo(uint8_t ch) { return ch >= 'A' && ch <= 'Z' ? (uint8_t)(ch + 32) : ch; }

/* ------------------------------------------------------------ handles */
#define MAX_HANDLES 256
static hle_handle g_h[MAX_HANDLES];
static char g_rpath[MAX_HANDLES][600];   /* path of a read-only file handle (see park_put) */
static char g_wpath[MAX_HANDLES][300];   /* final name of a voice file that is being written as <name>.part */

uint32_t hh_alloc(int type)
{
    for (int i = 1; i < MAX_HANDLES; i++)
        if (g_h[i].type == HT_FREE) {
            memset(&g_h[i], 0, sizeof g_h[i]);
            g_h[i].type = type;
            return 0x100u + 4u * (uint32_t)i;
        }
    pal_log("hle: handle table full");
    return 0;
}

hle_handle *hh_get(uint32_t h, int type)
{
    if (h < 0x100u || (h & 3u)) return NULL;
    uint32_t i = (h - 0x100u) / 4u;
    if (i >= MAX_HANDLES || g_h[i].type != type) return NULL;
    return &g_h[i];
}

void hh_free(uint32_t h)
{
    if (h < 0x100u || (h & 3u) || (h - 0x100u) / 4u >= MAX_HANDLES) return;
    g_h[(h - 0x100u) / 4u].type = HT_FREE;
}

/* ------------------------------------------------------------ paths */
/* Existence checks come from cached directory listings: probing each search
 * directory with an open costs ~20 ms per call on the 3DS SD card, and the game
 * looks for loose voice files before falling back to VOICE.LAB (5 probes each).
 * The player's files never change while running and the save overlay is only
 * written through this HLE, which drops the cache (path_cache_drop). */
#define PC_DIRS 48
typedef struct { char *dir; char *names; uint32_t len; int exists; } path_dir;   /* names: "\0"-separated, lower case, dirs end in '/' */
static const path_dir *path_cache_dir(const char *dir);
static path_dir g_pc[PC_DIRS];
static int      g_npc;

static void park_drop(void);
static void path_cache_drop(void)
{
    park_drop();
    for (int i = 0; i < g_npc; i++) { free(g_pc[i].dir); free(g_pc[i].names); }
    g_npc = 0;
}

/* RAM copy of the decoded voice lines. The game decodes VOICE.LAB entries into <save>/cache/*.wav and later
 * plays each by opening and reading the whole file (3 opens + a 100-300 KB read: ~65 ms hitch in Azahar, 15-25 ms
 * on hardware). Everything it writes there is kept in memory (up to VS_BUDGET); a later read-only open of such a
 * file is served from memory without touching the SD card. The file on the card is still written normally. */
extern int g_skip_level;
extern int g_battle_mode;
static int g_movies_open;
int g_menu_reached;   /* the menu's own .bik clips (ss0-3) were opened: the .bil intro movies are over (menu.txt test mode) */
/* Bink reads small movies (bigape) into memory and closes the file ~100 ms after opening it, so the handle count
 * alone says "no movie" for the whole playback: stay "open" for 3 s after an open/close and 0.8 s after each movie
 * frame presented (hle_movie_present, called from video.c). */
static uint32_t g_movie_until;
void hle_movie_touch(uint32_t ms) { uint32_t t = pal_time_ms() + ms; if (!g_movie_until || (int32_t)(t - g_movie_until) > 0) g_movie_until = t; }
int hle_movie_open(void) { return g_movies_open + (g_movie_until && (int32_t)(g_movie_until - pal_time_ms()) > 0); }   /* open .bik handles: movies keep the real clock (Bink paces itself by it) */
#define VS_N 128
#define VS_BUDGET (12u << 20)
typedef struct { char path[300]; uint8_t *data; uint32_t size, cap, stamp; int used, complete; } vs_entry;
static vs_entry g_vs[VS_N];
static uint32_t g_vs_bytes, g_vs_clock;

static int path_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (lo((uint8_t)*a) != lo((uint8_t)*b)) return 0;
    return *a == *b;
}

static int is_voice_wav(const char *p)
{
    static int off = -1;
    if (off < 0) off = getenv("TPM_NO_VS") != NULL;
    if (off) return 0;
    size_t n = strlen(p);
    if (n < 10 || n >= sizeof g_vs[0].path || p[n - 4] != '.' || lo((uint8_t)p[n - 3]) != 'w' ||
        lo((uint8_t)p[n - 2]) != 'a' || lo((uint8_t)p[n - 1]) != 'v') return 0;
    for (const char *q = p; *q; q++)
        if (q[0] == '/' && lo((uint8_t)q[1]) == 'c' && lo((uint8_t)q[2]) == 'a' && lo((uint8_t)q[3]) == 'c' &&
            lo((uint8_t)q[4]) == 'h' && lo((uint8_t)q[5]) == 'e' && q[6] == '/') return 1;
    return 0;
}

static void vs_free(vs_entry *e)
{
    if (e->used) { g_vs_bytes -= e->cap; free(e->data); }
    memset(e, 0, sizeof *e);
}

static void vs_drop(const char *path)
{
    for (int i = 0; i < VS_N; i++) if (g_vs[i].used && path_ieq(g_vs[i].path, path)) vs_free(&g_vs[i]);
}

static vs_entry *vs_find(const char *path)
{
    for (int i = 0; i < VS_N; i++)
        if (g_vs[i].used && g_vs[i].complete && path_ieq(g_vs[i].path, path)) { g_vs[i].stamp = ++g_vs_clock; return &g_vs[i]; }
    return NULL;
}

/* A new (growing) entry for a file about to be written; NULL if there is no room. */
static vs_entry *vs_begin(const char *path)
{
    vs_drop(path);
    for (int i = 0; i < VS_N; i++)
        if (!g_vs[i].used) {
            memset(&g_vs[i], 0, sizeof g_vs[i]);
            strcpy(g_vs[i].path, path);
            g_vs[i].used = 1;
            g_vs[i].stamp = ++g_vs_clock;
            return &g_vs[i];
        }
    /* table full: evict the least recently used complete entry */
    int v = -1;
    for (int i = 0; i < VS_N; i++)
        if (g_vs[i].complete && (v < 0 || g_vs[i].stamp < g_vs[v].stamp)) v = i;
    if (v < 0) return NULL;
    vs_free(&g_vs[v]);
    return vs_begin(path);
}

static int vs_put(vs_entry *e, uint32_t pos, const uint8_t *src, uint32_t n)
{
    if ((uint64_t)pos + n > (16u << 20)) return 0;
    uint32_t end = pos + n;
    if (end > e->cap) {
        uint32_t cap = e->cap ? e->cap : 65536u;
        while (cap < end) cap *= 2;
        while (g_vs_bytes - e->cap + cap > VS_BUDGET) {      /* make room: evict the oldest complete entry */
            int v = -1;
            for (int i = 0; i < VS_N; i++)
                if (g_vs[i].complete && (v < 0 || g_vs[i].stamp < g_vs[v].stamp)) v = i;
            if (v < 0) return 0;
            vs_free(&g_vs[v]);
        }
        uint8_t *nd = realloc(e->data, cap);
        if (!nd) return 0;
        if (cap > e->cap) memset(nd + e->cap, 0, cap - e->cap);
        g_vs_bytes += cap - e->cap;
        e->data = nd;
        e->cap = cap;
    }
    memcpy(e->data + pos, src, n);
    if (end > e->size) e->size = end;
    return 1;
}

/* ---- the decoded-voice cache files the game deletes and rewrites at EVERY level load ----
 * A level load deletes all of the .wav files of save/cache (529 files in the Espa level: ~64 ms each on the SD card = 34 s) and then
 * decodes every voice line of the level into a new file again (~260 ms each = 138 s): three minutes of SD card work for
 * files that come out byte for byte the same (the name encodes the voice line). So a delete only marks the file
 * ("tombstone": the game and its directory scans no longer see it, the bytes stay on the card), and the rewrite of a marked
 * file goes into a RAM buffer (sink). When the game closes it and the length equals the length of the file on the card, the
 * card is left alone; otherwise (damaged / cut-off file from a crash) the buffer is written out for real. */
#define CF_N 4096
/* state 0: on the card, 1: tombstone (deleted by the game, still on the card), 2: gone, 3: the reaper is deleting it, 4: a sink is rewriting it */
typedef struct { uint64_t h; uint32_t size; volatile uint8_t state; char path[300]; } cf_ent;
static cf_ent g_cf[CF_N];
static int g_ncf;
static char g_cf_dirs[4][300];
static int g_ncf_dirs;

static uint64_t cf_hash(const char *p)
{
    uint64_t h = 1469598103934665603ull;
    for (; *p; p++) { uint8_t ch = lo((uint8_t)*p); if (ch == '\\') ch = '/'; h = (h ^ ch) * 1099511628211ull; }
    return h;
}
static cf_ent *cf_find(const char *p)
{
    uint64_t h = cf_hash(p);
    for (int i = 0; i < g_ncf; i++) if (g_cf[i].h == h) return &g_cf[i];
    return NULL;
}
static cf_ent *cf_set(const char *p, uint32_t size, int state)
{
    cf_ent *e = cf_find(p);
    if (!e) { if (g_ncf == CF_N) return NULL; e = &g_cf[g_ncf]; e->h = cf_hash(p); }
    e->size = size; e->state = (uint8_t)state;
    if (strlen(p) < sizeof e->path) strcpy(e->path, p); else e->path[0] = 0;
    if (e == &g_cf[g_ncf]) { __sync_synchronize(); g_ncf++; }     /* published last: the reaper thread reads g_cf[0 .. g_ncf) */
    return e;
}
static void cf_wait(cf_ent *e) { for (int i = 0; e->state == 3 && i < 2000; i++) pal_sleep_ms(1); }
/* Orphans (voice files of other levels that were deleted by the game but never asked for again) are deleted for real by
 * a background thread, one file every 1.5 s, once the level load is over, so the card does not fill up with every level's
 * voices. It only takes entries in state 1 (compare-and-swap), the game thread takes them with the same swap. */
static pal_thread *g_cf_reaper;
static volatile uint32_t g_cf_last;                                /* time of the last tombstone */
static void cf_reaper(void *arg)
{
    (void)arg;
    for (;;) {
        pal_sleep_ms(1500);
        if (pal_time_ms() - g_cf_last < 40000u) continue;           /* the level load is still going on */
        int n = g_ncf;
        for (int i = 0; i < n; i++) {
            cf_ent *e = &g_cf[i];
            if (e->state != 1 || !e->path[0]) continue;
            if (!__sync_bool_compare_and_swap(&e->state, 1, 3)) continue;
            pal_file_remove(e->path);
            e->state = 2;
            pal_sleep_ms(1500);
            if (pal_time_ms() - g_cf_last < 40000u) break;
        }
    }
}
static int cf_dead(const char *dir, const char *name)
{
    char k[600];
    if (!g_ncf) return 0;
    snprintf(k, sizeof k, "%s/%s", dir, name);
    cf_ent *e = cf_find(k);
    return e && (e->state == 1 || e->state == 3);
}
/* First delete in a directory: learn the length of every voice file on the card (one batched directory read). */
static int cf_scan(const char *file_path)   /* 1: the directory has been read, so a voice file with no cf_ entry is not on the card */
{
    char dir[600], name[260];
    strncpy(dir, file_path, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char *sl = strrchr(dir, '/');
    if (!sl) return 0;
    *sl = 0;
    for (int i = 0; i < g_ncf_dirs; i++) if (!strcmp(g_cf_dirs[i], dir)) return g_ncf < CF_N;
    if (g_ncf_dirs == 4 || strlen(dir) >= sizeof g_cf_dirs[0]) return 0;
    strcpy(g_cf_dirs[g_ncf_dirs++], dir);
    pal_dir *d = pal_dir_open(dir);
    if (!d) return 1;
    int is_dir; uint32_t size;
    char junk[16][300]; int nj = 0;
    while (pal_dir_next(d, name, sizeof name, &is_dir, &size)) {
        char full[600];
        if (is_dir) continue;
        snprintf(full, sizeof full, "%s/%s", dir, name);
        if (is_voice_wav(full) && !cf_find(full)) cf_set(full, size, 0);
        else { size_t nl = strlen(full); if (nl > 9 && nl < sizeof junk[0] && !strcasecmp(full + nl - 9, ".wav.part") && nj < 16) strcpy(junk[nj++], full); }
    }
    pal_dir_close(d);
    for (int i = 0; i < nj; i++) pal_file_remove(junk[i]);               /* a write that was cut off (power / HOME during the load) */
    pal_log("io: voice cache %s: %d files known", dir, g_ncf);
    return g_ncf < CF_N;
}

typedef struct { uint8_t *buf; uint32_t size, cap, pos; char path[600]; int ram_only; } sink_t;
static int host_exists(const char *p);
static void path_cache_note(const char *p, int add);
static sink_t *sink_new(const char *path)
{
    sink_t *s = calloc(1, sizeof *s);
    if (s) { strncpy(s->path, path, sizeof s->path - 1); }
    return s;
}
static int sink_write(sink_t *s, const uint8_t *src, uint32_t n)
{
    uint64_t end = (uint64_t)s->pos + n;
    if (end > (16u << 20)) return 0;
    if (end > s->cap) {
        uint32_t cap = s->cap ? s->cap : 65536u;
        while (cap < end) cap *= 2;
        uint8_t *nb = realloc(s->buf, cap);
        if (!nb) return 0;
        memset(nb + s->cap, 0, cap - s->cap);
        s->buf = nb; s->cap = cap;
    }
    memcpy(s->buf + s->pos, src, n);
    s->pos = (uint32_t)end;
    if (s->pos > s->size) s->size = s->pos;
    return 1;
}
static uint32_t sink_read(sink_t *s, uint8_t *dst, uint32_t n)
{
    uint32_t k = s->pos < s->size ? s->size - s->pos : 0;
    if (k > n) k = n;
    if (k) memcpy(dst, s->buf + s->pos, k);
    s->pos += k;
    return k;
}
/* Hand the finished buffer to the RAM store (what a later read open of the file is served from). 1 on success. */
static int sink_publish(sink_t *s)
{
    int ok = 0;
    if (!s->size) return 0;
    vs_entry *v = vs_begin(s->path);
    if (v) {
        if (vs_put(v, 0, s->buf, s->size)) {
            if (v->size < v->cap) { uint8_t *nd = realloc(v->data, v->size); if (nd) { g_vs_bytes -= v->cap - v->size; v->data = nd; v->cap = v->size; } }
            v->complete = 1;
            ok = 1;
        } else vs_free(v);
    }
    return ok;
}
static void sink_close(sink_t *s, const char *key)
{
    if (s->ram_only) {                               /* lazily decoded voice line: lives in RAM only (see lz_get) */
        if (!sink_publish(s)) {                      /* no room in the store: write the file after all, so the line never goes missing */
            pal_fh *o = pal_fh_open(s->path, PAL_FH_RW_TRUNCATE);
            if (o) { if (s->size) pal_fh_write(o, s->buf, s->size); pal_fh_close(o); path_cache_note(s->path, 1); }
            if (key[0]) cf_set(key, s->size, 0);
            pal_log("io: voice line %s did not fit in the RAM store, written to the card", s->path);
        }
        free(s->buf);
        free(s);
        return;
    }
    cf_ent *e = cf_find(key);
    int same = e && e->state == 4 && e->size == s->size && s->size > 0;
    if (!same) {
        pal_fh *o = pal_fh_open(s->path, PAL_FH_RW_TRUNCATE);
        if (o) { if (s->size) pal_fh_write(o, s->buf, s->size); pal_fh_close(o); }
        pal_log("io: voice cache file rewritten: %s (%lu bytes, card had %ld)", s->path, (unsigned long)s->size, e ? (long)e->size : -1L);
    }
    cf_set(key, s->size, 0);
    sink_publish(s);                                 /* the played copy comes from the RAM store like for any decoded line */
    free(s->buf);
    free(s);
}

/* ---- lazily decoded voice lines ----
 * At a level start the game extracts EVERY voice line of the level (529 in Espa) into save/cache, but a play session opens
 * only a sixth of them (3343 files written, 570 read opens in one 55-minute log). Creating a file costs 175 ms on the card,
 * decoding it 80 ms. So the extraction call (FUN_00471c5c, native_471c5c.c) only notes its arguments here, and the first
 * time the game opens such a file for reading the original function is run for that one line, writing into a RAM sink
 * (no file on the card at all) that is published to the voice store. A line the store has evicted is decoded again. */
#define LZ_N 4096
typedef struct { uint64_t h; uint32_t lab, gen; char prefix[40], name[40], ext[12], gn[72]; } lz_ent;
static lz_ent g_lz[LZ_N];
static int g_nlz, g_lz_busy;
static uint32_t g_lz_gen;
static gptr g_lz_scratch;
static int lz_on(void)                               /* sdmc:/3ds/tpm/nolazy.txt (or env TPM_NO_LAZY) switches the lazy decode off, to compare */
{
    static int off = -1;
    if (off < 0) {
        pal_fh *t = pal_fh_open("sdmc:/3ds/tpm/nolazy.txt", PAL_FH_READ);
        off = getenv("TPM_NO_LAZY") != NULL || t != NULL;
        if (t) pal_fh_close(t);
        pal_log("lazy voice decode: %s", off ? "OFF" : "on");
    }
    return !off;
}
static lz_ent *lz_find(const char *key)
{
    uint64_t h = cf_hash(key);
    for (int i = 0; i < g_nlz; i++) if (g_lz[i].h == h) return &g_lz[i];
    return NULL;
}
int hle_voice_lazy_busy(void) { return g_lz_busy; }
static void lz_drop(const char *key)
{
    uint64_t h = cf_hash(key);
    for (int i = 0; i < g_nlz; i++) if (g_lz[i].h == h) { g_lz[i] = g_lz[--g_nlz]; return; }
}
int hle_voice_lazy_defer(const char *gn, uint32_t lab, const char *prefix, const char *name, const char *ext)
{
    char key[600];
    if (!lz_on() || g_lz_busy || !guest_save_dir()) return 0;
    if (strlen(prefix) >= sizeof g_lz[0].prefix || strlen(name) >= sizeof g_lz[0].name || strlen(ext) >= sizeof g_lz[0].ext) return 0;
    guest_path_in(key, sizeof key, guest_save_dir(), gn);
    if (!is_voice_wav(key)) return 0;
    lz_ent *z = lz_find(key);
    if (z && (z->lab != lab || strcmp(z->prefix, prefix) || strcmp(z->name, name) || strcmp(z->ext, ext))) vs_drop(key);   /* another level's line of the same file name: its RAM copy is stale */
    if (!z) {
        vs_drop(key);
        if (g_nlz == LZ_N) {                         /* full: drop the older half (lines of earlier levels) */
            uint32_t keep = g_lz_gen - LZ_N / 2;
            int k = 0;
            for (int i = 0; i < g_nlz; i++) if (g_lz[i].gen > keep) g_lz[k++] = g_lz[i];
            g_nlz = k;
            if (g_nlz == LZ_N) return 0;
        }
        z = &g_lz[g_nlz++];
        z->h = cf_hash(key);
    }
    z->lab = lab; z->gen = ++g_lz_gen;
    snprintf(z->gn, sizeof z->gn, "%s", gn);
    strcpy(z->prefix, prefix); strcpy(z->name, name); strcpy(z->ext, ext);
    return 1;
}
/* The RAM copy of a deferred voice line (decoding it now if it is not in the store), or NULL if the line is not deferred,
 * a real file is on the card, or the decode produced nothing. */
static vs_entry *lz_get(cpu_t *c, const char *gn)
{
    char key[600];
    if (!g_nlz || g_lz_busy || !guest_save_dir()) return NULL;
    guest_path_in(key, sizeof key, guest_save_dir(), gn);
    lz_ent *z = lz_find(key);
    if (!z) return NULL;
    vs_entry *e = vs_find(key);
    if (e) return e;
    if (host_exists(key)) return NULL;
    if (!g_lz_scratch) g_lz_scratch = gheap_alloc(256);
    if (!g_lz_scratch) return NULL;
    for (int k = 0; k < 40; k++) gmem_wr8(g_lz_scratch + (uint32_t)k, (uint8_t)z->prefix[k]);
    for (int k = 0; k < 40; k++) gmem_wr8(g_lz_scratch + 64u + (uint32_t)k, (uint8_t)z->name[k]);
    for (int k = 0; k < 12; k++) gmem_wr8(g_lz_scratch + 128u + (uint32_t)k, (uint8_t)z->ext[k]);
    uint32_t a[4] = { z->lab, g_lz_scratch, g_lz_scratch + 64u, g_lz_scratch + 128u };
    uint64_t t0 = pal_time_us();
    g_lz_busy = 1;
    guest_callback(c, 0x00471c5cu, 4, a);
    g_lz_busy = 0;
    e = vs_find(key);
    pal_log("io: voice line %s decoded on first use in %lu ms -> %s", gn, (unsigned long)((pal_time_us() - t0) / 1000), e ? "RAM" : "(no RAM copy)");
    return e;
}

/* Background fill of the voice store: a helper thread reads the existing voice WAVs of the save overlay
 * (a previous session decoded them) into private buffers; the main thread adopts finished buffers into the
 * store at its next file call, so the store itself is only ever touched by one thread. */
#define PF_MAX 128
typedef struct { uint8_t *data; uint32_t size; int idx; } pf_done;
static struct {
    int started, n, consumed;
    char (*paths)[300];
    pf_done done[PF_MAX];
    volatile int produced;
    volatile int stop;
    uint32_t budget;
    pal_thread *t;
} g_pf;

static void pf_thread(void *arg)
{
    (void)arg;
    uint32_t total = 0;
    for (int i = 0; i < g_pf.n && !g_pf.stop; i++) {
        pal_fh *f = pal_fh_open(g_pf.paths[i], PAL_FH_READ);
        if (!f) continue;
        int64_t size = pal_fh_seek(f, 0, 2);
        uint8_t *buf = NULL;
        if (size > 44 && size <= (4 << 20) && total + (uint32_t)size <= g_pf.budget && pal_fh_seek(f, 0, 0) == 0 &&
            (buf = malloc((size_t)size)) != NULL) {
            if (pal_fh_read(f, buf, (size_t)size) != size) { free(buf); buf = NULL; }
        }
        pal_fh_close(f);
        if (buf) {
            total += (uint32_t)size;
            g_pf.done[g_pf.produced].data = buf;
            g_pf.done[g_pf.produced].size = (uint32_t)size;
            g_pf.done[g_pf.produced].idx = i;
            __sync_synchronize();
            g_pf.produced = g_pf.produced + 1;
        }
    }
}

static void pf_adopt(void)
{
    __sync_synchronize();
    while (g_pf.consumed < g_pf.produced) {
        pf_done *d = &g_pf.done[g_pf.consumed++];
        if (vs_find(g_pf.paths[d->idx])) { free(d->data); continue; }   /* written this session: newer */
        vs_entry *e = vs_begin(g_pf.paths[d->idx]);
        if (!e || g_vs_bytes + d->size > VS_BUDGET) { if (e) vs_free(e); free(d->data); continue; }
        e->data = d->data; e->size = e->cap = d->size; e->complete = 1;
        g_vs_bytes += d->size;
    }
}

/* Start once, on the first voice file the game touches: its directory listing is the work list. */
static void pf_start(const char *wav_path)
{
    if (g_pf.started) return;
    g_pf.started = 1;
    char dir[600];
    const char *slash = strrchr(wav_path, '/');
    if (!slash || (size_t)(slash - wav_path) >= sizeof dir) return;
    memcpy(dir, wav_path, (size_t)(slash - wav_path));
    dir[slash - wav_path] = 0;
    const path_dir *d = path_cache_dir(dir);
    if (!d || !d->exists) return;
    g_pf.paths = malloc(sizeof *g_pf.paths * PF_MAX);
    if (!g_pf.paths) return;
    for (uint32_t o = 0; o < d->len && g_pf.n < PF_MAX; o += (uint32_t)strlen(d->names + o) + 1) {
        const char *e = d->names + o;
        size_t el = strlen(e);
        char full[300];
        if (el < 5 || e[el - 1] == '/' || el + (size_t)(slash - wav_path) + 2 >= sizeof full) continue;
        snprintf(full, sizeof full, "%s/%s", dir, e);
        if (!is_voice_wav(full) || vs_find(full)) continue;
        strcpy(g_pf.paths[g_pf.n++], full);
    }
    g_pf.budget = VS_BUDGET > g_vs_bytes + (1u << 20) ? VS_BUDGET - g_vs_bytes - (1u << 20) : 0;
    if (g_pf.n && g_pf.budget) g_pf.t = pal_thread_spawn_bg(pf_thread, NULL, 64 * 1024);
}

/* Read-only handles the guest closed are parked (open, at most PARK_N of them) and handed out again
 * on the next read-only open of the same path: the game opens each voice file three times in a row and
 * an open+close costs far more than a seek on the SD card. Any write-mode open or delete drops them. */
#define PARK_N 6
static struct { char path[600]; pal_fh *fh; } g_park[PARK_N];
static int g_park_next;

static void park_drop(void)
{
    for (int i = 0; i < PARK_N; i++) if (g_park[i].fh) { pal_fh_close(g_park[i].fh); g_park[i].fh = NULL; }
}

static pal_fh *park_take(const char *path)
{
    for (int i = 0; i < PARK_N; i++)
        if (g_park[i].fh && !strcmp(g_park[i].path, path)) {
            pal_fh *f = g_park[i].fh;
            g_park[i].fh = NULL;
            if (pal_fh_seek(f, 0, 0) < 0) { pal_fh_close(f); return NULL; }
            return f;
        }
    return NULL;
}

static void park_put(const char *path, pal_fh *f)
{
    if (strlen(path) >= sizeof g_park[0].path) { pal_fh_close(f); return; }
    int i = g_park_next;
    g_park_next = (g_park_next + 1) % PARK_N;
    if (g_park[i].fh) pal_fh_close(g_park[i].fh);
    strcpy(g_park[i].path, path);
    g_park[i].fh = f;
}

static const path_dir *path_cache_dir(const char *dir)
{
    for (int i = 0; i < g_npc; i++) if (!strcmp(g_pc[i].dir, dir)) return &g_pc[i];
    if (g_npc == PC_DIRS) path_cache_drop();
    path_dir *d = &g_pc[g_npc];
    memset(d, 0, sizeof *d);
    d->dir = malloc(strlen(dir) + 1);
    if (!d->dir) return NULL;
    strcpy(d->dir, dir);
    pal_dir *h = pal_dir_open(dir);
    if (h) {
        char name[260];
        int is_dir;
        uint32_t size, cap = 0;
        d->exists = 1;
        while (pal_dir_next(h, name, sizeof name, &is_dir, &size)) {
            size_t n = strlen(name);
            if (g_ncf && !is_dir && cf_dead(dir, name)) continue;
            if (d->len + n + 2 > cap) {
                cap = (d->len + (uint32_t)n + 2) * 2 + 256;
                char *nn = realloc(d->names, cap);
                if (!nn) break;
                d->names = nn;
            }
            for (size_t k = 0; k < n; k++) d->names[d->len + k] = name[k];   /* original spelling: see host_exists */
            d->len += (uint32_t)n;
            if (is_dir) d->names[d->len++] = '/';
            d->names[d->len++] = 0;
        }
        pal_dir_close(h);
    }
    g_npc++;
    return d;
}

static int leaf_eq(const char *e, size_t el, const char *leaf, size_t n)
{
    if (el == n + 1 && e[n] == '/') el = n;
    if (el != n) return 0;
    for (size_t k = 0; k < n; k++) if (lo((uint8_t)e[k]) != lo((uint8_t)leaf[k])) return 0;
    return 1;
}

/* Directory the cache already lists (never lists one). */
static path_dir *path_cache_find(const char *dir)
{
    for (int i = 0; i < g_npc; i++) if (!strcmp(g_pc[i].dir, dir)) return &g_pc[i];
    return NULL;
}

/* The save overlay is only changed through this HLE, so a created or deleted file is applied to
 * the cached listing of its directory instead of dropping every listing: every decoded voice
 * line is a new file in save/cache, and re-reading these directories costs ~20 ms on the SD card. */
static void path_cache_note(const char *p, int add)
{
    park_drop();
    const char *slash = strrchr(p, '/');
    char dir[600], leaf[260];
    size_t dl, n;
    path_dir *d;
    if (!slash || !slash[1]) return;
    dl = (size_t)(slash - p);
    n = strlen(slash + 1);
    if (dl >= sizeof dir || n >= sizeof leaf) { path_cache_drop(); return; }
    memcpy(dir, p, dl);
    dir[dl] = 0;
    d = path_cache_find(dir);
    if (!d || !d->exists) return;
    memcpy(leaf, slash + 1, n + 1);
    for (uint32_t o = 0; o < d->len; o += (uint32_t)strlen(d->names + o) + 1) {
        const char *e = d->names + o;
        if (leaf_eq(e, strlen(e), leaf, n)) {
            if (!add) {                                /* delete: cut the entry out of the listing (a re-listing per delete costs a full directory read each) */
                uint32_t el = (uint32_t)strlen(e) + 1;
                memmove(d->names + o, d->names + o + el, d->len - o - el);
                d->len -= el;
            }
            return;
        }
    }
    if (!add) return;
    if (d->len + n + 2 > 0x7fffffffu) { path_cache_drop(); return; }
    {
        char *nn = realloc(d->names, d->len + n + 2);
        if (!nn) { path_cache_drop(); return; }
        d->names = nn;
        memcpy(d->names + d->len, leaf, n + 1);
        d->len += (uint32_t)n + 1;
    }
}

/* Directories known to exist (so a write does not call mkdir on every path component each time). */
static char *g_known_dir[64];
static int   g_nknown;
static int known_dir(const char *p)
{
    for (int i = 0; i < g_nknown; i++) if (!strcmp(g_known_dir[i], p)) return 1;
    return 0;
}
static void known_dir_add(const char *p)
{
    if (g_nknown < 64 && (g_known_dir[g_nknown] = malloc(strlen(p) + 1)) != NULL) strcpy(g_known_dir[g_nknown++], p);
}

/* Existence check; with fix, the leaf of p is rewritten to the spelling the directory has. The game asks for
 * "X.WAV" and the file is "X.wav": a case-insensitive host open then scans the directory (16 ms in Azahar). */
static int host_lookup(char *p, int fix)
{
    char *slash = strrchr(p, '/');
    if (!slash || !slash[1]) return pal_is_dir(p);
    char dir[600];
    size_t dl = (size_t)(slash - p);
    if (dl >= sizeof dir) return 0;
    memcpy(dir, p, dl);
    dir[dl] = 0;
    const path_dir *d = path_cache_dir(dir);
    if (!d || !d->exists) return 0;
    size_t n = strlen(slash + 1);
    for (uint32_t o = 0; o < d->len; o += (uint32_t)strlen(d->names + o) + 1) {
        const char *e = d->names + o;
        if (leaf_eq(e, strlen(e), slash + 1, n)) { if (fix) memcpy(slash + 1, e, n); return 1; }
    }
    return 0;
}
static int host_exists(const char *p)
{
    char tmp[600];
    if (strlen(p) >= sizeof tmp) return 0;
    strcpy(tmp, p);
    return host_lookup(tmp, 0);
}

/* Search order: save overlay; the install dir (the game's working directory: the PC
 * installer fills it with GAMEDATA/BIN - exe, DLLs, SS*.BIK - plus big.lab unpacked
 * from BIG.Z), so after our install dir comes the CD's BIN; then CD GAMEDATA, GAMEDATA/GOBS. */
void hle_path_read(char *out, uint32_t n, const char *gn)
{
    char bin[512];
    snprintf(bin, sizeof bin, "%s/BIN", guest_data_dir());
    const char *bases[3] = {guest_save_dir(), guest_install_dir(), bin};
    for (int k = 0; k < 3; k++) {
        if (!bases[k]) continue;
        guest_path_in(out, n, bases[k], gn);
        if (host_lookup(out, 1)) return;
    }
    char gobs[512];
    snprintf(gobs, sizeof gobs, "%s/GOBS", guest_data_dir());
    guest_path_in(out, n, gobs, gn);
    if (host_lookup(out, 1)) return;
    guest_path(out, n, gn);
}

void hle_path_write(char *out, uint32_t n, const char *gn)
{
    guest_path_in(out, n, guest_save_dir() ? guest_save_dir() : guest_data_dir(), gn);
    size_t base = strlen(guest_save_dir() ? guest_save_dir() : "");
    for (size_t k = base + 1; out[k]; k++)
        if (out[k] == '/') {
            out[k] = 0;
            if (!known_dir(out)) {
                if (pal_mkdir(out)) path_cache_drop();        /* a new directory changes its parent's listing */
                if (pal_is_dir(out)) known_dir_add(out);
            }
            out[k] = '/';
        }
}

/* ------------------------------------------------------------ process */
static gptr g_cmdline, g_env, g_modname;

static gptr hle_strdup(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    gptr p = gheap_alloc(n);
    if (p) hle_str_out(p, n, s);
    return p;
}

#define EXE_PATH "C:\\TPM\\GAMEDATA\\BIN\\WMAIN.EXE"

static void k_GetVersion(cpu_t *c)        { hle_ret(c, 0xc0000a04u, 0); }   /* Windows 98 */
static void k_GetCommandLineA(cpu_t *c)   { hle_ret(c, g_cmdline, 0); }
static void k_GetEnvironmentStrings(cpu_t *c) { hle_ret(c, g_env, 0); }
static void k_GetEnvironmentStringsW(cpu_t *c){ g_hle_last_error = ERROR_CALL_NOT_IMPLEMENTED; hle_ret(c, 0, 0); }
static void k_FreeEnvironmentStrings(cpu_t *c){ hle_ret(c, 1, 1); }
static void k_SetEnvironmentVariableA(cpu_t *c){ hle_ret(c, 1, 2); }
static void k_GetStartupInfoA(cpu_t *c)
{
    gptr p = ARG(0);
    for (uint32_t k = 0; k < 68; k += 4) gmem_wr32(p + k, 0);
    gmem_wr32(p, 68);
    hle_ret(c, 0, 1);
}
static void k_GetModuleHandleA(cpu_t *c)
{
    char name[64];
    if (!ARG(0)) { hle_ret(c, GUEST_EXE_BASE, 1); return; }
    hle_str_in(name, sizeof name, ARG(0));
    for (int i = 1; i < MAX_HANDLES; i++)
        if (g_h[i].type == HT_MODULE && !strcmp(g_h[i].text, name)) { hle_ret(c, 0x100u + 4u * (uint32_t)i, 1); return; }
    g_hle_last_error = 126;
    hle_ret(c, 0, 1);
}
static void k_GetModuleFileNameA(cpu_t *c)
{
    uint32_t n = ARG(2);
    hle_str_out(ARG(1), n, EXE_PATH);
    uint32_t len = (uint32_t)strlen(EXE_PATH);
    hle_ret(c, len < n ? len : n, 3);
}
static void k_LoadLibraryA(cpu_t *c)
{
    char name[64];
    hle_str_in(name, sizeof name, ARG(0));
    for (int i = 1; i < MAX_HANDLES; i++)
        if (g_h[i].type == HT_MODULE && !strcmp(g_h[i].text, name)) { hle_ret(c, 0x100u + 4u * (uint32_t)i, 1); return; }
    uint32_t h = hh_alloc(HT_MODULE);
    if (h) snprintf(hh_get(h, HT_MODULE)->text, 128, "%s", name);
    pal_log("hle: LoadLibraryA(%s) -> %08lx", name, (unsigned long)h);
    hle_ret(c, h, 1);
}
static void k_GetProcAddress(cpu_t *c)
{
    hle_handle *m = hh_get(ARG(0), HT_MODULE);
    char name[64];
    if (ARG(1) < 0x10000u) snprintf(name, sizeof name, "#%u", (unsigned)ARG(1));
    else hle_str_in(name, sizeof name, ARG(1));
    const char *dll = m ? m->text : (ARG(0) == GUEST_EXE_BASE ? "WMAIN.EXE" : "?");
    uint32_t t = hle_thunk_for(dll, name);
    pal_log("hle: GetProcAddress(%s, %s) -> %08lx", dll, name, (unsigned long)t);
    hle_ret(c, t, 2);
}
static void k_ExitProcess(cpu_t *c)
{
    guest_set_exit_code(ARG(0));
    guest_stop(c, GUEST_STOP_EXIT, "ExitProcess");
}
static void k_TerminateProcess(cpu_t *c)
{
    guest_set_exit_code(ARG(1));
    guest_stop(c, GUEST_STOP_EXIT, "TerminateProcess");
}
static void k_RaiseException(cpu_t *c)
{
    char why[64];
    snprintf(why, sizeof why, "RaiseException %08lx", (unsigned long)ARG(0));
    guest_stop(c, GUEST_STOP_TRAP, why);
}
static void k_DebugBreak(cpu_t *c) { guest_stop(c, GUEST_STOP_TRAP, "DebugBreak"); }
static void k_UnhandledExceptionFilter(cpu_t *c) { hle_ret(c, 0, 1); }
static void k_RtlUnwind(cpu_t *c)
{
    /* Remove SEH frames above the target frame. Handlers of removed frames are
     * not run (they only matter for __try/__finally, which this exe reaches
     * through the CRT's longjmp path only). */
    gptr target = ARG(0);
    if (target) gmem_wr32(c->fs_base, target);
    hle_ret(c, 0, 4);
}
static void k_OutputDebugStringA(cpu_t *c)
{
    char s[256];
    hle_str_in(s, sizeof s, ARG(0));
    pal_log("guest dbg: %s", s);
    hle_ret(c, 0, 1);
}
static void k_GetLastError(cpu_t *c) { hle_ret(c, g_hle_last_error, 0); }
static void k_GetCurrentThread(cpu_t *c) { hle_ret(c, 0xfffffffeu, 0); }
static void k_GetCurrentProcess(cpu_t *c) { hle_ret(c, 0xffffffffu, 0); }
static void k_SetThreadPriority(cpu_t *c) { hle_ret(c, 1, 2); }
static void k_GetThreadPriority(cpu_t *c) { hle_ret(c, 0, 1); }
uint64_t g_sleep0_spin_us;   /* time the game's frame limiter spent spinning on Sleep(0): the frame's headroom (native_world.c adaptive distance) */
static void k_Sleep(cpu_t *c)
{
    guest_watchdog(c);
    if (ARG(0) == 0) {   /* diagnostic: gap between consecutive Sleep(0) polls (the game's frame limiter spins on it) */
        static uint64_t last, sum, mx; static uint32_t n;
        uint64_t now = pal_time_us();
        if (last && now - last < 5000) { g_sleep0_spin_us += now - last; sum += now - last; if (now - last > mx) mx = now - last; n++; }
        last = now;
        if (n && n % 2000 == 0) { pal_log("limiter: %lu polls, avg gap %lu us, max %lu us", (unsigned long)n, (unsigned long)(sum / n), (unsigned long)mx); sum = 0; mx = 0; n++; }
    }
    gthread_sleep(ARG(0));
    hle_ret(c, 0, 1);
}
static void k_GlobalMemoryStatus(cpu_t *c)
{
    gptr p = ARG(0);
    gmem_wr32(p + 0, 32);
    gmem_wr32(p + 4, 50);
    gmem_wr32(p + 8, 64u << 20);                 /* total phys: a 64 MB PC */
    gmem_wr32(p + 12, gheap_free_bytes());
    gmem_wr32(p + 16, 128u << 20);
    gmem_wr32(p + 20, 96u << 20);
    gmem_wr32(p + 24, 0x7ff00000u);
    gmem_wr32(p + 28, gvirt_free_bytes());
    hle_ret(c, 0, 1);
}
static void k_GetDiskFreeSpaceA(cpu_t *c)
{
    if (ARG(1)) gmem_wr32(ARG(1), 8);
    if (ARG(2)) gmem_wr32(ARG(2), 512);
    if (ARG(3)) gmem_wr32(ARG(3), 0x40000);      /* 1 GB free */
    if (ARG(4)) gmem_wr32(ARG(4), 0x80000);
    hle_ret(c, 1, 5);
}

/* ------------------------------------------------------------ heap / virtual */
static void k_HeapCreate(cpu_t *c)  { hle_ret(c, hh_alloc(HT_HEAP), 3); }
static void k_HeapDestroy(cpu_t *c) { hh_free(ARG(0)); hle_ret(c, 1, 1); }
static void k_HeapAlloc(cpu_t *c)
{
    gptr p = gheap_alloc(ARG(2));
    if (!p) g_hle_last_error = ERROR_NOT_ENOUGH_MEMORY;
    hle_ret(c, p, 3);
}
static void k_HeapFree(cpu_t *c) { gheap_free(ARG(2)); hle_ret(c, 1, 3); }
static void k_HeapReAlloc(cpu_t *c)
{
    gptr p = ARG(2);
    uint32_t n = ARG(3);
    if (ARG(1) & 0x10u) {                        /* HEAP_REALLOC_IN_PLACE_ONLY */
        hle_ret(c, n <= gheap_size(p) ? p : 0, 4);
        return;
    }
    hle_ret(c, gheap_realloc(p, n), 4);
}
static void k_VirtualAlloc(cpu_t *c)
{
    gptr a = ARG(0);
    uint32_t n = ARG(1), type = ARG(2);
    gptr r;
    if (a && (type & 0x1000u) && !(type & 0x2000u))       /* commit inside a reservation */
        r = gvirt_contains(a, n) ? (a & ~0xfffu) : 0;
    else
        r = gvirt_reserve(a, n);
    if (!r) g_hle_last_error = ERROR_NOT_ENOUGH_MEMORY;
    hle_ret(c, r, 4);
}
static void k_VirtualFree(cpu_t *c)
{
    uint32_t type = ARG(2);
    hle_ret(c, (type & 0x8000u) ? (uint32_t)gvirt_release(ARG(0)) : 1u, 3);
}

/* ------------------------------------------------------------ files */
static void k_GetStdHandle(cpu_t *c)
{
    uint32_t which = ARG(0);
    static uint32_t std[3];
    int k = which == 0xfffffff6u ? 0 : which == 0xfffffff5u ? 1 : which == 0xfffffff4u ? 2 : -1;
    if (k < 0) { hle_ret(c, INVALID_HANDLE, 1); return; }
    if (!std[k]) std[k] = hh_alloc(HT_STD);
    hle_ret(c, std[k], 1);
}
static void k_SetStdHandle(cpu_t *c) { hle_ret(c, 1, 2); }
static void k_SetHandleCount(cpu_t *c) { hle_ret(c, ARG(0), 1); }
static void k_GetFileType(cpu_t *c)
{
    uint32_t h = ARG(0);
    hle_ret(c, hh_get(h, HT_FILE) ? 1u : hh_get(h, HT_STD) ? 2u : 0u, 1);
}

static void k_CreateFileA_impl(cpu_t *c);
static void k_CreateFileA(cpu_t *c)
{
    mixer_service_io();
    uint64_t t0 = pal_time_us();
    k_CreateFileA_impl(c);
    uint64_t dt = pal_time_us() - t0;
    if (dt > 15000) pal_log("io: slow CreateFileA took %lu ms", (unsigned long)(dt / 1000));
}
static void k_CreateFileA_impl(cpu_t *c)
{
    char gn[260], path[600];
    hle_str_in(gn, sizeof gn, ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    int write = (access & 0x40000000u) != 0;
    pal_fh *f = NULL;
    vs_entry *me = NULL, *ce = NULL;
    sink_t *sink = NULL;
    char ckey[600], ppath[620];
    const char *open_as = NULL;
    int part = 0;
    ckey[0] = 0;
    if (!write && (g_skip_level == 2 || g_battle_mode) && strlen(gn) > 4 && (path_ieq(gn + strlen(gn) - 4, ".bil") || (path_ieq(gn + strlen(gn) - 4, ".bik") && !strstr(gn, "ss") && !strstr(gn, "SS")))) {   /* menu.txt: the intro movies are not opened at all */
        pal_log("hle: CreateFileA(%s) -> skipped (menu.txt)", gn);
        g_hle_last_error = ERROR_FILE_NOT_FOUND;
        hle_ret(c, INVALID_HANDLE, 7);
        return;
    }
    if (!write) {
        hle_path_read(path, sizeof path, gn);
        if (disp == 3 || disp == 4) {
            if (g_nlz) me = lz_get(c, gn);
            if (!me && is_voice_wav(path)) { if (!g_nlz) { pf_start(path); pf_adopt(); } me = vs_find(path); pal_log("vs: %s %s (store %lu KB, prefetch %d/%d)", me ? "hit " : "MISS", gn, (unsigned long)(g_vs_bytes >> 10), g_pf.consumed, g_pf.n); }
            if (!me) { f = park_take(path); if (!f) f = pal_fh_open(path, PAL_FH_READ); }
        }
    } else {
        char src[600];
        hle_path_read(src, sizeof src, gn);
        hle_path_write(path, sizeof path, gn);
        if (g_lz_busy && disp == 2 && is_voice_wav(path) && (sink = sink_new(path)) != NULL) {   /* lazy line (lz_get): RAM only */
            sink->ram_only = 1;
            guest_path_in(ckey, sizeof ckey, guest_save_dir(), gn);
            goto have_file;
        }
        if (disp == 2 && guest_save_dir() && is_voice_wav(path)) {       /* a voice line the game deletes at every level load: see cf_* */
            guest_path_in(ckey, sizeof ckey, guest_save_dir(), gn);
            cf_ent *te = cf_find(ckey);
            if (te) cf_wait(te);
            if (te && __sync_bool_compare_and_swap(&te->state, 1, 4)) {
                if ((sink = sink_new(path)) != NULL) { path_cache_note(path, 1); goto have_file; }
                te->state = 1;
            }
        }
        if (is_voice_wav(path) && (disp == 1 || disp == 2 || disp == 5) && strlen(path) < 590) {   /* written as .part, renamed when closed */
            snprintf(ppath, sizeof ppath, "%s.part", path);
            open_as = ppath; part = 1;
        }
        int exists = host_exists(path);
        if (!exists && strcmp(src, path) && host_exists(src) && disp != 2 && disp != 1) {
            /* copy-on-write: the original stays untouched, the overlay gets a copy */
            pal_fh *in = pal_fh_open(src, PAL_FH_READ), *out = pal_fh_open(path, PAL_FH_RW_TRUNCATE);
            char buf[4096];
            int64_t got;
            while (in && out && (got = pal_fh_read(in, buf, sizeof buf)) > 0) pal_fh_write(out, buf, (size_t)got);
            pal_fh_close(in);
            pal_fh_close(out);
            exists = 1;
        }
        if (!open_as) open_as = path;
        switch (disp) {
        case 1: f = exists ? NULL : pal_fh_open(open_as, PAL_FH_RW_TRUNCATE);
                if (exists) g_hle_last_error = ERROR_FILE_EXISTS;
                break;
        case 2: f = pal_fh_open(open_as, PAL_FH_RW_TRUNCATE); break;
        case 3: f = exists ? pal_fh_open(path, PAL_FH_RW_EXISTING) : NULL; break;
        case 4: f = pal_fh_open(path, PAL_FH_RW_CREATE); break;
        case 5: f = exists ? pal_fh_open(open_as, PAL_FH_RW_TRUNCATE) : NULL; break;
        }
    }
have_file:;
    uint32_t h = 0;
    if (f && write) {
        path_cache_note(path, 1);
        if (is_voice_wav(path)) {
            if (disp == 1 || disp == 2 || disp == 5) ce = vs_begin(path);
            else vs_drop(path);
        }
    }
    if ((f || me || sink) && (h = hh_alloc(HT_FILE)) != 0) {
        hle_handle *hd = hh_get(h, HT_FILE);
        hd->fh = f;
        hd->index = 0;
        hd->aux = NULL;
        hd->ra_virtual = 0;
        hd->mpos = 0;
        g_wpath[(h - 0x100u) / 4u][0] = 0;
        if (me) { hd->index = 3; hd->aux = me; hd->mpos = 0; }
        else if (sink) { hd->index = 4; hd->aux = sink; strcpy(g_rpath[(h - 0x100u) / 4u], ckey); sink = NULL; }
        else if (ce) {
            hd->index = 2; hd->aux = ce;
            if (part) strcpy(g_wpath[(h - 0x100u) / 4u], path);
            if (guest_save_dir()) { if (!ckey[0]) guest_path_in(ckey, sizeof ckey, guest_save_dir(), gn); strcpy(g_rpath[(h - 0x100u) / 4u], ckey); }
            else g_rpath[(h - 0x100u) / 4u][0] = 0;
        }
        else if (!write && strlen(path) < sizeof g_rpath[0]) {
            strcpy(g_rpath[(h - 0x100u) / 4u], path); hd->index = 1;   /* index 1: read-only, path known */
            size_t pl = strlen(path);
            if (pl > 4 && (path_ieq(path + pl - 4, ".bik") || path_ieq(path + pl - 4, ".bil"))) { hd->aux = (void *)2; g_movies_open++; hle_movie_touch(3000); if (path_ieq(path + pl - 4, ".bik")) g_menu_reached = 1; }
            { const char *b = path, *q; for (q = path; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
              if (pl > 4 && !strncasecmp(b, "logo.", 5)) { extern int g_logo_open; g_logo_open++; hd->aux = hd->aux ? (void *)3 : (void *)4; } }
        }
    }
    else {
        if (f) pal_fh_close(f);
        if (ce) vs_free(ce);
    }
    if (sink) { free(sink->buf); free(sink); sink = NULL; }   /* no handle left for it */
    pal_log("hle: CreateFileA(%s, %s, disp %lu) -> %s", gn, write ? "rw" : "r", (unsigned long)disp,
            h ? "ok" : "FAIL");
    if (!h && getenv("TPM_TRACE_OPEN")) {   /* debug: code addresses on the guest stack */
        char bt[200];
        int bl = 0, k = 0;
        for (uint32_t o = 0; o < 0x400 && k < 10; o += 4) {
            uint32_t w = gmem_rd32(c->r[ESP] + o);
            if (w >= 0x401000u && w < 0x4a8000u) { bl += snprintf(bt + bl, sizeof bt - (size_t)bl, " %08lx", (unsigned long)w); k++; }
        }
        pal_log("hle:   stack%s", bt);
    }
    if (!h) { if (g_hle_last_error != ERROR_FILE_EXISTS) g_hle_last_error = ERROR_FILE_NOT_FOUND; h = INVALID_HANDLE; }
    hle_ret(c, h, 7);
}

/* Read-ahead for streamed files (iMUSE music reads 512-2048 bytes at a time; every refill of the stdio buffer is a
 * ~6 ms SD access = a hitch every ~1.3 s). After three back-to-back small sequential reads on a read-only handle,
 * a helper thread keeps the next 32 KB blocks of that file in memory (its own file handle); ReadFile then copies.
 * One stream at a time. The helper only ever writes a slot in state 1 (requested); the main thread only reads
 * slots in state 2 (ready) and re-requests a slot when state is 0. */
#define RA_BLK (32u * 1024u)
typedef struct { uint8_t *buf; int64_t off; uint32_t len; volatile int state; uint32_t gen; char path[600]; } ra_slot;
static struct {
    uint32_t h;                  /* bound handle (0: none) */
    uint32_t gen;                /* bumped at every bind/unbind: slots of other generations are dead */
    int64_t  last_end;           /* end of the previous read (pattern detection) */
    int      seq;
    char     path[600];
    ra_slot  s[2];
    pal_sem *req;
    pal_thread *t;
    volatile int stop;
    char     hpath[600];         /* helper-owned */
    pal_fh  *hf;
} g_ra;

static void ra_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pal_sem_wait(g_ra.req);
        if (g_ra.stop) break;
        for (int i = 0; i < 2; i++) {
            ra_slot *sl = &g_ra.s[i];
            if (sl->state != 1) continue;
            if (!g_ra.hf || strcmp(g_ra.hpath, sl->path)) {
                if (g_ra.hf) pal_fh_close(g_ra.hf);
                strcpy(g_ra.hpath, sl->path);
                g_ra.hf = pal_fh_open(g_ra.hpath, PAL_FH_READ);
            }
            int64_t r = -1;
            if (g_ra.hf && pal_fh_seek(g_ra.hf, sl->off, 0) == sl->off) r = pal_fh_read(g_ra.hf, sl->buf, RA_BLK);
            sl->len = r > 0 ? (uint32_t)r : 0;
            __sync_synchronize();
            sl->state = 2;
        }
    }
    if (g_ra.hf) { pal_fh_close(g_ra.hf); g_ra.hf = NULL; }
}

static void ra_request(int64_t off)
{
    for (int i = 0; i < 2; i++) if (g_ra.s[i].state == 2 && g_ra.s[i].gen != g_ra.gen) g_ra.s[i].state = 0;
    for (int i = 0; i < 2; i++) {            /* already present or in flight */
        ra_slot *sl = &g_ra.s[i];
        if (sl->state && sl->gen == g_ra.gen && sl->off == off) return;
    }
    for (int i = 0; i < 2; i++) {
        ra_slot *sl = &g_ra.s[i];
        if (sl->state == 0) { sl->off = off; sl->len = 0; sl->gen = g_ra.gen; strcpy(sl->path, g_ra.path); __sync_synchronize(); sl->state = 1; pal_sem_post(g_ra.req); return; }
    }
}

static void ra_unbind(void)
{
    if (!g_ra.h) return;
    g_ra.h = 0; g_ra.seq = 0; g_ra.gen++;
    for (int i = 0; i < 2; i++) if (g_ra.s[i].state == 2) g_ra.s[i].state = 0;   /* slots in flight are freed when the helper is done */
}

/* The handle's file position is virtual (hd->mpos, hd->aux set) while blocks are served; ra_sync makes the real one match. */
static void ra_sync(hle_handle *f)
{
    if (f->index == 1 && f->ra_virtual) { pal_fh_seek(f->fh, f->mpos, 0); f->ra_virtual = 0; }
}

static int64_t ra_read(uint32_t h, hle_handle *f, const char *path, uint8_t *dst, uint32_t n)
{
    pal_fh *fh = f->fh;
    int64_t pos = (int64_t)f->mpos;
    if (pos < 0) return -1;
    if (g_ra.h != h) {
        if (g_ra.h) { /* another stream owns the helper */ return -1; }
        if (pos == g_ra.last_end && g_ra.seq >= 0) g_ra.seq++; else g_ra.seq = 0;
        g_ra.last_end = pos + n;
        if (g_ra.seq < 3 || strlen(path) >= sizeof g_ra.path) return -1;
        if (!g_ra.t) {
            g_ra.req = pal_sem_new(0);
            if (!g_ra.req) { g_ra.seq = -1000000; return -1; }
            for (int i = 0; i < 2; i++) { g_ra.s[i].buf = malloc(RA_BLK); if (!g_ra.s[i].buf) { g_ra.seq = -1000000; return -1; } }
            g_ra.t = pal_thread_spawn_bg(ra_thread, NULL, 64 * 1024);
            if (!g_ra.t) { g_ra.seq = -1000000; return -1; }
        }
        strcpy(g_ra.path, path);
        g_ra.h = h;
        g_ra.gen++;
        ra_request(pos & ~(int64_t)(RA_BLK - 1));
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        ra_slot *sl = &g_ra.s[i];
        if (sl->state == 2 && sl->gen == g_ra.gen && pos >= sl->off && pos + n <= sl->off + sl->len) {
            memcpy(dst, sl->buf + (pos - sl->off), n);
            f->mpos = (uint32_t)(pos + n); f->ra_virtual = 1;
            int64_t blk = sl->off;
            if (pos + n > blk + RA_BLK / 2) ra_request(blk + RA_BLK);
            for (int k = 0; k < 2; k++) {        /* release blocks fully behind the position */
                ra_slot *o = &g_ra.s[k];
                if (o->state == 2 && o->off + o->len <= pos) o->state = 0;
            }
            return n;
        }
    }
    ra_request(pos & ~(int64_t)(RA_BLK - 1));    /* miss: ask for the block, read normally this time */
    return -1;
}

static void k_ReadFile(cpu_t *c)
{
    mixer_service_io();
    hle_handle *f = hh_get(ARG(0), HT_FILE);
    uint32_t n = ARG(2), got = 0;
    if (f && n) {
        uint8_t *dst = gmem_span(ARG(1), n, 1);
        if (dst) {
            uint64_t t0 = pal_time_us();
            int64_t r;
            if (f->index == 4) {
                r = sink_read(f->aux, dst, n);
            } else if (f->index == 3) {
                const vs_entry *e = f->aux;
                uint32_t k = f->mpos < e->size ? e->size - f->mpos : 0;
                if (k > n) k = n;
                memcpy(dst, e->data + f->mpos, k);
                f->mpos += k;
                r = k;
            } else if (f->index == 1 && n <= 8192 && (r = ra_read(ARG(0), f, g_rpath[(ARG(0) - 0x100u) / 4u], dst, n)) >= 0) {
                /* served from the read-ahead blocks (position tracked in mpos) */
            } else {
                if (f->index == 1) ra_sync(f);
                r = pal_fh_read(f->fh, dst, n);
                if (f->index == 1 && r > 0) f->mpos += (uint32_t)r;
            }
            got = r > 0 ? (uint32_t)r : 0;
            {   /* I/O statistics, logged every 500 reads (perf phase 14) */
                static uint64_t us, bytes; static uint32_t calls, big;
                uint64_t dt = pal_time_us() - t0;
                if (dt > 15000) pal_log("io: slow read %lu bytes took %lu ms", (unsigned long)n, (unsigned long)(dt / 1000));
                us += dt; bytes += got; calls++; if (n >= 65536u) big++;
                if (calls % 500 == 0) {
                    pal_log("io: %lu reads (%lu >=64K), %lu KB, %lu ms in pal_fh_read", (unsigned long)calls, (unsigned long)big,
                            (unsigned long)(bytes >> 10), (unsigned long)(us / 1000));
                }
            }
        }
    }
    if (ARG(3)) gmem_wr32(ARG(3), got);
    if (!f) g_hle_last_error = ERROR_INVALID_HANDLE;
    hle_ret(c, f != NULL, 5);
}

static void k_WriteFile(cpu_t *c)
{
    mixer_service_io();
    uint32_t h = ARG(0), n = ARG(2), put = 0;
    hle_handle *f = hh_get(h, HT_FILE);
    const uint8_t *src = gmem_span(ARG(1), n, 0);
    if (f && f->index == 4) { if (src && sink_write(f->aux, src, n)) put = n; }
    else if (f && src) {
        int64_t pos = f->index == 2 ? pal_fh_seek(f->fh, 0, 1) : -1;
        int64_t r = pal_fh_write(f->fh, src, n);
        put = r > 0 ? (uint32_t)r : 0;
        if (f->index == 2 && (pos < 0 || put != n || !vs_put(f->aux, (uint32_t)pos, src, n))) {   /* capture failed: forget it */
            vs_free(f->aux);
            f->index = 0;
            f->aux = NULL;
        }
    }
    else if (hh_get(h, HT_STD) && src) {
        char line[256];
        uint32_t k = n < sizeof line - 1 ? n : (uint32_t)sizeof line - 1;
        memcpy(line, src, k);
        line[k] = 0;
        pal_log("guest stdout: %s", line);
        put = n;
    }
    if (ARG(3)) gmem_wr32(ARG(3), put);
    hle_ret(c, f || hh_get(h, HT_STD), 5);
}

static void k_SetFilePointer(cpu_t *c)
{
    hle_handle *f = hh_get(ARG(0), HT_FILE);
    int64_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (int64_t)((uint64_t)gmem_rd32(ARG(2)) << 32 | ARG(1));
    if (!f) { g_hle_last_error = ERROR_INVALID_HANDLE; hle_ret(c, INVALID_HANDLE, 4); return; }
    int64_t pos;
    if (f->index == 4) {
        sink_t *sk = f->aux;
        int64_t base = ARG(3) == 0 ? 0 : ARG(3) == 1 ? (int64_t)sk->pos : (int64_t)sk->size;
        pos = base + off;
        if (pos < 0) pos = -1; else sk->pos = (uint32_t)pos;
    } else if (f->index == 3) {
        const vs_entry *e = f->aux;
        int64_t base = ARG(3) == 0 ? 0 : ARG(3) == 1 ? (int64_t)f->mpos : (int64_t)e->size;
        pos = base + off;
        if (pos < 0) pos = -1; else f->mpos = (uint32_t)pos;
    } else {
        if (f->index == 1) ra_sync(f);
        pos = pal_fh_seek(f->fh, off, (int)ARG(3));
        if (f->index == 1 && pos >= 0) f->mpos = (uint32_t)pos;
    }     /* FILE_BEGIN/CURRENT/END == SEEK_SET/CUR/END */
    if (ARG(2)) gmem_wr32(ARG(2), pos < 0 ? 0 : (uint32_t)(pos >> 32));
    hle_ret(c, pos < 0 ? INVALID_HANDLE : (uint32_t)pos, 4);
}

static void k_SetEndOfFile(cpu_t *c)
{
    mixer_service_io();   /* the level load issues hundreds of these in a row (each 10-40 ms on the SD card): keep the DSP fed */
    /* Only reached after a write; stdio cannot truncate portably. Log so it is visible. */
    { hle_handle *sf = hh_get(ARG(0), HT_FILE); if (sf && sf->index == 4) { sink_t *sk = sf->aux; sk->size = sk->pos; hle_ret(c, 1, 1); return; } }
    pal_log("hle: SetEndOfFile not supported (file keeps its length)");
    hle_ret(c, hh_get(ARG(0), HT_FILE) != NULL, 1);
}
static void k_FlushFileBuffers(cpu_t *c) { hle_ret(c, 1, 1); }

static void k_CloseHandle(cpu_t *c)
{
    mixer_service_io();
    uint32_t h = ARG(0);
    hle_handle *f = hh_get(h, HT_FILE);
    if (f) {
        if (f->index == 3) { /* RAM copy: nothing to close */ }
        else if (f->index == 4) { sink_close(f->aux, g_rpath[(h - 0x100u) / 4u]); f->aux = NULL; }
        else if (f->index == 1) {
            if ((f->aux == (void *)2 || f->aux == (void *)3) && g_movies_open > 0) { g_movies_open--; hle_movie_touch(3000); }
            if (f->aux == (void *)3 || f->aux == (void *)4) { extern int g_logo_open; if (g_logo_open > 0) g_logo_open--; }
            if (g_ra.h == h) ra_unbind();
            park_put(g_rpath[(h - 0x100u) / 4u], f->fh);
        }
        else {
            if (f->index == 2) {
                vs_entry *e = f->aux;
                if (e->size && e->size < e->cap) {          /* shrink the growth slack: the budget counts real bytes */
                    uint8_t *nd = realloc(e->data, e->size);
                    if (nd) { g_vs_bytes -= e->cap - e->size; e->data = nd; e->cap = e->size; }
                }
                e->complete = 1;
                { int64_t fsz = pal_fh_seek(f->fh, 0, 2); const char *ck = g_rpath[(h - 0x100u) / 4u]; if (fsz >= 0 && ck[0]) cf_set(ck, (uint32_t)fsz, 0); }
            }
            pal_fh_close(f->fh);
            if (g_wpath[(h - 0x100u) / 4u][0]) {
                char pp[320]; const char *fin = g_wpath[(h - 0x100u) / 4u];
                snprintf(pp, sizeof pp, "%s.part", fin);
                pal_file_rename(pp, fin);
                g_wpath[(h - 0x100u) / 4u][0] = 0;
            }
        }
    }
    if (hh_get(h, HT_STD)) { hle_ret(c, 1, 1); return; }
    hh_free(h);
    hle_ret(c, 1, 1);
}

static void k_DeleteFileA(cpu_t *c)
{
    mixer_service_io();   /* the level load issues hundreds of these in a row (each 10-40 ms on the SD card): keep the DSP fed */
    char gn[260], path[600];
    hle_str_in(gn, sizeof gn, ARG(0));
    int ok = 0;
    if (guest_save_dir()) {
        guest_path_in(path, sizeof path, guest_save_dir(), gn);
        cf_ent *ce = NULL;
        int known = 0;                                                       /* every voice file on the card is in the cf_ table */
        if (is_voice_wav(path)) { known = cf_scan(path); ce = cf_find(path); }
        if (ce) cf_wait(ce);
        if (ce && (ce->state == 1 || ce->state == 4)) ok = 0;              /* already deleted */
        else if (ce && ce->state == 0 && host_exists(path)) {              /* tombstone: the bytes stay on the card (see cf_*) */
            ce->state = 1; ok = 1; path_cache_note(path, 0); vs_drop(path);
            g_cf_last = pal_time_ms();
            if (!g_cf_reaper) g_cf_reaper = pal_thread_spawn_bg(cf_reaper, NULL, 32 * 1024);
        } else if (known && (!ce || ce->state == 2)) {                      /* not on the card: a failed delete there costs ~180 ms, hundreds per level load */
            ok = 0;
        } else {
            ok = pal_file_remove(path);
            if (ok) { path_cache_note(path, 0); vs_drop(path); if (ce) ce->state = 2; }
        }
        if (g_nlz && is_voice_wav(path)) {                                   /* a deferred line: nothing on the card, but it existed */
            lz_ent *z = lz_find(path);
            if (z) { lz_drop(path); vs_drop(path); ok = 1; }
        }
    }
    pal_log("hle: DeleteFileA(%s) -> %d", gn, ok);
    if (!ok) g_hle_last_error = ERROR_FILE_NOT_FOUND;
    hle_ret(c, (uint32_t)ok, 1);
}
/* Called by the native override of the game's "extract one voice line into save/cache" (FUN_00471c5c): when that file is still on
 * the card unchanged (deleted by the game's cache clean-up a moment ago, tombstone) the decode is skipped altogether. */
int hle_voice_cache_have(const char *gn)
{
    char key[600], wp[600];
    static int n;
    if (!guest_save_dir() || !g_ncf) return 0;
    guest_path_in(key, sizeof key, guest_save_dir(), gn);
    cf_ent *e = cf_find(key);
    if (!e) return 0;
    cf_wait(e);
    if (e->size < 64 || !__sync_bool_compare_and_swap(&e->state, 1, 0)) return 0;
    hle_path_write(wp, sizeof wp, gn);
    path_cache_note(wp, 1);
    if (n++ % 100 == 0) pal_log("io: voice cache: %d lines reused without decoding", n);
    return 1;
}
static void k_CreateDirectoryA(cpu_t *c)
{
    mixer_service_io();   /* the level load issues hundreds of these in a row (each 10-40 ms on the SD card): keep the DSP fed */
    char gn[260], path[600];
    hle_str_in(gn, sizeof gn, ARG(0));
    hle_path_write(path, sizeof path, gn);
    {
        int ok = pal_mkdir(path);
        if (ok) path_cache_drop();
        hle_ret(c, (uint32_t)(ok || pal_is_dir(path)), 2);
    }
}
static void k_RemoveDirectoryA(cpu_t *c) { hle_ret(c, 0, 1); }

/* ---- FindFirstFile: one directory, '*' and '?' wildcards, case-insensitive ---- */
static int wild(const char *p, const char *s)
{
    if (!strcmp(p, "*.*")) p = "*";
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (;; s++) { if (wild(p + 1, s)) return 1; if (!*s) return 0; }
        }
        if (!*s) return 0;
        if (*p != '?' && up((uint8_t)*p) != up((uint8_t)*s)) return 0;
    }
    return !*s;
}

/* Directory listings of the read-only game data are cached: the game scans gobs\ and music\ twice each during start-up and
 * every scan is a slow SD directory read. Listings are recorded while the first scan is read to its end. */
#define DC_DIRS 8
#define DC_MAX  400
typedef struct { char name[48]; uint32_t size; uint8_t is_dir; } dc_ent;
typedef struct { char path[200]; int n, complete; dc_ent *e; } dc_dir;
static dc_dir g_dc[DC_DIRS];
static int g_dc_n;
static dc_dir *dc_find(const char *path) { for (int i = 0; i < g_dc_n; i++) if (!strcmp(g_dc[i].path, path)) return &g_dc[i]; return NULL; }
static int dc_cacheable(const char *path) { return strstr(path, "/data/") != NULL && strlen(path) < sizeof g_dc[0].path; }
/* The lines of save/cache that are deferred (lz_*) are not on the card, but the game lists that directory after the extraction
 * (FindFirstFile .\cache\*.*) to learn which voice lines exist; without them every line falls back to a plain file name and the
 * dialogues are silent. After the real entries a scan of that directory therefore returns the deferred lines, too. */
static int lz_virt_next(hle_handle *f, gptr out)
{
    const char *pat = f->text + strlen(f->text) + 1;
    while ((int)f->mpos < g_nlz) {
        const lz_ent *z = &g_lz[f->mpos++];
        char key[600], dir[600];
        guest_path_in(key, sizeof key, guest_save_dir(), z->gn);
        strcpy(dir, key);
        char *sl = strrchr(dir, '/');
        if (!sl) continue;
        *sl = 0;
        const char *bn = sl + 1;
        if (!path_ieq(dir, f->text) || !wild(pat, bn) || host_exists(key)) continue;   /* on the card: the real scan listed it */
        const vs_entry *e = vs_find(key);
        for (uint32_t k = 0; k < 320; k += 4) gmem_wr32(out + k, 0);
        gmem_wr32(out, 0x20u);
        gmem_wr32(out + 32, e ? e->size : 60000u);
        hle_str_out(out + 44, 260, bn);
        return 1;
    }
    return 0;
}

static int find_next(hle_handle *f, gptr out)
{
    if (f->index == 9) return lz_virt_next(f, out);
    const char *pat = f->text + strlen(f->text) + 1;
    char name[260];
    int is_dir;
    uint32_t size;
    if (!f->dir && f->aux) {                       /* replay a cached listing (mpos = next entry) */
        dc_dir *d = (dc_dir *)f->aux;
        while ((int)f->mpos < d->n) {
            dc_ent *e = &d->e[f->mpos++];
            if (!wild(pat, e->name)) continue;
            for (uint32_t k = 0; k < 320; k += 4) gmem_wr32(out + k, 0);
            gmem_wr32(out, e->is_dir ? 0x10u : 0x20u);
            gmem_wr32(out + 32, e->size);
            hle_str_out(out + 44, 260, e->name);
            return 1;
        }
        return 0;
    }
    /* recording scan: list the directory fully, then replay (a wildcard scan reads everything anyway) */
    if (f->dir && f->index == 7) {
        dc_dir *d = (dc_dir *)f->aux;
        char nm[260]; int id; uint32_t sz;
        while (pal_dir_next(f->dir, nm, sizeof nm, &id, &sz)) {
            if (d->n < DC_MAX && strlen(nm) < sizeof d->e[0].name) { dc_ent *e = &d->e[d->n++]; strcpy(e->name, nm); e->size = sz; e->is_dir = (uint8_t)id; }
            else d->n = DC_MAX + 1;                  /* unusable: do not cache */
        }
        pal_dir_close(f->dir); f->dir = NULL; f->index = 0;
        if (d->n <= DC_MAX) d->complete = 1; else { d->n = 0; d->complete = 0; free(d->e); d->e = NULL; g_dc_n--; f->aux = NULL; return 0; }
        f->mpos = 0;
        return find_next(f, out);
    }
    if (!f->dir) return 0;                         /* already exhausted (or evicted): stays "no more files" */
    while (pal_dir_next(f->dir, name, sizeof name, &is_dir, &size)) {
        if (!wild(pat, name)) continue;
        if (g_ncf && !is_dir && cf_dead(f->text, name)) continue;          /* a voice file the game deleted (tombstone) */
        for (uint32_t k = 0; k < 320; k += 4) gmem_wr32(out + k, 0);
        gmem_wr32(out, is_dir ? 0x10u : 0x20u);
        gmem_wr32(out + 32, size);
        hle_str_out(out + 44, 260, name);
        return 1;
    }
    /* End of the scan: release the SD directory (and its FS archive) now. The game scans .\cache\*.wav on every level load
     * and never calls FindClose on that scan; each one used to strand an FS handle until the FS service refused every
     * further open (level file, saves) after about a dozen loads. */
    pal_dir_close(f->dir);
    f->dir = NULL;
    if (g_nlz && guest_save_dir()) { f->index = 9; f->mpos = 0; return lz_virt_next(f, out); }
    return 0;
}

/* Safety net for scans the game abandons before the end: remember the last few find handles holding an SD directory and,
 * when several are open at once, close the oldest one (its handle then just reports "no more files"). */
static uint32_t g_find_dirs[8];
static int g_find_slot;
extern int g_pal_dir_open;
static void find_evict(void)
{
    if (g_pal_dir_open < 6) return;
    for (int i = 0; i < 8; i++) {
        uint32_t h = g_find_dirs[(g_find_slot + i) & 7];
        hle_handle *f = h ? hh_get(h, HT_FIND) : NULL;
        if (f && f->dir && f->index != 7) {          /* index 7 = a directory listing being recorded: leave it */
            pal_log("hle: closing an abandoned directory scan (%d open)", g_pal_dir_open);
            pal_dir_close(f->dir);
            f->dir = NULL;
            return;
        }
    }
}

static void k_FindFirstFileA(cpu_t *c)
{
    mixer_service_io();   /* the level load issues hundreds of these in a row (each 10-40 ms on the SD card): keep the DSP fed */
    find_evict();
    char gn[260], path[600];
    hle_str_in(gn, sizeof gn, ARG(0));
    hle_path_read(path, sizeof path, gn);
    char *slash = strrchr(path, '/');
    uint32_t h = INVALID_HANDLE;
    if (slash) {
        *slash = 0;
        dc_dir *cd = dc_cacheable(path) ? dc_find(path) : NULL;
        pal_dir *d = (cd && cd->complete) ? NULL : pal_dir_open(path);
        if (cd && cd->complete) {                       /* served from the cache */
            uint32_t hn = hh_alloc(HT_FIND);
            if (hn) {
                hle_handle *f = hh_get(hn, HT_FIND);
                f->dir = NULL; f->aux = cd; f->mpos = 0; f->index = 0;
                size_t dl = strlen(path);
                if (dl + strlen(slash + 1) + 2 < sizeof f->text) { memcpy(f->text, path, dl + 1); strcpy(f->text + dl + 1, slash + 1); }
                if (find_next(f, ARG(1))) h = hn; else hh_free(hn);
            }
            pal_log("hle: FindFirstFileA(%s) -> %s (cached)", gn, h != INVALID_HANDLE ? "found" : "none");
            if (h == INVALID_HANDLE) g_hle_last_error = ERROR_FILE_NOT_FOUND;
            hle_ret(c, h, 2);
            return;
        }
        if (!d && guest_save_dir() && !getenv("TPM_OLD_CACHE")) {   /* TPM_OLD_CACHE: golden check against the pre-voice-fix reference */
            /* A wildcard never exists as a file, so hle_path_read falls through to the data dir.
             * Directories the game creates itself (.\cache\, the decoded voice clips) live in
             * the save overlay: search there too, or the voice cache is never indexed. */
            guest_path_in(path, sizeof path, guest_save_dir(), gn);
            slash = strrchr(path, '/');
            if (slash) { *slash = 0; d = pal_dir_open(path); }
        }
        if (!slash) d = NULL;
        uint32_t hn = d ? hh_alloc(HT_FIND) : 0;
        if (hn) {
            hle_handle *f = hh_get(hn, HT_FIND);
            f->dir = d;
            if (d) g_find_dirs[g_find_slot++ & 7] = hn;
            f->aux = NULL; f->index = 0;
            if (d && dc_cacheable(path) && g_dc_n < DC_DIRS && !dc_find(path)) {   /* first scan of a game-data directory: record it */
                dc_dir *nd = &g_dc[g_dc_n];
                nd->e = (dc_ent *)malloc(sizeof(dc_ent) * DC_MAX);
                if (nd->e) { strcpy(nd->path, path); nd->n = 0; nd->complete = 0; g_dc_n++; f->aux = nd; f->index = 7; }
            }
            size_t dl = strlen(path);
            if (dl + strlen(slash + 1) + 2 < sizeof f->text) {
                memcpy(f->text, path, dl + 1);
                strcpy(f->text + dl + 1, slash + 1);
            }
            if (find_next(f, ARG(1))) h = hn;
            else { if (f->dir) pal_dir_close(f->dir); hh_free(hn); }
        } else if (d) pal_dir_close(d);
    }
    pal_log("hle: FindFirstFileA(%s) -> %s", gn, h != INVALID_HANDLE ? "found" : "none");
    if (h == INVALID_HANDLE) g_hle_last_error = ERROR_FILE_NOT_FOUND;
    hle_ret(c, h, 2);
}
static void k_FindNextFileA(cpu_t *c)
{
    mixer_service_io();   /* the level load issues hundreds of these in a row (each 10-40 ms on the SD card): keep the DSP fed */
    hle_handle *f = hh_get(ARG(0), HT_FIND);
    int ok = f && find_next(f, ARG(1));
    if (!ok) g_hle_last_error = ERROR_NO_MORE_FILES;
    hle_ret(c, (uint32_t)ok, 2);
}
static void k_FindClose(cpu_t *c)
{
    hle_handle *f = hh_get(ARG(0), HT_FIND);
    if (f) { if (f->dir) pal_dir_close(f->dir); hh_free(ARG(0)); }
    hle_ret(c, f != NULL, 1);
}

/* ------------------------------------------------------------ time */
/* Deterministic clock: the game sees 1999-05-19 12:00:00 plus elapsed run time. */
#define BOOT_FILETIME 125713584000000000ull

/* Clamped game clock (g_clock_clamp): the game advances its simulation in fixed 1/32 s steps for however much
 * time passed since the last frame, so a frame that takes 49 ms makes it run 1.5 steps per frame, which costs
 * more time, which means more steps. The guest clock therefore advances by at most one 30 fps frame (33.3 ms)
 * between two presented frames (time beyond that only trickles at 1/8 speed so waits on the clock still end).
 * The game then slows down instead of snowballing; at full speed nothing changes. */
int g_clock_clamp;
static uint64_t g_gc_base, g_gc_last;
#define GC_FRAME_US 36000u
/* gaps over 80 ms are loads/blocking waits, not slow frames: those pass through unchanged */
static uint64_t gc_adv(uint64_t d) { return d <= GC_FRAME_US || d > 80000u || g_movies_open ? d : GC_FRAME_US + (d - GC_FRAME_US) / 8u; }
static uint64_t guest_time_us(void)
{
    uint64_t now = pal_time_us();
    return g_clock_clamp ? g_gc_base + gc_adv(now - g_gc_last) : now;
}
void hle_clock_present(void)
{
    if (!g_clock_clamp) return;
    uint64_t now = pal_time_us();
    g_gc_base += gc_adv(now - g_gc_last);
    g_gc_last = now;
}

static void k_QueryPerformanceFrequency(cpu_t *c) { gwr64(ARG(0), 1000000u); hle_ret(c, 1, 1); }
static void k_QueryPerformanceCounter(cpu_t *c)   { gwr64(ARG(0), guest_time_us()); hle_ret(c, 1, 1); }
static void w_timeGetTime(cpu_t *c)               { hle_ret(c, g_clock_clamp ? (uint32_t)(guest_time_us() / 1000u) : pal_time_ms(), 0); }

static void filetime_to_systemtime(uint64_t ft, gptr st)
{
    uint64_t ms = ft / 10000u;
    uint32_t msec = (uint32_t)(ms % 1000u);
    uint64_t secs = ms / 1000u;
    uint32_t days = (uint32_t)(secs / 86400u), rem = (uint32_t)(secs % 86400u);
    uint32_t dow = (days + 1u) % 7u;                   /* 1601-01-01 was a Monday */
    uint32_t y = 1601;
    for (;;) {
        uint32_t len = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366u : 365u;
        if (days < len) break;
        days -= len;
        y++;
    }
    static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint32_t m = 0;
    for (; m < 12; m++) {
        uint32_t len = mdays[m] + (m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
        if (days < len) break;
        days -= len;
    }
    gmem_wr16(st + 0, (uint16_t)y);
    gmem_wr16(st + 2, (uint16_t)(m + 1));
    gmem_wr16(st + 4, (uint16_t)dow);
    gmem_wr16(st + 6, (uint16_t)(days + 1));
    gmem_wr16(st + 8, (uint16_t)(rem / 3600u));
    gmem_wr16(st + 10, (uint16_t)(rem / 60u % 60u));
    gmem_wr16(st + 12, (uint16_t)(rem % 60u));
    gmem_wr16(st + 14, (uint16_t)msec);
}
static void k_GetLocalTime(cpu_t *c)
{
    filetime_to_systemtime(BOOT_FILETIME + (uint64_t)pal_time_ms() * 10000u, ARG(0));
    hle_ret(c, 0, 1);
}
static void k_FileTimeToLocalFileTime(cpu_t *c) { gwr64(ARG(1), (uint64_t)grd64s(ARG(0))); hle_ret(c, 1, 2); }
static void k_FileTimeToSystemTime(cpu_t *c)    { filetime_to_systemtime((uint64_t)grd64s(ARG(0)), ARG(1)); hle_ret(c, 1, 2); }
static void k_GetTimeZoneInformation(cpu_t *c)
{
    for (uint32_t k = 0; k < 172; k += 4) gmem_wr32(ARG(0) + k, 0);
    hle_ret(c, 0, 1);
}

/* ------------------------------------------------------------ locale (cp1252, ASCII rules) */
static void k_GetACP(cpu_t *c)  { hle_ret(c, 1252, 0); }
static void k_GetOEMCP(cpu_t *c) { hle_ret(c, 437, 0); }
static void k_GetCPInfo(cpu_t *c)
{
    gptr p = ARG(1);
    gmem_wr32(p, 1);
    gmem_wr8(p + 4, '?'); gmem_wr8(p + 5, 0);
    for (uint32_t k = 6; k < 20; k++) gmem_wr8(p + k, 0);
    hle_ret(c, 1, 2);
}
static void wide_unsupported(cpu_t *c, int nargs) { g_hle_last_error = ERROR_CALL_NOT_IMPLEMENTED; hle_ret(c, 0, nargs); }
static void k_LCMapStringW(cpu_t *c)    { wide_unsupported(c, 6); }
static void k_GetStringTypeW(cpu_t *c)  { wide_unsupported(c, 4); }
static void k_GetLocaleInfoW(cpu_t *c)  { wide_unsupported(c, 4); }
/* GetLocaleInfoA(lcid, type, buf, n): US English, ANSI 1252 */
static void k_GetLocaleInfoA(cpu_t *c)
{
    const char *v = NULL;
    switch (ARG(1) & 0xffffu) {
    case 0x0b: v = "437"; break;          /* LOCALE_IDEFAULTCODEPAGE */
    case 0x1004: v = "1252"; break;       /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x01: v = "0409"; break;         /* LOCALE_ILANGUAGE */
    case 0x03: v = "ENU"; break;          /* LOCALE_SABBREVLANGNAME */
    case 0x07: v = "USA"; break;          /* LOCALE_SABBREVCTRYNAME */
    case 0x0e: v = "."; break;            /* LOCALE_SDECIMAL */
    case 0x0f: v = ","; break;            /* LOCALE_STHOUSAND */
    }
    if (!v) { pal_log("hle: GetLocaleInfoA type %lx not known", (unsigned long)ARG(1)); g_hle_last_error = 87; hle_ret(c, 0, 4); return; }
    uint32_t len = (uint32_t)strlen(v) + 1;
    if (ARG(3) == 0) { hle_ret(c, len, 4); return; }
    if (ARG(3) < len) { g_hle_last_error = 122; hle_ret(c, 0, 4); return; }
    hle_str_out(ARG(2), ARG(3), v);
    hle_ret(c, len, 4);
}
static void k_SetLastError(cpu_t *c) { g_hle_last_error = ARG(0); hle_ret(c, 0, 1); }
static uint32_t g_error_mode;
static void k_SetErrorMode(cpu_t *c) { uint32_t o = g_error_mode; g_error_mode = ARG(0); hle_ret(c, o, 1); }
static void k_CompareStringW(cpu_t *c)  { wide_unsupported(c, 6); }

static uint32_t src_len(gptr s, uint32_t n)
{
    if (n != 0xffffffffu) return n;
    uint32_t k = 0;
    while (gmem_rd8(s + k)) k++;
    return k + 1;
}

static void k_LCMapStringA(cpu_t *c)
{
    uint32_t flags = ARG(1), n = src_len(ARG(2), ARG(3)), cap = ARG(5);
    if (!cap) { hle_ret(c, n, 6); return; }
    if (cap < n) { g_hle_last_error = 122; hle_ret(c, 0, 6); return; }
    for (uint32_t k = 0; k < n; k++) {
        uint8_t ch = gmem_rd8(ARG(2) + k);
        gmem_wr8(ARG(4) + k, (flags & 0x200u) ? up(ch) : (flags & 0x100u) ? lo(ch) : ch);
    }
    hle_ret(c, n, 6);
}
static void k_GetStringTypeA(cpu_t *c)
{
    uint32_t n = src_len(ARG(2), ARG(3));
    for (uint32_t k = 0; k < n; k++) {
        uint8_t ch = gmem_rd8(ARG(2) + k);
        uint16_t t = 0;
        if (ch >= 'A' && ch <= 'Z') t |= 0x101;
        if (ch >= 'a' && ch <= 'z') t |= 0x102;
        if (ch >= '0' && ch <= '9') t |= 0x84;
        if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) t |= 0x80;
        if (ch == ' ' || (ch >= 9 && ch <= 13)) t |= 8;
        if (ch == ' ' || ch == 9) t |= 0x40;
        if (ch < 32 || ch == 127) t |= 0x20;
        if (ch > 32 && ch < 127 && !(t & 0x104)) t |= 0x10;
        gmem_wr16(ARG(4) + 2 * k, t);
    }
    hle_ret(c, 1, 5);
}
static void k_CompareStringA(cpu_t *c)
{
    uint32_t n1 = src_len(ARG(2), ARG(3)), n2 = src_len(ARG(4), ARG(5));
    if (ARG(3) == 0xffffffffu) n1--;
    if (ARG(5) == 0xffffffffu) n2--;
    int icase = ARG(1) & 1u, r = 0;
    for (uint32_t k = 0; !r && k < n1 && k < n2; k++) {
        uint8_t a = gmem_rd8(ARG(2) + k), b = gmem_rd8(ARG(4) + k);
        if (icase) { a = lo(a); b = lo(b); }
        r = (a > b) - (a < b);
    }
    if (!r) r = (n1 > n2) - (n1 < n2);
    hle_ret(c, (uint32_t)(r + 2), 6);
}
static void k_MultiByteToWideChar(cpu_t *c)
{
    uint32_t n = src_len(ARG(2), ARG(3)), cap = ARG(5);
    if (cap) {
        if (cap < n) { g_hle_last_error = 122; hle_ret(c, 0, 6); return; }
        for (uint32_t k = 0; k < n; k++) gmem_wr16(ARG(4) + 2 * k, gmem_rd8(ARG(2) + k));
    }
    hle_ret(c, n, 6);
}
static void k_WideCharToMultiByte(cpu_t *c)
{
    uint32_t n = ARG(3), cap = ARG(5);
    if (n == 0xffffffffu) { n = 0; while (gmem_rd16(ARG(2) + 2 * n)) n++; n++; }
    if (cap) {
        if (cap < n) { g_hle_last_error = 122; hle_ret(c, 0, 8); return; }
        for (uint32_t k = 0; k < n; k++) {
            uint16_t w = gmem_rd16(ARG(2) + 2 * k);
            gmem_wr8(ARG(4) + k, w < 256 ? (uint8_t)w : '?');
        }
    }
    hle_ret(c, n, 8);
}

/* ------------------------------------------------------------ console: logged, no-op */
static void k_ok0(cpu_t *c) { hle_ret(c, 1, 0); }
static void k_ok1(cpu_t *c) { hle_ret(c, 1, 1); }
static void k_ok2(cpu_t *c) { hle_ret(c, 1, 2); }
static void k_GetConsoleScreenBufferInfo(cpu_t *c)
{
    for (uint32_t k = 0; k < 22; k += 2) gmem_wr16(ARG(1) + k, 0);
    gmem_wr16(ARG(1), 80); gmem_wr16(ARG(1) + 2, 25);
    hle_ret(c, 1, 2);
}
static void k_FillConsoleOutputCharacterA(cpu_t *c) { if (ARG(4)) gmem_wr32(ARG(4), ARG(2)); hle_ret(c, 1, 5); }
static void k_WriteConsoleA(cpu_t *c)
{
    char s[256];
    uint32_t n = ARG(2) < sizeof s - 1 ? ARG(2) : (uint32_t)sizeof s - 1;
    for (uint32_t k = 0; k < n; k++) s[k] = (char)gmem_rd8(ARG(1) + k);
    s[n] = 0;
    pal_log("guest console: %s", s);
    if (ARG(3)) gmem_wr32(ARG(3), ARG(2));
    hle_ret(c, 1, 5);
}

/* ------------------------------------------------------------ profile (.ini) files */
static int ini_load(const char *gn, char *buf, uint32_t cap)
{
    char path[600];
    hle_path_read(path, sizeof path, gn);
    pal_fh *f = pal_fh_open(path, PAL_FH_READ);
    if (!f) return 0;
    int64_t n = pal_fh_read(f, buf, cap - 1);
    pal_fh_close(f);
    buf[n > 0 ? n : 0] = 0;
    return 1;
}
static int ieq(const char *a, const char *b, size_t n)
{
    for (size_t k = 0; k < n; k++) if (lo((uint8_t)a[k]) != lo((uint8_t)b[k])) return 0;
    return 1;
}
/* Finds key in section; returns pointer to the value and its length. */
static const char *ini_find(const char *buf, const char *sec, const char *key, size_t *vlen)
{
    int in = 0;
    for (const char *l = buf; *l;) {
        const char *e = l;
        while (*e && *e != '\n') e++;
        const char *s = l;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        size_t ll = (size_t)(e - s);
        while (ll && (s[ll - 1] == '\r' || s[ll - 1] == ' ')) ll--;
        if (ll && s[0] == '[') {
            in = ll >= strlen(sec) + 2 && s[strlen(sec) + 1] == ']' && ieq(s + 1, sec, strlen(sec));
        } else if (in && ll) {
            const char *eq = memchr(s, '=', ll);
            if (eq) {
                size_t kl = (size_t)(eq - s);
                while (kl && s[kl - 1] == ' ') kl--;
                if (kl == strlen(key) && ieq(s, key, kl)) {
                    const char *v = eq + 1;
                    while (v < s + ll && *v == ' ') v++;
                    *vlen = (size_t)(s + ll - v);
                    return v;
                }
            }
        }
        l = *e ? e + 1 : e;
    }
    return NULL;
}

static char g_ini[32768];
/* The last profile file stays cached: the game reads hundreds of keys at boot and
 * reopening the file for each costs ~10 ms on the 3DS SD card. Only our own
 * WritePrivateProfileString changes it, and that updates the cache. */
char g_popt_mask[16];   /* popt.txt: forced POPT0..11 (main.c) */
int g_perf_default_low;   /* set by the 3DS main: default the game's performance rating to Low */
static char g_ini_name[260];
static int  g_ini_cached, g_ini_exists;
static int ini_get(const char *gn)
{
    if (!g_ini_cached || strcmp(gn, g_ini_name)) {
        g_ini_exists = ini_load(gn, g_ini, sizeof g_ini);
        if (!g_ini_exists) g_ini[0] = 0;
        snprintf(g_ini_name, sizeof g_ini_name, "%s", gn);
        g_ini_cached = 1;
    }
    return g_ini_exists;
}

static void k_GetPrivateProfileStringA(cpu_t *c)
{
    char sec[128], key[128], def[256], file[260];
    gptr out = ARG(3);
    uint32_t cap = ARG(4);
    hle_str_in(sec, sizeof sec, ARG(0));
    hle_str_in(key, sizeof key, ARG(1));
    hle_str_in(def, sizeof def, ARG(2));
    hle_str_in(file, sizeof file, ARG(5));
    const char *v = NULL;
    size_t vl = 0;
    if (ARG(0) && ARG(1) && ini_get(file)) v = ini_find(g_ini, sec, key, &vl);
    char val[256];
    if (v) { if (vl > sizeof val - 1) vl = sizeof val - 1; memcpy(val, v, vl); val[vl] = 0; }
    else snprintf(val, sizeof val, "%s", def);
    /* The game's resolution follows the screen we report (widescreen: 800x480). */
    if (!strcmp(sec, "options") && !strcmp(key, "screen_width")) { snprintf(val, sizeof val, "%lu", (unsigned long)g_screen_w); v = val; }
    if (!strcmp(sec, "options") && !strcmp(key, "screen_height")) { snprintf(val, sizeof val, "%lu", (unsigned long)g_screen_h); v = val; }
    if (!strcmp(sec, "options") && !strcmp(key, "allow_all_modes") && g_screen_w != 640) { snprintf(val, sizeof val, "1"); v = val; }
    /* Menus above max_menu_* drop to 640x480 and the in-game return path does not come back:
     * allow menus at the screen size. */
    if (!strcmp(sec, "options") && !strcmp(key, "max_menu_Width")) { snprintf(val, sizeof val, "%lu", (unsigned long)g_screen_w); v = val; }
    if (!strcmp(sec, "options") && !strcmp(key, "max_menu_Height")) { snprintf(val, sizeof val, "%lu", (unsigned long)g_screen_h); v = val; }
    /* First run (no saved obi.ini values): the game would rate this machine "Superior" and switch on every performance
     * option (mirrors, shadows, trails, particles...). On the 3DS start at its own "Low" rating (perf_level 1, performance toggles
     * POPT0..5 off, the free POPT6..11 on); the Options menu still changes it and the game saves its choice as usual. */
    if (g_perf_default_low && !v && !strcmp(sec, "options")) {
        if (!strcmp(key, "evaluated")) { snprintf(val, sizeof val, "1"); v = val; }
        else if (!strcmp(key, "perf_level")) { snprintf(val, sizeof val, "1"); v = val; }
        else if (!strncmp(key, "POPT", 4) && key[4] >= '0' && key[4] <= '9') {
            /* Measured in Azahar (base 33.5 ms): every toggle but POPT3 is nearly free (all others on: 33.6-34.4 ms);
             * POPT3 alone costs ~+3.5 ms (the steam/particle effects) and stays off. */
            int idx = atoi(key + 4);
            snprintf(val, sizeof val, "%d", idx == 3 ? 0 : 1); v = val;
        }
    }
    /* popt.txt test hook: 12 digits 0/1 force POPT0..11 regardless of obi.ini (A/B of the game's own quality toggles) */
    { extern char g_popt_mask[16];
      if (g_popt_mask[0] && !strcmp(sec, "options") && !strncmp(key, "POPT", 4) && key[4] >= '0' && key[4] <= '9') {
          int idx = atoi(key + 4);
          if (idx < 12 && (g_popt_mask[idx] == '0' || g_popt_mask[idx] == '1')) { snprintf(val, sizeof val, "%c", g_popt_mask[idx]); v = val; }
      } }
    if (!ARG(0) || !ARG(1)) pal_log("hle: GetPrivateProfileStringA section/key enumeration not supported");
    hle_str_out(out, cap, val);
    uint32_t n = (uint32_t)strlen(val);
    pal_log("hle: ini %s [%s] %s = '%s'%s", file, sec, key, val, v ? "" : " (default)");
    hle_ret(c, cap && n >= cap ? cap - 1 : n, 6);
}

static void k_WritePrivateProfileStringA(cpu_t *c)
{
    char sec[128], key[128], val[256], file[260], path[600];
    hle_str_in(sec, sizeof sec, ARG(0));
    hle_str_in(key, sizeof key, ARG(1));
    hle_str_in(val, sizeof val, ARG(2));
    hle_str_in(file, sizeof file, ARG(3));
    if (!ARG(1) || !ARG(2)) { pal_log("hle: WritePrivateProfileStringA delete not supported"); hle_ret(c, 1, 4); return; }
    ini_get(file);
    static char out[sizeof g_ini + 512];
    size_t vl;
    const char *v = ini_find(g_ini, sec, key, &vl);
    if (v) {
        size_t pre = (size_t)(v - g_ini);
        snprintf(out, sizeof out, "%.*s%s%s", (int)pre, g_ini, val, v + vl);
    } else {
        /* append the key at the end of its section, or a new section at the end */
        char hdr[140];
        snprintf(hdr, sizeof hdr, "[%s]", sec);
        const char *h = NULL;
        for (const char *p = g_ini; (p = strchr(p, '[')) != NULL; p++)
            if (ieq(p, hdr, strlen(hdr))) { h = p; break; }
        if (h) {
            const char *eol = strchr(h, '\n');
            size_t pre = eol ? (size_t)(eol + 1 - g_ini) : strlen(g_ini);
            snprintf(out, sizeof out, "%.*s%s%s=%s\r\n%s", (int)pre, g_ini, eol ? "" : "\r\n", key, val, g_ini + pre);
        } else {
            snprintf(out, sizeof out, "%s%s[%s]\r\n%s=%s\r\n", g_ini,
                     g_ini[0] && g_ini[strlen(g_ini) - 1] != '\n' ? "\r\n" : "", sec, key, val);
        }
    }
    if (g_ini_exists && !strcmp(out, g_ini)) {          /* the game rewrites the same values on every start: an SD write each (50-100 ms) */
        hle_ret(c, 1, 4);
        return;
    }
    hle_path_write(path, sizeof path, file);
    pal_fh *f = pal_fh_open(path, PAL_FH_RW_TRUNCATE);
    if (f) {
        path_cache_note(path, 1);
        pal_fh_write(f, out, strlen(out));
        pal_fh_close(f);
        snprintf(g_ini, sizeof g_ini, "%s", out);
        g_ini_exists = 1;
    } else g_ini_cached = 0;
    pal_log("hle: ini write %s [%s] %s = '%s' -> %s", file, sec, key, val, f ? "ok" : "FAIL");
    hle_ret(c, f != NULL, 4);
}

/* ------------------------------------------------------------ table */
static const hle_def defs[] = {
    {"KERNEL32.dll", "GetVersion", k_GetVersion},
    {"KERNEL32.dll", "GetCommandLineA", k_GetCommandLineA},
    {"KERNEL32.dll", "GetEnvironmentStrings", k_GetEnvironmentStrings},
    {"KERNEL32.dll", "GetEnvironmentStringsW", k_GetEnvironmentStringsW},
    {"KERNEL32.dll", "FreeEnvironmentStringsA", k_FreeEnvironmentStrings},
    {"KERNEL32.dll", "FreeEnvironmentStringsW", k_FreeEnvironmentStrings},
    {"KERNEL32.dll", "SetEnvironmentVariableA", k_SetEnvironmentVariableA},
    {"KERNEL32.dll", "GetStartupInfoA", k_GetStartupInfoA},
    {"KERNEL32.dll", "GetModuleHandleA", k_GetModuleHandleA},
    {"KERNEL32.dll", "GetModuleFileNameA", k_GetModuleFileNameA},
    {"KERNEL32.dll", "LoadLibraryA", k_LoadLibraryA},
    {"KERNEL32.dll", "GetProcAddress", k_GetProcAddress},
    {"KERNEL32.dll", "ExitProcess", k_ExitProcess},
    {"KERNEL32.dll", "TerminateProcess", k_TerminateProcess},
    {"KERNEL32.dll", "RaiseException", k_RaiseException},
    {"KERNEL32.dll", "DebugBreak", k_DebugBreak},
    {"KERNEL32.dll", "UnhandledExceptionFilter", k_UnhandledExceptionFilter},
    {"KERNEL32.dll", "RtlUnwind", k_RtlUnwind},
    {"KERNEL32.dll", "OutputDebugStringA", k_OutputDebugStringA},
    {"KERNEL32.dll", "GetLastError", k_GetLastError},
    {"KERNEL32.dll", "GetCurrentThread", k_GetCurrentThread},
    {"KERNEL32.dll", "GetCurrentProcess", k_GetCurrentProcess},
    {"KERNEL32.dll", "SetThreadPriority", k_SetThreadPriority},
    {"KERNEL32.dll", "GetThreadPriority", k_GetThreadPriority},
    {"KERNEL32.dll", "Sleep", k_Sleep},
    {"KERNEL32.dll", "GlobalMemoryStatus", k_GlobalMemoryStatus},
    {"KERNEL32.dll", "GetDiskFreeSpaceA", k_GetDiskFreeSpaceA},
    {"KERNEL32.dll", "HeapCreate", k_HeapCreate},
    {"KERNEL32.dll", "HeapDestroy", k_HeapDestroy},
    {"KERNEL32.dll", "HeapAlloc", k_HeapAlloc},
    {"KERNEL32.dll", "HeapFree", k_HeapFree},
    {"KERNEL32.dll", "HeapReAlloc", k_HeapReAlloc},
    {"KERNEL32.dll", "VirtualAlloc", k_VirtualAlloc},
    {"KERNEL32.dll", "VirtualFree", k_VirtualFree},
    {"KERNEL32.dll", "GetStdHandle", k_GetStdHandle},
    {"KERNEL32.dll", "SetStdHandle", k_SetStdHandle},
    {"KERNEL32.dll", "SetHandleCount", k_SetHandleCount},
    {"KERNEL32.dll", "GetFileType", k_GetFileType},
    {"KERNEL32.dll", "CreateFileA", k_CreateFileA},
    {"KERNEL32.dll", "ReadFile", k_ReadFile},
    {"KERNEL32.dll", "WriteFile", k_WriteFile},
    {"KERNEL32.dll", "SetFilePointer", k_SetFilePointer},
    {"KERNEL32.dll", "SetEndOfFile", k_SetEndOfFile},
    {"KERNEL32.dll", "FlushFileBuffers", k_FlushFileBuffers},
    {"KERNEL32.dll", "CloseHandle", k_CloseHandle},
    {"KERNEL32.dll", "DeleteFileA", k_DeleteFileA},
    {"KERNEL32.dll", "CreateDirectoryA", k_CreateDirectoryA},
    {"KERNEL32.dll", "RemoveDirectoryA", k_RemoveDirectoryA},
    {"KERNEL32.dll", "FindFirstFileA", k_FindFirstFileA},
    {"KERNEL32.dll", "FindNextFileA", k_FindNextFileA},
    {"KERNEL32.dll", "FindClose", k_FindClose},
    {"KERNEL32.dll", "QueryPerformanceFrequency", k_QueryPerformanceFrequency},
    {"KERNEL32.dll", "QueryPerformanceCounter", k_QueryPerformanceCounter},
    {"KERNEL32.dll", "GetLocalTime", k_GetLocalTime},
    {"KERNEL32.dll", "FileTimeToLocalFileTime", k_FileTimeToLocalFileTime},
    {"KERNEL32.dll", "FileTimeToSystemTime", k_FileTimeToSystemTime},
    {"KERNEL32.dll", "GetTimeZoneInformation", k_GetTimeZoneInformation},
    {"KERNEL32.dll", "GetACP", k_GetACP},
    {"KERNEL32.dll", "GetOEMCP", k_GetOEMCP},
    {"KERNEL32.dll", "GetCPInfo", k_GetCPInfo},
    {"KERNEL32.dll", "LCMapStringA", k_LCMapStringA},
    {"KERNEL32.dll", "LCMapStringW", k_LCMapStringW},
    {"KERNEL32.dll", "GetStringTypeA", k_GetStringTypeA},
    {"KERNEL32.dll", "GetStringTypeW", k_GetStringTypeW},
    {"KERNEL32.dll", "GetLocaleInfoA", k_GetLocaleInfoA},
    {"KERNEL32.dll", "GetLocaleInfoW", k_GetLocaleInfoW},
    {"KERNEL32.dll", "SetLastError", k_SetLastError},
    {"KERNEL32.dll", "SetErrorMode", k_SetErrorMode},
    {"KERNEL32.dll", "CompareStringA", k_CompareStringA},
    {"KERNEL32.dll", "CompareStringW", k_CompareStringW},
    {"KERNEL32.dll", "MultiByteToWideChar", k_MultiByteToWideChar},
    {"KERNEL32.dll", "WideCharToMultiByte", k_WideCharToMultiByte},
    {"KERNEL32.dll", "AllocConsole", k_ok0},
    {"KERNEL32.dll", "FreeConsole", k_ok0},
    {"KERNEL32.dll", "SetConsoleTitleA", k_ok1},
    {"KERNEL32.dll", "FlushConsoleInputBuffer", k_ok1},
    {"KERNEL32.dll", "SetConsoleCursorPosition", k_ok2},
    {"KERNEL32.dll", "SetConsoleCursorInfo", k_ok2},
    {"KERNEL32.dll", "SetConsoleTextAttribute", k_ok2},
    {"KERNEL32.dll", "GetConsoleScreenBufferInfo", k_GetConsoleScreenBufferInfo},
    {"KERNEL32.dll", "FillConsoleOutputCharacterA", k_FillConsoleOutputCharacterA},
    {"KERNEL32.dll", "WriteConsoleA", k_WriteConsoleA},
    {"KERNEL32.dll", "GetPrivateProfileStringA", k_GetPrivateProfileStringA},
    {"KERNEL32.dll", "WritePrivateProfileStringA", k_WritePrivateProfileStringA},
    {"WINMM.dll", "timeGetTime", w_timeGetTime},
};

void hle_kernel32_init(void)
{
    memset(g_h, 0, sizeof g_h);
    g_hle_last_error = 0;
    g_ini_cached = 0;
    path_cache_drop();
    hle_register(defs, (int)(sizeof defs / sizeof defs[0]));
}

/* Called after the guest heap exists. */
void hle_kernel32_process_init(void)
{
    g_cmdline = hle_strdup("\"" EXE_PATH "\"");
    g_modname = hle_strdup(EXE_PATH);
    static const char env[] = "PATH=C:\\WINDOWS\0windir=C:\\WINDOWS\0\0";
    g_env = gheap_alloc(sizeof env);
    for (uint32_t k = 0; k < sizeof env; k++) gmem_wr8(g_env + k, (uint8_t)env[k]);
}
