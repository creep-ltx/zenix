/* SPDX-License-Identifier: GPL-2.0-only */
/* The CFile core's OS layer on Zenix (fs/os.h, "the ONE place that knows which Kickstart this is"; ports/ahiru/README):
 * upstream's fs/os.c is AmigaOS's (ExAll, assigns, the DosList, packets by hand for 1.3); this is the same interface
 * over the C library, with the meanings the core expects:
 *  - a directory's entries come from readdir and lstat, so a soft link is ST_SOFTLINK (never walked by a copy or a
 *    delete: the core's rule) and the rest ST_USERDIR or ST_FILE (a hard link is one more name of a file, as on any
 *    Unix); the protection carries the Unix mode as the AmigaOS layer's Examine does (ports/common: bits 8-19 and
 *    bit 31, and the r w e bits set when this user may not), and h for a name starting with '.' (Unix's hidden
 *    files: Ahiru's colour for hidden ones); days and minutes from 1978, AmigaOS's;
 *  - the volume list is the mount table (SYS_mountinfo): "/" and every mount below it, as volumes;
 *  - a lock's name is its absolute path with the soft links resolved (NameFromLock); free bytes from the mount's
 *    filesystem (Info);
 *  - patterns are Unix wildcards (fnmatch: * ? [...]), upstream's "#?" read as "*"; names match by case, as Zenix's
 *    names are;
 *  - a date is set with SetFileDate (SYS_utimes, whole seconds); os_launch runs the command in the background through
 *    the shell (SystemTags with SYS_Asynch); there are no requesters to keep quiet. */
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/dos.h>
#define ZENIX_LIBC                      /* usys.h: the C library's errno and constants */
#include "../../../user/usys.h"   /* SYS_mountinfo: the mount table */
#include "fs/os.h"
#include "fs/place.h"
#include "core/util.h"
#include "fs/shared.h"

UWORD v_exec = 40, v_dos = 40, v_int = 40, v_gfx = 40;   /* upstream takes the roads of AmigaOS 3.x (V36 and up) */

/* AmigaOS's NAME.info is NAME's icon and travels with it; on Zenix a NAME.info is a file of its own (Texinfo's ...):
 * deleting, moving or renaming NAME must not take an unrelated file with it, unasked (audit B3-5) */
int os_init(void) { fs_sh()->s_icons = 0; return 1; }
void os_exit(void) { }

void *xalloc(ULONG size) { return calloc(1, size ? size : 1); }
void xfree(void *p) { free(p); }

/* ---- directory scan ---- */

struct dscan {
    DIR *dir;
    char path[CPATH];
    int err;
};

struct dscan *ds_open(const char *path)
{
    struct dscan *d;
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (!l) return NULL;
    if (!(d = calloc(1, sizeof(*d)))) {
        UnLock(l);
        return NULL;
    }
    if (!NameFromLock(l, (STRPTR)d->path, sizeof(d->path)) || !(d->dir = opendir(d->path))) {
        UnLock(l);
        free(d);
        return NULL;                    /* (a file is not a directory: opendir says ENOTDIR) */
    }
    UnLock(l);
    return d;
}

int ds_next(struct dscan *d, struct dent *e)
{
    struct dirent *de;
    for (;;) {
        errno = 0;
        if (!(de = readdir(d->dir))) break;
        char p[CPATH];
        struct stat st;
        int n = (int)strlen(de->d_name);
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n > 107 || strlen(d->path) + 1 + (size_t)n >= 256) continue;    /* a name no call could open (PATH_MAX) */
        sfmt(p, sizeof(p), strcmp(d->path, "/") ? "%s/%s" : "%s%s", d->path, de->d_name);
        memset(e, 0, sizeof(*e));
        memcpy(e->name, de->d_name, (size_t)n + 1);
        if (lstat(p, &st) < 0) continue;        /* gone since the listing */
        e->type = S_ISLNK(st.st_mode) ? ST_SOFTLINK : S_ISDIR(st.st_mode) ? ST_USERDIR : ST_FILE;
        e->size = S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) ? (ULONG)st.st_size : 0;
        e->prot = (ULONG)(0x80000000UL | (unsigned long)(st.st_mode & 07777) << 8);
        if (access(p, W_OK) < 0) e->prot |= FIBF_WRITE;
        if (access(p, R_OK) < 0) e->prot |= FIBF_READ;
        if (!S_ISDIR(st.st_mode) && !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) e->prot |= FIBF_EXECUTE;
        if (de->d_name[0] == '.') e->prot |= 1UL << 7;          /* h: a hidden file (FIBF_HIDDEN, Ahiru's colour) */
        e->days = (LONG)(st.st_mtime / 86400) - AMIGA_EPOCH_DAYS;
        e->mins = (LONG)(st.st_mtime % 86400 / 60);
        return 1;
    }
    return errno ? -1 : 0;
}

void ds_close(struct dscan *d)
{
    if (!d) return;
    if (d->dir) closedir(d->dir);
    free(d);
}

/* ---- locks and names ---- */

int os_lockname(BPTR lock, char *dst, int size) { return NameFromLock(lock, (STRPTR)dst, size) != 0; }

int os_samelock(BPTR a, BPTR b)
{
    LONG r;
    if (!a || !b) return 0;
    r = SameLock(a, b);
    return r == LOCK_SAME ? 2 : r == LOCK_SAME_VOLUME ? 1 : 0;
}

void os_quiet(int on) { (void)on; }     /* no requesters on Zenix */

int os_pathtype(const char *path)
{
    BPTR l;
    struct FileInfoBlock fib;
    int t;
    if (!(l = Lock((STRPTR)path, ACCESS_READ))) return 0;
    t = Examine(l, &fib) && fib.fib_DirEntryType > 0 ? 2 : 1;
    UnLock(l);
    return t;
}

LONG os_freebytes(const char *path)
{
    struct InfoData id;
    BPTR l;
    LONG n = -1;
    if ((l = Lock((STRPTR)path, ACCESS_READ))) {
        if (Info(l, &id) && id.id_BytesPerBlock > 0) {
            ULONG nfree = (ULONG)(id.id_NumBlocks - id.id_NumBlocksUsed), bpb = (ULONG)id.id_BytesPerBlock;
            n = nfree > 0x7fffffffUL / bpb ? 0x7fffffff : (LONG)(nfree * bpb);
        }
        UnLock(l);
    }
    return n;
}

void os_fault(LONG err, char *dst, int size)
{
    if (Fault(err, NULL, (STRPTR)dst, size) <= 0 || !dst[0]) sfmt(dst, size, "error %ld", (long)err);
}

int os_setdate(const char *path, const struct DateStamp *ds) { return SetFileDate((STRPTR)path, ds) != 0; }

int os_islink(const char *path)
{
    struct stat st;
    char p[CPATH];
    size_t n = strlen(path);
    if (!n || n >= sizeof(p)) return 0;
    strcpy(p, path);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;    /* "link/" names the link too */
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}

/* ---- the volume list: the mount table ---- */

int os_volumes(void (*add)(void *ctx, const char *name, int kind), void *ctx)
{
    struct umountinfo mi;
    u32 i;
    int k = 0;
    for (i = 0; sys_mountinfo(i, &mi) == 0 && i < 64; i++) {
        char name[sizeof(mi.path) + 1];
        memcpy(name, mi.path, sizeof(mi.path));
        name[sizeof(mi.path)] = 0;
        if (name[0] != '/') continue;
        add(ctx, name, VL_VOLUME);
        k++;
    }
    return k;
}

/* ---- patterns: Unix wildcards ---- */

static char *pat;

int os_patprep(const char *p)
{
    size_t i, n = 0, l = strlen(p);
    os_patfree();
    if (!(pat = malloc(l + 1))) return 0;
    for (i = 0; i < l; i++) {
        if (p[i] == '#' && p[i + 1] == '?') {       /* AmigaOS's "any run": the star */
            pat[n++] = '*';
            i++;
        } else
            pat[n++] = p[i];
    }
    pat[n] = 0;
    return 1;
}

int os_patmatch(const char *name) { return pat && fnmatch(pat, name, 0) == 0; }

void os_patfree(void)
{
    free(pat);
    pat = NULL;
}

/* a command started in dir and left running (its input and output /dev/null): TRUE when it started */
int os_launch(const char *dir, const char *cmd)
{
    BPTR lk = Lock((STRPTR)dir, ACCESS_READ), old = 0;
    int ok;
    if (lk) old = CurrentDir(lk);
    ok = SystemTags((STRPTR)cmd, SYS_Asynch, TRUE, TAG_DONE) == 0;
    if (lk) {
        CurrentDir(old);
        UnLock(lk);
    }
    return ok;
}
