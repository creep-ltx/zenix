/* SPDX-License-Identifier: GPL-2.0-only */
/* ZenixFS library: format in docs/zenixfs-format.md, plan in docs/zenixfs.md.
 * Portable C: needs only <stdint.h>, <stddef.h>, memcpy/memset/memcmp and malloc/free (ZENIXFS_MALLOC/ZENIXFS_FREE).
 * The same source runs on the host (tests, tools) and later inside the Zenix filesystem server. */
#ifndef ZENIXFS_H
#define ZENIXFS_H
#include <stdint.h>
#include <stddef.h>


/* Block device the filesystem sits on. Block numbers are filesystem blocks (1 << bshift bytes). */
struct zenixfs_dev {
    void *ctx;
    int (*read)(void *ctx, uint32_t blk, void *buf);          /* 0 or -1 */
    int (*write)(void *ctx, uint32_t blk, const void *buf);   /* 0 or -1 */
    int (*flush)(void *ctx);                                   /* barrier: all earlier writes are durable */
    uint32_t bshift;
    uint32_t nblocks;
};

#define ZENIXFS_ROOT_INO 1
#define ZENIXFS_NAME_MAX 255
#define ZENIXFS_INLINE_MAX 192

/* errors (negative return values) */
#define ZENIXFS_EIO      (-5)
#define ZENIXFS_ENOENT   (-2)
#define ZENIXFS_EEXIST   (-17)
#define ZENIXFS_ENOTDIR  (-20)
#define ZENIXFS_EISDIR   (-21)
#define ZENIXFS_EINVAL   (-22)
#define ZENIXFS_ENOSPC   (-28)
#define ZENIXFS_ENOTEMPTY (-39)
#define ZENIXFS_ENAMETOOLONG (-36)
#define ZENIXFS_EFBIG    (-27)
#define ZENIXFS_ECORRUPT (-117)   /* checksum or structure error found */
#define ZENIXFS_EROFS    (-30)    /* mounted read-only */

/* Unix mode type bits */
#define ZENIXFS_S_IFMT  0170000
#define ZENIXFS_S_IFDIR 0040000
#define ZENIXFS_S_IFREG 0100000
#define ZENIXFS_S_IFLNK 0120000

struct zenixfs_inode {
    uint32_t mode, uid, gid, nlink;
    uint64_t size, mtime, ctime;
    uint32_t flags, parent, rdev;
};

struct zenixfs_statfs { uint32_t bsize, blocks, bfree, files; };

struct zenixfs;   /* a mounted volume */

/* Names are opaque bytes; the volume records which encoding its creator used, as a hint for the operating system
 * (convert at the boundary when it differs from the system's own). The filesystem never changes or checks names. */
#define ZENIXFS_CHARSET_LATIN1 0   /* the default, and what an unset field means */
#define ZENIXFS_CHARSET_UTF8   1

/* make a filesystem on dev; label may be NULL. reserve = blocks kept for unlink (0 = default 2%). */
int  zenixfs_mkfs(struct zenixfs_dev *dev, const char *label, uint32_t reserve, uint64_t now);
/* mount; cache_blocks = size of the block cache in blocks (at least 32). */
int  zenixfs_mount(struct zenixfs **out, struct zenixfs_dev *dev, uint32_t cache_blocks);
/* like zenixfs_mount; bm_pages = how many pages of the free-space map are kept in RAM (0 = 8; one page covers 8 * (blocksize - 32) blocks) */
int  zenixfs_mount_ex(struct zenixfs **out, struct zenixfs_dev *dev, uint32_t cache_blocks, uint32_t bm_pages);
/* mount with options, for recovery: flags ZENIXFS_MNT_NOBITMAPCHECK (do not insist on a good free-space map at mount),
 * ZENIXFS_MNT_READONLYISH (do not clean up orphans at mount); skip_commits = ignore that many of the newest commit records
 * (mount an older state of the volume). */
#define ZENIXFS_MNT_NOBITMAPCHECK 1
#define ZENIXFS_MNT_READONLYISH   2
#define ZENIXFS_MNT_READONLY      4   /* nothing is ever written (no orphan cleanup, no commit; changes fail with EROFS): for fsck */
int  zenixfs_is_readonly(struct zenixfs *fs);
struct zenixfs_mount_opts { uint32_t cache_blocks, bm_pages, flags, skip_commits; };
int  zenixfs_mount_opts(struct zenixfs **out, struct zenixfs_dev *dev, const struct zenixfs_mount_opts *o);
/* optional, after the mount: a read of n neighbouring blocks in one request (0 or -1); zenixfs_read then reads runs of
 * whole blocks straight into the caller's buffer (docs/d42-faster-ide.md) */
void zenixfs_set_readn(struct zenixfs *fs, int (*readn)(void *ctx, uint32_t blk, uint32_t n, void *buf));
int  zenixfs_unmount(struct zenixfs *fs);                    /* commits */
int  zenixfs_sync(struct zenixfs *fs);                       /* commit everything now */
void zenixfs_set_time(struct zenixfs *fs, uint64_t now);
int  zenixfs_statfs(struct zenixfs *fs, struct zenixfs_statfs *st);
uint32_t zenixfs_charset(struct zenixfs *fs);
int  zenixfs_set_charset(struct zenixfs *fs, uint32_t charset);     /* rewrites the superblock (one block) */

int  zenixfs_lookup(struct zenixfs *fs, uint32_t dir, const char *name, int len, uint32_t *ino);
int  zenixfs_getattr(struct zenixfs *fs, uint32_t ino, struct zenixfs_inode *out);
int  zenixfs_setattr(struct zenixfs *fs, uint32_t ino, const struct zenixfs_inode *in);   /* mode uid gid mtime flags(amiga bits) */
int  zenixfs_create(struct zenixfs *fs, uint32_t dir, const char *name, int len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t *ino);
int  zenixfs_mkdir(struct zenixfs *fs, uint32_t dir, const char *name, int len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t *ino);
int  zenixfs_symlink(struct zenixfs *fs, uint32_t dir, const char *name, int len, const char *target, uint32_t uid, uint32_t gid, uint32_t *ino);
int  zenixfs_readlink(struct zenixfs *fs, uint32_t ino, char *buf, int max);           /* length */
int  zenixfs_link(struct zenixfs *fs, uint32_t dir, const char *name, int len, uint32_t ino);
int  zenixfs_unlink(struct zenixfs *fs, uint32_t dir, const char *name, int len);
int  zenixfs_rmdir(struct zenixfs *fs, uint32_t dir, const char *name, int len);
int  zenixfs_rename(struct zenixfs *fs, uint32_t odir, const char *oname, int olen, uint32_t ndir, const char *nname, int nlen);
/* readdir: cookie 0 starts; returns 1 with an entry, 0 at the end. "." and ".." are returned first. */
int  zenixfs_readdir(struct zenixfs *fs, uint32_t dir, uint64_t *cookie, char *name, uint32_t *ino, uint8_t *type);
int  zenixfs_read(struct zenixfs *fs, uint32_t ino, uint64_t off, void *buf, uint32_t len);    /* bytes read */
int  zenixfs_write(struct zenixfs *fs, uint32_t ino, uint64_t off, const void *buf, uint32_t len);
int  zenixfs_truncate(struct zenixfs *fs, uint32_t ino, uint64_t size);
/* an inode that is unlinked while open: call zenixfs_orphan_add when the last name goes and the file is open,
 * zenixfs_orphan_done when the last close happens (frees it). zenixfs_unlink never frees an orphan-flagged inode. */
int  zenixfs_orphan_add(struct zenixfs *fs, uint32_t ino);
int  zenixfs_orphan_done(struct zenixfs *fs, uint32_t ino);
/* A restarted server (DESIGN D11 5.4) mounts with ZENIXFS_MNT_READONLYISH (the orphans stay), learns which inodes are
 * still open, and then: zenixfs_orphans_keep frees every orphan for which held() says 0 and keeps the others (held()
 * may note that they are orphans: their last close frees them); returns how many it kept, or negative.
 * zenixfs_ino_floor sets the next inode number to at least the committed one + 65536 and at least min, and commits:
 * numbers handed out in the seconds before a crash (not committed) are never handed out again while the kernel may
 * still hold them. */
int  zenixfs_orphans_keep(struct zenixfs *fs, int (*held)(uint32_t ino));
int  zenixfs_ino_floor(struct zenixfs *fs, uint32_t min);
/* write UUID (16 bytes) into the superblock of the volume on dev (mkfs.zenixfs, zenixfs-tool: a volume id, D11 4.3) */
int  zenixfs_set_uuid(struct zenixfs_dev *dev, const uint8_t *uuid);

/* Recovery. zenixfs_rebuild_bitmap: recompute the free-space map from the tree (every block the tree reaches is used, the rest is free)
 * and commit it; returns the number of blocks whose state changed, or negative. zenixfs_rewrite_super: write a new superblock for a
 * volume whose block 0 was destroyed (the geometry comes from the device). */
int  zenixfs_rebuild_bitmap(struct zenixfs *fs);   /* ECORRUPT, nothing changed, if the walk could not read the whole tree */
/* Repair a volume in place, or refuse and change nothing. want ZENIXFS_REPAIR_AUTO: the tree whole -> the map rebuilt; only
 * inner nodes damaged -> new inner nodes over the leaves (nothing lost, decided before anything is written); else the commit
 * before, if its tree is whole and it is really the one before (report.back says it went back; rule 3 keeps its data unless a
 * later transaction had started writing, which cannot be told: no data checksums). want K: the state K valid commits
 * back, whatever that is, on the owner's word (K = 0: the newest valid one, however many records around it are lost): past the
 * commit before, its files' data may have been overwritten.
 * Returns 0 clean, > 0 problems that remain (links), negative: refused, the volume as it was; then use salvage. */
#define ZENIXFS_REPAIR_AUTO 0xFFFFFFFFu
struct zenixfs_repair_report { uint32_t back, leaves, nodes, changed; };
int  zenixfs_repair(struct zenixfs_dev *dev, uint32_t cache, uint32_t want, void (*log)(void *, const char *), void *ctx, struct zenixfs_repair_report *rep);
int  zenixfs_count_commits(struct zenixfs_dev *dev);
int  zenixfs_super_ok(struct zenixfs_dev *dev);
/* number of blocks of a file that extents map (*mapped); returns 1 if the file is stored inline (no extents), 0 otherwise, or negative */
int  zenixfs_mapped_blocks(struct zenixfs *fs, uint32_t ino, uint64_t *mapped);
int  zenixfs_rewrite_super(struct zenixfs_dev *dev, const char *label, uint32_t reserve, uint32_t charset, uint64_t now);

/* check the whole volume: prints problems through log (may be NULL); returns the number of problems. */
int  zenixfs_fsck(struct zenixfs *fs, void (*log)(void *ctx, const char *msg), void *ctx);

/* statistics for tests and benchmarks */
/* mount_damage: commit records or roots that failed their checks at mount. Not 0 means the volume was mounted from an
 * OLDER commit (recent changes are gone) or a record was damaged: the caller must tell the user. */
/* mount_back: how many commits behind the newest that may have existed the mounted one is. mount_old: this is an older state
 * than the newest valid record, or 2 or more back, or its map copy was reused: older file data may have been overwritten; an
 * ordinary mount is then read-only. */
struct zenixfs_stats { uint32_t reads, writes, flushes, commits, cache_hits, cache_misses, mount_damage, mount_back, mount_old; };
void zenixfs_get_stats(struct zenixfs *fs, struct zenixfs_stats *st);

#endif
