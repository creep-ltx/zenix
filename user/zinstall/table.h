/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The installer's partition tables (DESIGN D35, docs/d35-installer.md): what is on a disk, what may be done with it, the
 * exact list of sector writes that does it. No system calls: every sector goes through struct tio, so the PC's tests
 * and fuzzer run the same code on image files. Sectors on the disk are bytes; RDB blocks are turned into longs here, so
 * the code is the same on either endianness. */
typedef unsigned int t_u32;
typedef unsigned char t_u8;

#define T_MAXPART 40
#define T_MAXREGION 5                   /* a top-level RDB, or one in each 0x76 partition of an MBR */
#define T_MAXW 24                       /* a partition step writes every kept PART block anew: 20 kept and 3 new, and the RDSK block */
#define T_MAXOPT 48
#define T_MINSECT (28u * 2048)          /* the least a Zenix can be put on */

struct tio {
    void *ctx;
    int (*rd)(void *ctx, t_u32 lba, t_u32 n, void *buf);        /* 0, or an error */
    int (*wr)(void *ctx, t_u32 lba, t_u32 n, const void *buf);
    int (*fl)(void *ctx);                                       /* everything written is durable: 0, or an error */
};

enum t_kind {
    TD_BLANK,                           /* nothing found: zeros where a table or a filesystem would be */
    TD_RDB, TD_MBR,
    TD_BARE,                            /* a filesystem with no table */
    TD_UNKNOWN,                         /* data, no table we can read (also an MBR that lies outside the disk) */
    TD_GPT, TD_HYBRID                   /* never written */
};

enum { TP_RDB, TP_MBR };

struct t_part {
    char dev[10];                       /* the name the disk server gives it: ide0p3 */
    char label[32];                     /* its RDB name; "" for an MBR entry */
    char vol[32];                       /* an AmigaDOS volume's name, read from its root block; else "" */
    t_u32 start, sectors;               /* on the whole disk; sectors 0: its geometry could not be understood */
    t_u32 dostype;                      /* RDB */
    t_u32 blk;                          /* its PART block, on the whole disk */
    t_u32 lowcyl, highcyl;              /* RDB: its own cylinders, as HDToolBox shows them */
    t_u8 how, mbrtype, slot;            /* TP_*; MBR: type byte, slot 0-3 */
    t_u8 bootable, nomount, busy;       /* busy: something mounted overlaps it */
    signed char pri;                    /* RDB: boot priority */
    signed char region;                 /* the RDB it is in, -1 for an MBR entry */
    signed char holds;                  /* an MBR 0x76 entry: the region inside it, -1 if none */
    t_u8 del;                           /* marked to be deleted (the partition step): free space for the plan, gone from the table */
};

struct t_region {                       /* one RDB */
    t_u32 base, limit;                  /* where it starts on the disk, how many sectors it may use */
    t_u32 blk, cyl, heads, secs, locyl, hicyl;      /* (blk relative to base, as in rdb.h) */
    const char *noedit;                 /* 0, or why this table must not be changed */
};

struct t_busy { t_u32 start, sectors; };

struct tdisk {
    enum t_kind kind;
    t_u32 sectors;
    const char *what;                   /* in words, what kind says: "an AmigaDOS volume, no partition table" ... */
    const char *noedit;                 /* the table (of an MBR, or the top RDB) must not be changed: why */
    int nparts, nregions, nslotsused;
    int hasfat, anybusy, full;          /* FAT partition on it; mounted device on it; more partitions than T_MAXPART */
    int oldrdb;                         /* the old top RDB could be walked whole (whole-disk: which blocks are free) */
    int zb, zr, zh;                     /* a Zenix an earlier zinstall made: its ZBn, ZRn, ZHn (same n, one Amiga table); -1 none */
    t_u8 sec0[512];
    t_u8 digest[32];                    /* SHA-256 over every sector read to make all this, and the size */
    struct t_part part[T_MAXPART];
    struct t_region region[T_MAXREGION];
};

enum t_optkind { TO_NEWCONT, TO_FREE, TO_PART, TO_WHOLE, TO_REINSTALL, TO_REMOVE };
struct t_opt {
    enum t_optkind kind;
    int region, part;                   /* TO_FREE: the RDB; TO_PART: the partition */
    t_u32 start, sectors;               /* the space (sectors on the whole disk) */
};

struct t_write {
    t_u32 lba;
    int flush;                          /* flush after this one */
    t_u8 data[512], old[512];           /* what to write; what must be there now */
};

enum { R_BOOT, R_ROOT, R_HOME };
/* how Zenix's share is laid out (DESIGN D35 section 3): the boot partition is always the rule's */
enum { TL_RECOMMENDED, TL_ONE, TL_CUSTOM };
struct t_want {
    int layout;                         /* TL_*: recommended (a root sized for the space, /home the rest), one (the root only), custom */
    t_u32 root, home;                   /* TL_CUSTOM: sectors; home 0: no /home partition */
    int rootfs, homefs;                 /* 1: ZenixFS, 0: ext2 */
};
#define T_MINROOT (32u * 2048)          /* a custom root's least */
#define T_MINHOME (4u * 2048)
#define T_HOME_REST 0xFFFFFFFFu         /* TL_CUSTOM's home: all that is left */
struct t_newp {
    char label[8];
    t_u32 lo, hi;                       /* cylinders of its RDB */
    t_u32 start, sectors;               /* on the whole disk */
    t_u32 dostype, blk;                 /* blk on the whole disk */
};
struct t_plan {
    struct t_opt opt;
    int nnew, zenixfs, homefs;
    struct t_newp nw[3];                /* R_BOOT, R_ROOT, and R_HOME if nnew is 3 */
    int atomic;                         /* 0: the plan's text says the old table can be damaged before the new one is whole */
    int pri;                            /* the boot partition's boot priority */
    char aos[32];                       /* the AmigaOS partition for the boot menu; "" when none */
    t_u32 cyl;
    int keep;                           /* a chosen RDB partition split: what Zenix does not take stays a partition of the old name */
    struct t_newp rest;                 /* (label: the old name; lo, hi in the old partition's own cylinders; dostype the old one) */
    int nw1, nw2, commit1;              /* writes of stage 1 (nw1, the one that makes the new table real: commit1), of stage 2 */
    struct t_write w1[T_MAXW], w2[5];
    int erase_home;                     /* TO_REINSTALL: /home made anew too */
};

/* Read the disk. busy: the sectors of mounted devices on this medium. 0 (anything found, see d->kind), -1 a read failed */
int t_read(struct tdisk *d, const struct tio *io, t_u32 sectors, const struct t_busy *busy, int nbusy);
/* Why partition i may not be chosen; 0 if it may */
const char *t_part_refused(const struct tdisk *d, int i);
/* Why partition i may not be deleted; 0 if it may (a FAT entry, a 0x76 as a whole, a mounted one, a damaged table: never) */
const char *t_del_refused(const struct tdisk *d, int i);
int t_ndel(const struct tdisk *d);      /* how many are marked */
/* What may be done with this disk. With partitions marked to delete: only the free space they leave, in their own table. Returns how many options; each is a real possibility of at least T_MINSECT.
 * Free space first (the default), partitions next, the whole disk last. */
int t_options(const struct tdisk *d, const struct tio *io, struct t_opt *o, int max);
/* Why the whole disk may not be erased; 0 if it may */
const char *t_whole_refused(const struct tdisk *d);
/* Make the plan: 0, or -1 with *why. maxsect: use at most this many sectors (0: as many as the rules say). For a chosen RDB partition
 * less than all of it splits it: Zenix first, the rest one partition with the old name, dostype and environment, empty for its system */
int t_plan_make(const struct tdisk *d, const struct tio *io, const struct t_opt *o, t_u32 maxsect, const struct t_want *w,
                struct t_plan *p, const char **why);
/* The tables read again: 1 exactly as in d (the digest, the size), 0 changed, -1 cannot read */
int t_same(const struct tdisk *d, const struct tio *io, t_u32 sectors, const struct t_busy *busy, int nbusy);
/* Do the writes: each checked against what must be there, written, read back, flushed where marked.
 * 0, or -1 with *why (the writes before it are done). */
int t_commit(const struct tio *io, struct t_write *w, int n, const char **why);
/* The Zenix already on the disk (d->zr >= 0): made again in its own partitions (the root, and /home with erase_home), its partitions
 * unbootable and unmounted for the time it takes, as a new install's; or taken out of the table (one write), its space free again */
int t_plan_reinstall(const struct tdisk *d, const struct tio *io, const struct t_want *w, int erase_home, struct t_plan *p, const char **why);
int t_plan_remove(const struct tdisk *d, const struct tio *io, struct t_plan *p, const char **why);

const char *t_typename(const struct t_part *p);   /* in words: "AmigaDOS FFS", "PFS3", "Linux (ext2)", "FAT32" ... */
