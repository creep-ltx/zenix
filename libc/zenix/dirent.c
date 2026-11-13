/* SPDX-License-Identifier: GPL-2.0-only */
/* opendir, readdir, rewinddir, closedir, dirfd (DESIGN D27 section 5) over libz's zdir (DESIGN D49: SYS_readdirs, the
 * records in batches; SYS_readdir one by one where the kernel or the server lacks it). The DIR and its entry come from
 * malloc (no buffers on the stack); rewinddir opens the directory again. d_type from the record's type, or from its
 * stat; DT_UNKNOWN when neither says. */
#include <dirent.h>
#include <zenix.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "usys.h"

struct _zenix_dir { struct zdir *z; struct dirent ent; char path[ZPATH_MAX]; };

DIR *opendir(const char *path)
{
    struct ustat st;
    if (strlen(path) >= ZPATH_MAX) { errno = ENAMETOOLONG; return 0; }
    if (sys_stat(path, &st) < 0) return 0;
    if ((st.mode & S_IFMT) != S_IFDIR) { errno = ENOTDIR; return 0; }
    DIR *d = malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return 0; }
    if (!(d->z = zdir_open(path))) { free(d); return 0; }
    strcpy(d->path, path);
    return d;
}
struct dirent *readdir(DIR *d)
{
    static const unsigned char dt[8] = { DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK };
    const struct udirx *x = zdir_next(d->z);
    if (!x) {                                           /* the end (errno untouched) or an error (errno set) */
        if (zdir_err(d->z)) errno = zdir_err(d->z);
        return 0;
    }
    d->ent.d_ino = x->ino;
    d->ent.d_type = x->type < 8 ? dt[x->type] : DT_UNKNOWN;
    if (d->ent.d_type == DT_UNKNOWN && (x->flags & UDX_STAT))
        d->ent.d_type = (unsigned char)((x->st.mode >> 12) & 15);   /* DT_* are the mode's type bits */
    memcpy(d->ent.d_name, x->name, strlen(x->name) + 1);   /* (1-63 bytes and its 0: zdir checked it) */
    return &d->ent;
}
void rewinddir(DIR *d)
{
    struct zdir *z = zdir_open(d->path);
    if (!z) return;
    zdir_close(d->z);
    d->z = z;
}
int closedir(DIR *d)
{
    int r = sys_close(zdir_fd(d->z));
    free(d->z);
    free(d);
    return r < 0 ? -1 : 0;
}
int dirfd(DIR *d) { return zdir_fd(d->z); }
