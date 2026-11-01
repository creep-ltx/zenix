/* SPDX-License-Identifier: GPL-2.0-only */
/* opendir, readdir, rewinddir, closedir, dirfd over SYS_readdir (DESIGN D27 section 5). The DIR and its entry come
 * from malloc (no buffers on the stack); rewinddir opens the directory again. */
#include <dirent.h>
#include <zenix.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "usys.h"

struct _zenix_dir { int fd; struct dirent ent; char path[ZPATH_MAX]; };

DIR *opendir(const char *path)
{
    struct ustat st;
    if (strlen(path) >= ZPATH_MAX) { errno = ENAMETOOLONG; return 0; }
    if (sys_stat(path, &st) < 0) return 0;
    if ((st.mode & S_IFMT) != S_IFDIR) { errno = ENOTDIR; return 0; }
    DIR *d = malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return 0; }
    if ((d->fd = sys_open(path, O_RDONLY)) < 0) { free(d); return 0; }
    strcpy(d->path, path);
    return d;
}
struct dirent *readdir(DIR *d)
{
    struct udirent u;
    if (sys_readdir(d->fd, &u) <= 0) return 0;          /* the end (errno untouched) or an error (errno set) */
    d->ent.d_ino = u.ino;
    d->ent.d_type = DT_UNKNOWN;
    memcpy(d->ent.d_name, u.name, sizeof d->ent.d_name);
    d->ent.d_name[sizeof d->ent.d_name - 1] = 0;
    return &d->ent;
}
void rewinddir(DIR *d)
{
    int fd = sys_open(d->path, O_RDONLY);
    if (fd < 0) return;
    sys_close(d->fd);
    d->fd = fd;
}
int closedir(DIR *d)
{
    int r = sys_close(d->fd);
    free(d);
    return r < 0 ? -1 : 0;
}
int dirfd(DIR *d) { return d->fd; }
