/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/diskproto.h"
#include "zenixfs_disk.h"

/* fsck.zenixfs DEVICE
 * Checks a ZenixFS volume: every checksum, the tree order, links and the free-space map. It only reports; a crash never needs it.
 * The volume is mounted read-only (ZENIXFS_MNT_READONLY): nothing is written, not even the orphan cleanup a mount does. */
static void say(void *ctx, const char *m) { (void)ctx; printf("  %s\n", m); }

int main(int argc, char **argv)
{
    if (argc != 2) { printf("usage: fsck.zenixfs device\n"); return 2; }
    if (zdisk_open(argv[1], DISK_RAW) < 0) { printf("fsck.zenixfs: cannot open %s (mounted, or no such device)\n", argv[1]); return 1; }
    struct zenixfs_dev d;
    struct zenixfs *fs = 0;
    for (unsigned bs = 10; bs <= 12 && !fs; bs++) {
        zdisk_dev(&d, bs);
        struct zenixfs_mount_opts mo = { 64, 0, ZENIXFS_MNT_READONLY, 0 };
        if (zenixfs_mount_opts(&fs, &d, &mo) != 0) fs = 0;
    }
    if (!fs) { printf("fsck.zenixfs: %s is not a ZenixFS volume, or it is too damaged to open\n", argv[1]); return 1; }
    struct zenixfs_stats st;
    zenixfs_get_stats(fs, &st);
    if (st.mount_damage)
        printf("fsck.zenixfs: %u commit record(s) were unreadable; checking the last good commit\n", st.mount_damage);
    int n = zenixfs_fsck(fs, say, 0);
    zenixfs_unmount(fs);
    if (n) { printf("fsck.zenixfs: %d problem(s) found\n", n); return 1; }
    printf("fsck.zenixfs: clean\n");
    return 0;
}
