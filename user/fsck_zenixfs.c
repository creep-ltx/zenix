/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/diskproto.h"
#include "zenixfs_disk.h"

/* fsck.zenixfs [-y|--old] DEVICE
 * Checks a ZenixFS volume: every checksum, the tree order, links and the free-space map. A crash never needs it. Without
 * -y the volume is mounted read-only (ZENIXFS_MNT_READONLY): nothing is written, not even the orphan cleanup a mount does.
 * -y repairs what can be repaired without guessing (zenixfs_repair, docs/zenixfs.md 0b): a destroyed superblock, the
 * free-space map, damaged inner tree nodes, a damaged newest commit. It refuses the rest and then changes nothing. --old takes
 * the newest state that can be read and builds on it, on the owner's word: its older files' data may have been overwritten. */
static void say(void *ctx, const char *m) { (void)ctx; dprintf(1, "  %s\n", m); }

/* the block size of the volume: from its superblock, or (superblock gone) where the commit records check out; 0 if neither */
static unsigned find_bs(struct zenixfs_dev *d, int *nosuper)
{
    unsigned best = 0;
    int most = 0;
    for (unsigned bs = 10; bs <= 12; bs++) {             /* (a mount that fails is not a missing superblock) */
        zdisk_dev(d, bs);
        if (zenixfs_super_ok(d)) { *nosuper = 0; return bs; }
        int n = zenixfs_count_commits(d);
        if (n > most) { most = n; best = bs; }
    }
    *nosuper = 1;
    return best;
}

static int repair(const char *dev, uint32_t want)
{
    struct zenixfs_dev d;
    int nosuper;
    unsigned bs = find_bs(&d, &nosuper);
    if (!bs) { dprintf(1, "fsck.zenixfs: nothing on %s looks like ZenixFS\n", dev); return 1; }
    zdisk_dev(&d, bs);
    if (nosuper) {
        if (zenixfs_rewrite_super(&d, "recovered", 0, ZENIXFS_CHARSET_LATIN1, (uint64_t)sys_time())) { dprintf(1, "fsck.zenixfs: cannot rewrite the superblock from this partition's size: nothing changed (zenixfs-tool salvage on a PC)\n"); return 1; }
        dprintf(1, "fsck.zenixfs: superblock rewritten (block size %u)\n", 1u << bs);
    }
    struct zenixfs_repair_report r;
    int n = zenixfs_repair(&d, 64, want, say, 0, &r);
    if (n < 0) {
        dprintf(1, "fsck.zenixfs: the tree is damaged in a way this cannot undo, or the state is too old to build on: nothing %schanged. Copy out what can be read, or use zenixfs-tool salvage on a PC (man rescue)\n", nosuper ? "more " : "");
        return 1;
    }
    if (r.nodes) dprintf(1, "fsck.zenixfs: damaged inner tree nodes replaced: %u new nodes over %u leaves; nothing lost\n", r.nodes, r.leaves);
    if (r.back) dprintf(1, "fsck.zenixfs: %u commit(s) back: the newest was damaged, its changes are gone; older file data is not checked, look at your newest files\n", r.back);
    dprintf(1, "fsck.zenixfs: free-space map rebuilt (%u blocks changed)\n", r.changed);
    if (n) { dprintf(1, "fsck.zenixfs: %d problem(s) remain\n", n); return 1; }
    dprintf(1, "fsck.zenixfs: repaired, clean\n");
    return 0;
}

static const char USAGE[] = "usage: fsck.zenixfs [-y|--old] device\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int y = argc == 3 && !strcmp(argv[1], "-y"), old = argc == 3 && !strcmp(argv[1], "--old");
    const char *dev = argv[argc - 1];
    if (argc != 2 && !y && !old) zusage(USAGE);
    if (argc == 2 && dev[0] == '-') zbadopt(dev, USAGE);
    if (zdisk_open(dev, DISK_RAW) < 0) { dprintf(1, "fsck.zenixfs: cannot open %s (mounted, or no such device)\n", dev); return 1; }
    if (y || old) return repair(dev, old ? 0 : ZENIXFS_REPAIR_AUTO);
    struct zenixfs_dev d;
    struct zenixfs *fs = 0;
    for (unsigned bs = 10; bs <= 12 && !fs; bs++) {
        zdisk_dev(&d, bs);
        struct zenixfs_mount_opts mo = { 64, 0, ZENIXFS_MNT_READONLY, 0 };
        if (zenixfs_mount_opts(&fs, &d, &mo) != 0) fs = 0;
    }
    if (!fs) { dprintf(1, "fsck.zenixfs: %s is not a ZenixFS volume, or it is too damaged to open (fsck.zenixfs -y)\n", dev); return 1; }
    struct zenixfs_stats st;
    zenixfs_get_stats(fs, &st);
    if (st.mount_damage)
        dprintf(1, "fsck.zenixfs: %u commit record(s) or roots were unreadable; checking the last good commit\n", st.mount_damage);
    int n = zenixfs_fsck(fs, say, 0);
    zenixfs_unmount(fs);
    if (n) { dprintf(1, "fsck.zenixfs: %d problem(s) found (fsck.zenixfs -y repairs what it can)\n", n); return 1; }
    if (st.mount_old) { dprintf(1, "fsck.zenixfs: only an older state can be read: fsck.zenixfs -y, or --old to build on it\n"); return 1; }
    dprintf(1, "fsck.zenixfs: clean\n");
    return 0;
}
