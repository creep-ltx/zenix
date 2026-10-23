/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* df: space on the root filesystem */
int main(void)
{
    struct ustatfs f;
    if (sys_statfs(&f) < 0) { printf("df: cannot read the filesystem\n"); return 1; }
    u32 kb = f.bsize / 1024 ? f.bsize / 1024 : 1;
    u32 total = f.blocks * kb, free = f.bfree * kb, used = total - free;
    printf("Filesystem  1K-blocks     Used Available Use%%  Inodes(free)  Mounted on\n");
    printf("/           %9u %8u %9u %3u%%  %6u(%u)  /\n", total, used, free,
           total ? (u32)(((unsigned long long)used * 100 + total - 1) / total) : 0, f.files, f.ffree);
    return 0;
}
