#include <errno.h>
/* pal file backend on stdio; shared by the 3DS (newlib sdmc:) and host builds. */
#include <stdio.h>
#include <stdlib.h>
#include "pal/pal.h"
int g_pal_fh_open, g_pal_dir_open;   /* diagnostics: handles currently open (logged with the ram line) */

struct pal_file { FILE *fp; int64_t size; };

pal_file *pal_file_open(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    pal_file *f = malloc(sizeof *f);
    if (!f) { fclose(fp); return NULL; }
    f->fp = fp;
    f->size = (fseek(fp, 0, SEEK_END) == 0) ? (int64_t)ftell(fp) : -1;
    return f;
}

int64_t pal_file_size(pal_file *f) { return f->size; }

size_t pal_file_read_at(pal_file *f, uint64_t off, void *dst, size_t n)
{
    if (f->size < 0 || off > (uint64_t)f->size) return 0;
    if (fseek(f->fp, (long)off, SEEK_SET) != 0) return 0;
    return fread(dst, 1, n, f->fp);
}

void pal_file_close(pal_file *f)
{
    if (!f) return;
    fclose(f->fp);
    free(f);
}

/* Write buffer stays below the 64 KB "large block" threshold. SD writes are
 * much faster in big sequential chunks than in small ones. */
#define WBUF_SIZE (32 * 1024)
struct pal_wfile { FILE *fp; int err; char buf[WBUF_SIZE]; };

pal_wfile *pal_wfile_create(const char *path)
{
    pal_wfile *f = malloc(sizeof *f);
    if (!f) return NULL;
    f->fp = fopen(path, "wb");
    if (!f->fp) { free(f); return NULL; }
    f->err = 0;
    setvbuf(f->fp, f->buf, _IOFBF, sizeof f->buf);
    return f;
}

int pal_wfile_write(pal_wfile *f, const void *src, size_t n)
{
    if (!f->err && fwrite(src, 1, n, f->fp) != n) f->err = 1;
    return !f->err;
}

int pal_wfile_close(pal_wfile *f)
{
    if (!f) return 0;
    int ok = !f->err && fflush(f->fp) == 0;
    ok = (fclose(f->fp) == 0) && ok;
    free(f);
    return ok;
}

int pal_file_rename(const char *from, const char *to)
{
    remove(to);   /* Windows rename() does not replace */
    return rename(from, to) == 0;
}

int pal_file_remove(const char *path) { return remove(path) == 0; }

/* ---- general file handles ---- */
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

struct pal_fh { FILE *fp; };

pal_fh *pal_fh_open(const char *path, int mode)
{
    FILE *fp = NULL;
    switch (mode) {
    case PAL_FH_READ:        fp = fopen(path, "rb"); break;
    case PAL_FH_RW_EXISTING: fp = fopen(path, "rb+"); break;
    case PAL_FH_RW_CREATE:   fp = fopen(path, "rb+"); if (!fp) fp = fopen(path, "wb+"); break;
    case PAL_FH_RW_TRUNCATE: fp = fopen(path, "wb+"); break;
    }
    if (!fp) { if (errno != ENOENT) pal_log("io: fopen(%s) failed, errno %d (open files %d dirs %d)", path, errno, g_pal_fh_open, g_pal_dir_open); return NULL; }
    if (mode == PAL_FH_READ) setvbuf(fp, NULL, _IOFBF, 32768);   /* the game streams music/voices in ~5 KB reads: fewer SD calls */
    pal_fh *f = malloc(sizeof *f);
    if (!f) { fclose(fp); return NULL; }
    f->fp = fp;
    g_pal_fh_open++;
    return f;
}

/* Large reads (voice WAVs are 100-300 KB) bypass the stdio buffer on the 3DS: fread would move them in
 * 32 KB pieces, one SD call each, ~34 ms for 184 KB in Azahar. fflush on a read stream (newlib) discards
 * the buffer and puts the file descriptor at the logical position, so stdio stays consistent. */
#ifdef __3DS__
#include <unistd.h>
#endif
int64_t pal_fh_read(pal_fh *f, void *dst, size_t n)
{
#ifdef __3DS__
    if (n >= 65536u && fflush(f->fp) == 0) {
        int fd = fileno(f->fp);
        size_t got = 0;
        while (got < n) {
            int r = (int)read(fd, (char *)dst + got, n - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        return (int64_t)got;
    }
#endif
    return (int64_t)fread(dst, 1, n, f->fp);
}
int64_t pal_fh_write(pal_fh *f, const void *src, size_t n) { return (int64_t)fwrite(src, 1, n, f->fp); }

int64_t pal_fh_seek(pal_fh *f, int64_t off, int whence)
{
    if (fseek(f->fp, (long)off, whence) != 0) return -1;
    return (int64_t)ftell(f->fp);
}

void pal_fh_close(pal_fh *f)
{
    if (!f) return;
    g_pal_fh_open--;
    fclose(f->fp);
    free(f);
}

int pal_mkdir(const char *path)
{
#ifdef _WIN32
    return mkdir(path) == 0;
#else
    return mkdir(path, 0777) == 0;
#endif
}

int pal_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

#ifndef __3DS__   /* the 3DS backend reads directories natively (one call gives name, size, type) */
struct pal_dir { DIR *d; char base[512]; };

pal_dir *pal_dir_open(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return NULL;
    pal_dir *pd = malloc(sizeof *pd);
    if (!pd) { closedir(d); return NULL; }
    pd->d = d;
    snprintf(pd->base, sizeof pd->base, "%s", path);
    return pd;
}

int pal_dir_next(pal_dir *pd, char *name, size_t n, int *is_dir, uint32_t *size)
{
    struct dirent *e;
    while ((e = readdir(pd->d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(name, n, "%s", e->d_name);
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", pd->base, e->d_name);
        struct stat st;
        int ok = stat(full, &st) == 0;
        *is_dir = ok && S_ISDIR(st.st_mode);
        *size = ok ? (uint32_t)st.st_size : 0;
        return 1;
    }
    return 0;
}

void pal_dir_close(pal_dir *pd)
{
    if (!pd) return;
    closedir(pd->d);
    free(pd);
}
#endif

