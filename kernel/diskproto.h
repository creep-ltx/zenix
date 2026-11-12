/* SPDX-License-Identifier: GPL-2.0-only */
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
#define DISK_INFOSZ 9  /* DISK_INFO with a2 = the caller's buffer size: at most that many bytes of struct diskinfo */
#define DISK_LISTSZ 10 /* DISK_LIST with a2 = the buffer size (the old two write the frozen first 80 bytes; their a2 was
                        * never set by callers, so it cannot carry a size) */
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
#define DISKINFO_FIRST 80      /* struct diskinfo's first size: what DISK_INFO / DISK_LIST write (frozen) */
#define DISKCAP_FLUSH  1       /* DISK_FLUSH really reaches the medium */
#define DISKCAP_FLASH  2       /* flash (CF/SD): no seek cost, align writes to erase; unset: rotating or unknown */
#define DISKCAP_RAM    4       /* memory: writes are durable only until reset */
#define DISKCAP_RO     8       /* DESIGN D44: a read-only medium (a CD): DISK_WRITE answers -EROFS */

/* DESIGN D44 (docs/d44-cd.md; frozen, grows only at the end): SYS_atapi's request, a packet command to the IDE port's
 * ATAPI CD drive, for the disk server only. Data comes from the drive (no command here sends data to it): of the bytes
 * it sends, the first SKIP are dropped and the next LEN go to BUF (LEN and SKIP even; BUF anywhere), the rest dropped.
 * Answers the bytes the drive sent, or -ENODEV (no CD drive), -EINVAL (odd), -EPERM, -EFAULT (the struct), -EIO (ERR
 * set at the end, the ATA error register in ERR - the sense key in its bits 7-4 -, or no answer in time or nonsense:
 * ERR 0, said on the log, and the drive given DEVICE RESET) */
struct uatapi {
    unsigned char cdb[12];     /* the packet (12 bytes: the drive's IDENTIFY PACKET DEVICE says so, checked at boot) */
    void *buf;
    unsigned len, skip;
    unsigned err;              /* back: the error register after a failed command, 0 otherwise */
};
#define UATAPI_FIRST   28      /* its first size */

/* DESIGN D46 (docs/d46-usb.md 3.5; frozen, grows only at the end): a USB stick through the USB server (/sbin/usbd, port
 * "usb"). usbd calls DISK_USBHELLO once it can serve (a0 its port): diskd forgets the USB disk it had (a mount of it
 * stays, answering -ESTALE), opens a window that the owner of port usb may post into, and answers at once. From then
 * on both ways go by posts (SYS_post, never a call that waits): diskd posts UR_READ records into usbd's window and
 * waits for the UR_DONE with that request's number, at most 5 s; usbd posts UR_ADD, UR_DEL and UR_DONE into diskd's.
 * Each window is a ring of UREC_N records; a record's seq counts from 1 after the hello, the slot is seq % UREC_N, and
 * a reader takes the records whose seq is past the last it took. The sectors are read into usbd's bounce buffer (its
 * address in UR_ADD) and copied out by diskd: usbd never writes into a client's memory. */
#define DISK_USBHELLO 11   /* (D46) a0 the port of usbd: root, the owner of port "usb" only -> 0 */
struct urec {
    unsigned kind;              /* UR_* */
    unsigned seq;               /* this record's number in its window */
    unsigned unit;              /* the USB disk: 0 (usb0) */
    unsigned lba, count;        /* UR_READ: sectors; UR_ADD: the size in 512-byte sectors and the most a request may ask */
    int result;            /* UR_DONE: 0, or -errno */
    unsigned a, b;              /* UR_DONE: a the request's seq; UR_ADD: a the bounce buffer, b its 32-byte label */
};
#define UREC_N   8
#define UR_READ  1
#define UR_DONE  16
#define UR_ADD   17
#define UR_DEL   18
