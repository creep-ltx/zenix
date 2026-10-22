#pragma once
/* Messages to the disk server ("disk" port). Devices are named: "ide0" is the
 * whole disk, "ide0p1", "ide0p2"... are the partitions of its RDB. A handle
 * from DISK_OPEN is a small number; reads and writes through a partition
 * handle are confined to that partition (LBAs are partition-relative). */
#define DISK_OPEN   1  /* a0 name, a1 flags (DISK_RAW or DISK_MOUNT) -> handle, or -1 */
#define DISK_RAW    1  /*   for tools: refused while the device is mounted */
#define DISK_MOUNT  2  /*   for a filesystem server: the caller becomes the only writer */
#define DISK_UNMOUNT 7 /* a0 handle: release a mount */
#define DISK_READ   2  /* a0 handle, a1 lba, a2 sector count, a3 buffer -> 0 or -1 */
#define DISK_WRITE  3  /* same */
#define DISK_INFO   4  /* a0 handle, a1 struct diskinfo* -> 0 */
#define DISK_LIST   5  /* a0 index, a1 struct diskinfo* -> 0, -1 past the end */
#define DISK_RESCAN 6  /* re-read the partition table (after fdisk wrote it) */
#define DISK_FLUSH  8  /* a0 handle: everything written so far is durable when this returns (ATA FLUSH CACHE); needed for crash safety */
struct diskinfo {
    char name[16];             /* "ide0", "ide0p1" */
    char label[32];            /* RDB drive name (DH0...) or "" */
    unsigned sectors;          /* 512-byte sectors */
    unsigned start;            /* first sector on the whole disk (0 for the disk itself) */
    unsigned dostype;          /* RDB DosType, 0 for the whole disk */
    unsigned flags;            /* 1 = bootable */
    unsigned mounted;          /* pid of the filesystem server that has it, 0 if free */
    unsigned caps;             /* DISKCAP_* bits */
    unsigned maxsect;          /* most sectors one read/write call may carry */
    unsigned erase;            /* flash erase/page size in sectors, 0 if unknown or rotating */
};
#define DISKCAP_FLUSH  1       /* DISK_FLUSH really reaches the medium */
#define DISKCAP_FLASH  2       /* flash (CF/SD): no seek cost, align writes to erase; unset: rotating or unknown */
#define DISKCAP_RAM    4       /* memory: writes are durable only until reset */
