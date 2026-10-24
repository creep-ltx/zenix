/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* df: space on every mounted filesystem, the root first (the mounts as SYS_mountinfo lists them) */
int main(void)
{
    struct umountinfo mi;
    struct ustatfs f;
    int shown = 0;
    printf("Filesystem  1K-blocks     Used Available Use%%  Inodes(free)  Mounted on\n");
    for (u32 i = 0; sys_mountinfo(i, &mi) == 0; i++) {
        if (sys_statfs_mount(&f, i) < 0) {
            printf("%-10s  cannot read the filesystem%26s%s\n", mi.dev[0] ? mi.dev : "-", "", mi.path);
            continue;
        }
        u32 kb = f.bsize / 1024 ? f.bsize / 1024 : 1;
        u32 total = f.blocks * kb, free = f.bfree * kb, used = total - free;
        printf("%-10s  %9u %8u %9u %3u%%  %6u(%u)  %s\n", mi.dev[0] ? mi.dev : "-", total, used, free,
               total ? (u32)(((unsigned long long)used * 100 + total - 1) / total) : 0, f.files, f.ffree, mi.path);
        shown++;
    }
    return shown ? 0 : 1;
}
