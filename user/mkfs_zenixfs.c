/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "rdb.h"
#include "../kernel/diskproto.h"
#include "zenixfs_disk.h"

/* mkfs.zenixfs [-b 10|11|12] [-l LABEL] [-c latin1|utf8] DEVICE
 * Makes a ZenixFS filesystem (docs/zenixfs-format.md). The block size is 1 << b bytes; by default 1 KB under 16 MB,
 * 2 KB under 256 MB, else 4 KB. The device must not be mounted. */
int main(int argc, char **argv)
{
    const char *dev = 0, *label = 0;
    unsigned bs = 0, cs = ZENIXFS_CHARSET_LATIN1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) bs = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cs = !strcmp(argv[++i], "utf8") ? ZENIXFS_CHARSET_UTF8 : ZENIXFS_CHARSET_LATIN1;
        else dev = argv[i];
    }
    if (!dev || (bs && (bs < 10 || bs > 12))) {
        printf("usage: mkfs.zenixfs [-b 10|11|12] [-l label] [-c latin1|utf8] device\n");
        return 2;
    }
    if (zdisk_open(dev, DISK_RAW) < 0) { printf("mkfs.zenixfs: cannot open %s (mounted, or no such device)\n", dev); return 1; }
    unsigned mb = zdisk_sectors() / 2048;
    if (!bs) bs = mb < 16 ? 10 : mb < 256 ? 11 : 12;
    if (!strcmp(dev, "ide0") && zdisk_dostype() == 0)
        printf("mkfs.zenixfs: warning: formatting the whole disk, not a partition\n");
    struct zenixfs_dev d;
    zdisk_dev(&d, bs);
    int r = zenixfs_mkfs(&d, label, 0, (uint64_t)sys_time());
    if (r) { printf("mkfs.zenixfs: failed (%d); the device may be too small\n", r); return 1; }
    {   /* a volume id (DESIGN D11 4.3): the clock, the tick and the E-clock, mixed */
        unsigned char u[16];
        u32 x = (u32)sys_time() * 2654435761u ^ sys_ticks();
        for (int i = 0; i < 16; i++) { x += sys_hrtime(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; u[i] = (unsigned char)(x >> 24); }
        if (zenixfs_set_uuid(&d, u)) { printf("mkfs.zenixfs: cannot write the volume id\n"); return 1; }
    }
    if (cs != ZENIXFS_CHARSET_LATIN1) {                  /* the charset hint lives in the superblock, set it through a mount */
        struct zenixfs *fs;
        if (zenixfs_mount(&fs, &d, 32) == 0) { zenixfs_set_charset(fs, cs); zenixfs_unmount(fs); }
    }
    printf("mkfs.zenixfs: %s: %u blocks of %u bytes\n", dev, d.nblocks, 1u << bs);
    return 0;
}
