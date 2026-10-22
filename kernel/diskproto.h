#pragma once
/* Messages to the disk server ("disk" port). Devices are named: "ide0" is the
 * whole disk, "ide0p1", "ide0p2"... are the partitions of its RDB. A handle
 * from DISK_OPEN is a small number; reads and writes through a partition
 * handle are confined to that partition (LBAs are partition-relative). */
#define DISK_OPEN   1  /* a0 name -> handle, or -1 */
#define DISK_READ   2  /* a0 handle, a1 lba, a2 sector count, a3 buffer -> 0 or -1 */
#define DISK_WRITE  3  /* same */
#define DISK_INFO   4  /* a0 handle, a1 struct diskinfo* -> 0 */
#define DISK_LIST   5  /* a0 index, a1 struct diskinfo* -> 0, -1 past the end */
#define DISK_RESCAN 6  /* re-read the partition table (after fdisk wrote it) */
struct diskinfo {
    char name[16];             /* "ide0", "ide0p1" */
    char label[32];            /* RDB drive name (DH0...) or "" */
    unsigned sectors;          /* 512-byte sectors */
    unsigned start;            /* first sector on the whole disk (0 for the disk itself) */
    unsigned dostype;          /* RDB DosType, 0 for the whole disk */
    unsigned flags;            /* 1 = bootable */
};
