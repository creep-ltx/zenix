/* SPDX-License-Identifier: GPL-2.0-only */
/* utimensat and realpath (made for make, DESIGN D52 section 4): in a file of their own, not in sys.c, because realpath
 * calls malloc and an archive member is taken whole when any of its symbols is wanted: with them in sys.c every program
 * (read and write live there) would link the allocator and the environment (+250 bytes of file and 1.3 KB of data and
 * BSS in every process: tools/libc-size.sh's budgets). */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zenix.h>
#include "usys.h"

#define FS_UTIME_NOW ZX_FS_UTIME_NOW      /* SYS_utimes: the server takes its clock (kernel/fsproto.h, via zenix-consts.h) */
static int ret(int r) { return r < 0 ? -1 : r; }        /* errno is already set */

/* utimensat over SYS_utimes: Zenix has one time per file (the modification's, whole seconds), so the access time
 * is ignored and the nanoseconds are dropped. Only AT_FDCWD or an absolute path, and no flags:
 * Zenix's SYS_utimes follows a symbolic link. UTIME_NOW sets the server's clock (anyone who may write the file);
 * a given time only the owner and root may set (the server says EPERM). UTIME_OMIT on the modification time does
 * nothing, as POSIX says. times == 0 is both UTIME_NOW. */
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    if (flags) { errno = EINVAL; return -1; }
    if (dirfd != AT_FDCWD && path[0] != '/') { errno = ENOSYS; return -1; }      /* an absolute path ignores dirfd */
    if (!times) return ret(sys_utimes(path, FS_UTIME_NOW));
    for (int i = 0; i < 2; i++)
        if (times[i].tv_nsec != UTIME_NOW && times[i].tv_nsec != UTIME_OMIT &&
            (times[i].tv_nsec < 0 || times[i].tv_nsec >= 1000000000L)) { errno = EINVAL; return -1; }
    if (times[1].tv_nsec == UTIME_OMIT) {                    /* nothing to set: the file must still exist */
        struct ustat st;
        return ret(sys_stat(path, &st));
    }
    if (times[1].tv_nsec == UTIME_NOW) return ret(sys_utimes(path, FS_UTIME_NOW));
    if (times[1].tv_sec < 0 || (u32)times[1].tv_sec >= FS_UTIME_NOW) { errno = EINVAL; return -1; }
    return ret(sys_utimes(path, (u32)times[1].tv_sec));
}
/* realpath over libz's resolver (zrealpath: . and .. and links, paths of at most 255 bytes), and the name must exist
 * (POSIX: ENOENT otherwise; zrealpath alone does not look at the last part). The caller's buffer needs PATH_MAX (256)
 * bytes; resolved == 0 mallocs one. A path or link text zrealpath cannot take is ENAMETOOLONG. */
char *realpath(const char *path, char *resolved)
{
    struct ustat st;
    char tmp[ZPATH_MAX];
    if (!path) { errno = EINVAL; return 0; }
    if (sys_stat(path, &st) < 0) return 0;                  /* errno from the system call: ENOENT, ENOTDIR, EACCES */
    if (zrealpath(path, tmp, sizeof tmp) < 0) { errno = ENAMETOOLONG; return 0; }
    if (!resolved && !(resolved = malloc(ZPATH_MAX))) { errno = ENOMEM; return 0; }
    strcpy(resolved, tmp);
    return resolved;
}
