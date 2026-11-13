/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/fsproto.h"
/* touch [-c] file... : create files, or set their modification time to now (FS_UTIME_NOW: the server takes its clock,
 * and "now" is allowed to anyone who may write the file, a given time only to the owner and root) */
int main(int argc, char **argv)
{
    int no_create = 0, i = 1, rc = 0;
    if (i < argc && !strcmp(argv[i], "-c")) { no_create = 1; i++; }
    for (; i < argc; i++) {
        struct ustat st;
        if (sys_stat(argv[i], &st) == 0) {
            if (sys_utimes(argv[i], FS_UTIME_NOW) < 0) { dprintf(2, "touch: %s: %s\n", argv[i], zwhy("cannot set the time")); rc = 1; }
            continue;
        }
        if (no_create) continue;
        int fd = sys_open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) { dprintf(2, "touch: %s: %s\n", argv[i], zwhy("cannot create")); rc = 1; } else {
            sys_close(fd);
            if (sys_utimes(argv[i], FS_UTIME_NOW) < 0) { dprintf(2, "touch: %s: %s\n", argv[i], zwhy("cannot set the time")); rc = 1; }
        }
    }
    return rc;
}
