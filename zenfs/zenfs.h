/* SPDX-License-Identifier: GPL-2.0-only */
/* ZenFS library: format in docs/zenfs-format.md, plan in docs/zenfs.md.
 * Portable C: needs only <stdint.h>, <stddef.h>, memcpy/memset/memcmp and malloc/free (ZFS_MALLOC/ZFS_FREE).
 * The same source runs on the host (tests, tools) and later inside the Zenix filesystem server. */
#ifndef ZENFS_H
#define ZENFS_H
#include <stdint.h>
#include <stddef.h>

#ifndef ZFS_MALLOC
#include <stdlib.h>
#define ZFS_MALLOC(n) malloc(n)
#define ZFS_FREE(p)   free(p)
#endif

/* Block device the filesystem sits on. Block numbers are filesystem blocks (1 << bshift bytes). */
struct zfs_dev {
    void *ctx;
    int (*read)(void *ctx, uint32_t blk, void *buf);          /* 0 or -1 */
    int (*write)(void *ctx, uint32_t blk, const void *buf);   /* 0 or -1 */
    int (*flush)(void *ctx);                                   /* barrier: all earlier writes are durable */
    uint32_t bshift;
    uint32_t nblocks;
};

#define ZFS_ROOT_INO 1
#define ZFS_NAME_MAX 255
#define ZFS_INLINE_MAX 192

/* errors (negative return values) */
#define ZFS_EIO      (-5)
#define ZFS_ENOENT   (-2)
#define ZFS_EEXIST   (-17)
#define ZFS_ENOTDIR  (-20)
#define ZFS_EISDIR   (-21)
#define ZFS_EINVAL   (-22)
#define ZFS_ENOSPC   (-28)
#define ZFS_ENOTEMPTY (-39)
#define ZFS_ENAMETOOLONG (-36)
#define ZFS_EFBIG    (-27)
#define ZFS_ECORRUPT (-117)   /* checksum or structure error found */

/* Unix mode type bits */
#define ZFS_S_IFMT  0170000
#define ZFS_S_IFDIR 0040000
#define ZFS_S_IFREG 0100000
#define ZFS_S_IFLNK 0120000

struct zfs_inode {
    uint32_t mode, uid, gid, nlink;
    uint64_t size, mtime, ctime;
    uint32_t flags, parent, rdev;
};

struct zfs_statfs { uint32_t bsize, blocks, bfree, files; };

struct zfs;   /* a mounted volume */

/* Names are opaque bytes; the volume records which encoding its creator used, as a hint for the operating system
 * (convert at the boundary when it differs from the system's own). The filesystem never changes or checks names. */
#define ZFS_CHARSET_LATIN1 0   /* the default, and what an unset field means */
#define ZFS_CHARSET_UTF8   1

/* make a filesystem on dev; label may be NULL. reserve = blocks kept for unlink (0 = default 2%). */
int  zfs_mkfs(struct zfs_dev *dev, const char *label, uint32_t reserve, uint64_t now);
/* mount; cache_blocks = size of the block cache in blocks (at least 32). */
int  zfs_mount(struct zfs **out, struct zfs_dev *dev, uint32_t cache_blocks);
int  zfs_unmount(struct zfs *fs);                    /* commits */
int  zfs_sync(struct zfs *fs);                       /* commit everything now */
void zfs_set_time(struct zfs *fs, uint64_t now);
int  zfs_statfs(struct zfs *fs, struct zfs_statfs *st);
uint32_t zfs_charset(struct zfs *fs);
int  zfs_set_charset(struct zfs *fs, uint32_t charset);     /* rewrites the superblock (one block) */

int  zfs_lookup(struct zfs *fs, uint32_t dir, const char *name, int len, uint32_t *ino);
int  zfs_getattr(struct zfs *fs, uint32_t ino, struct zfs_inode *out);
int  zfs_setattr(struct zfs *fs, uint32_t ino, const struct zfs_inode *in);   /* mode uid gid mtime flags(amiga bits) */
int  zfs_create(struct zfs *fs, uint32_t dir, const char *name, int len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t *ino);
int  zfs_mkdir(struct zfs *fs, uint32_t dir, const char *name, int len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t *ino);
int  zfs_symlink(struct zfs *fs, uint32_t dir, const char *name, int len, const char *target, uint32_t uid, uint32_t gid, uint32_t *ino);
int  zfs_readlink(struct zfs *fs, uint32_t ino, char *buf, int max);           /* length */
int  zfs_link(struct zfs *fs, uint32_t dir, const char *name, int len, uint32_t ino);
int  zfs_unlink(struct zfs *fs, uint32_t dir, const char *name, int len);
int  zfs_rmdir(struct zfs *fs, uint32_t dir, const char *name, int len);
int  zfs_rename(struct zfs *fs, uint32_t odir, const char *oname, int olen, uint32_t ndir, const char *nname, int nlen);
/* readdir: cookie 0 starts; returns 1 with an entry, 0 at the end. "." and ".." are returned first. */
int  zfs_readdir(struct zfs *fs, uint32_t dir, uint64_t *cookie, char *name, uint32_t *ino, uint8_t *type);
int  zfs_read(struct zfs *fs, uint32_t ino, uint64_t off, void *buf, uint32_t len);    /* bytes read */
int  zfs_write(struct zfs *fs, uint32_t ino, uint64_t off, const void *buf, uint32_t len);
int  zfs_truncate(struct zfs *fs, uint32_t ino, uint64_t size);
/* an inode that is unlinked while open: call zfs_orphan_add when the last name goes and the file is open,
 * zfs_orphan_done when the last close happens (frees it). zfs_unlink never frees an orphan-flagged inode. */
int  zfs_orphan_add(struct zfs *fs, uint32_t ino);
int  zfs_orphan_done(struct zfs *fs, uint32_t ino);

/* check the whole volume: prints problems through log (may be NULL); returns the number of problems. */
int  zfs_fsck(struct zfs *fs, void (*log)(void *ctx, const char *msg), void *ctx);

/* statistics for tests and benchmarks */
/* mount_damage: commit records or roots that failed their checks at mount. Not 0 means the volume was mounted from an
 * OLDER commit (recent changes are gone) or a record was damaged: the caller must tell the user. */
struct zfs_stats { uint32_t reads, writes, flushes, commits, cache_hits, cache_misses, mount_damage; };
void zfs_get_stats(struct zfs *fs, struct zfs_stats *st);

#endif
