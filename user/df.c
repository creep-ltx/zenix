/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* df [FILE...]: space on every mounted filesystem, the root first (the mounts as SYS_mountinfo lists them); with FILEs,
 * only the filesystem each one is on (the mount whose path is the longest leading part of its real path), as POSIX */
static const char USAGE[] = "usage: df [FILE...]\n";

/* the mount holding the real path p: the longest mount path that is p or a leading directory of it */
static int mount_of(const char *p)
{
    struct umountinfo mi;
    int best = -1;
    u32 bestlen = 0;
    for (u32 i = 0; sys_mountinfo(i, &mi) == 0; i++) {
        u32 n = strlen(mi.path);
        int root = n == 1 && mi.path[0] == '/';
        if (root ? (best < 0) : (!strncmp(p, mi.path, n) && (p[n] == 0 || p[n] == '/') && n >= bestlen)) {
            best = (int)i;
            bestlen = root ? 1 : n;
        }
    }
    return best;
}

static int row(u32 i)
{
    struct umountinfo mi;
    struct ustatfs f;
    if (sys_mountinfo(i, &mi) < 0)
        return 0;
    if (sys_statfs_mount(&f, i) < 0) {
        dprintf(1, "%-10s  cannot read the filesystem%26s%s\n", mi.dev[0] ? mi.dev : "-", "", mi.path);
        return 0;
    }
    u32 kb = f.bsize / 1024 ? f.bsize / 1024 : 1;
    u32 total = f.blocks * kb, free = f.bfree * kb, used = total - free;
    dprintf(1, "%-10s  %9u %8u %9u %3u%%  %6u(%u)  %s\n", mi.dev[0] ? mi.dev : "-", total, used, free,
           total ? (u32)(((unsigned long long)used * 100 + total - 1) / total) : 0, f.files, f.ffree, mi.path);
    return 1;
}

int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "--"))
        first = 2;
    else if (argc > 1 && argv[1][0] == '-' && argv[1][1])
        zbadopt(argv[1], USAGE);
    dprintf(1, "Filesystem  1K-blocks     Used Available Use%%  Inodes(free)  Mounted on\n");
    int shown = 0, bad = 0;
    if (first >= argc) {
        struct umountinfo mi;
        for (u32 i = 0; sys_mountinfo(i, &mi) == 0; i++)
            shown += row(i);
        return shown ? 0 : 1;
    }
    for (int a = first; a < argc; a++) {
        char full[ZPATH_MAX];
        if (zrealpath(argv[a], full, sizeof full) < 0) {
            dprintf(2, "df: %s: %s\n", argv[a], zwhy("cannot resolve"));
            bad = 1;
            continue;
        }
        int m = mount_of(full);
        if (m < 0 || !row((u32)m))
            bad = 1;
    }
    return bad;
}
