/* SPDX-License-Identifier: GPL-2.0-only */
/* ZXFS library: format in docs/zxfs-format.md, plan in docs/zxfs.md.
 * Portable C: needs only <stdint.h>, <stddef.h>, memcpy/memset/memcmp and malloc/free (ZXFS_MALLOC/ZXFS_FREE).
 * The same source runs on the host (tests, tools) and later inside the Zenix filesystem server. */
#ifndef ZXFS_H
#define ZXFS_H
#include <stdint.h>
#include <stddef.h>

#ifndef ZXFS_MALLOC
#include <stdlib.h>
#define ZXFS_MALLOC(n) malloc(n)
#define ZXFS_FREE(p)   free(p)
#endif

/* Block device the filesystem sits on. Block numbers are filesystem blocks (1 << bshift bytes). */
struct zxfs_dev {
    void *ctx;
    int (*read)(void *ctx, uint32_t blk, void *buf);          /* 0 or -1 */
    int (*write)(void *ctx, uint32_t blk, const void *buf);   /* 0 or -1 */
    int (*flush)(void *ctx);                                   /* barrier: all earlier writes are durable */
    uint32_t bshift;
    uint32_t nblocks;
};

#define ZXFS_ROOT_INO 1
#define ZXFS_NAME_MAX 255
#define ZXFS_INLINE_MAX 192

/* errors (negative return values) */
#define ZXFS_EIO      (-5)
#define ZXFS_ENOENT   (-2)
#define ZXFS_EEXIST   (-17)
#define ZXFS_ENOTDIR  (-20)
#define ZXFS_EISDIR   (-21)
#define ZXFS_EINVAL   (-22)
#define ZXFS_ENOSPC   (-28)
#define ZXFS_ENOTEMPTY (-39)
#define ZXFS_ENAMETOOLONG (-36)
#define ZXFS_EFBIG    (-27)
#define ZXFS_ECORRUPT (-117)   /* checksum or structure error found */

/* Unix mode type bits */
#define ZXFS_S_IFMT  0170000
#define ZXFS_S_IFDIR 0040000
#define ZXFS_S_IFREG 0100000
#define ZXFS_S_IFLNK 0120000

struct zxfs_inode {
    uint32_t mode, uid, gid, nlink;
    uint64_t size, mtime, ctime;
    uint32_t flags, parent, rdev;
};

struct zxfs_statfs { uint32_t bsize, blocks, bfree, files; };

struct zxfs;   /* a mounted volume */

/* Names are opaque bytes; the volume records which encoding its creator used, as a hint for the operating system
 * (convert at the boundary when it differs from the system's own). The filesystem never changes or checks names. */
#define ZXFS_CHARSET_LATIN1 0   /* the default, and what an unset field means */
#define ZXFS_CHARSET_UTF8   1

/* make a filesystem on dev; label may be NULL. reserve = blocks kept for unlink (0 = default 2%). */
int  zxfs_mkfs(struct zxfs_dev *dev, const char *label, uint32_t reserve, uint64_t now);
/* mount; cache_blocks = size of the block cache in blocks (at least 32). */
int  zxfs_mount(struct zxfs **out, struct zxfs_dev *dev, uint32_t cache_blocks);
int  zxfs_unmount(struct zxfs *fs);                    /* commits */
int  zxfs_sync(struct zxfs *fs);                       /* commit everything now */
void zxfs_set_time(struct zxfs *fs, uint64_t now);
int  zxfs_statfs(struct zxfs *fs, struct zxfs_statfs *st);
uint32_t zxfs_charset(struct zxfs *fs);
int  zxfs_set_charset(struct zxfs *fs, uint32_t charset);     /* rewrites the superblock (one block) */

int  zxfs_lookup(struct zxfs *fs, uint32_t dir, const char *name, int len, uint32_t *ino);
int  zxfs_getattr(struct zxfs *fs, uint32_t ino, struct zxfs_inode *out);
int  zxfs_setattr(struct zxfs *fs, uint32_t ino, const struct zxfs_inode *in);   /* mode uid gid mtime flags(amiga bits) */
int  zxfs_create(struct zxfs *fs, uint32_t dir, const char *name, int len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t *ino);
int  zxfs_mkdir(struct zxfs *fs, uint32_t dir, const char *name, int len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t *ino);
int  zxfs_symlink(struct zxfs *fs, uint32_t dir, const char *name, int len, const char *target, uint32_t uid, uint32_t gid, uint32_t *ino);
int  zxfs_readlink(struct zxfs *fs, uint32_t ino, char *buf, int max);           /* length */
int  zxfs_link(struct zxfs *fs, uint32_t dir, const char *name, int len, uint32_t ino);
int  zxfs_unlink(struct zxfs *fs, uint32_t dir, const char *name, int len);
int  zxfs_rmdir(struct zxfs *fs, uint32_t dir, const char *name, int len);
int  zxfs_rename(struct zxfs *fs, uint32_t odir, const char *oname, int olen, uint32_t ndir, const char *nname, int nlen);
/* readdir: cookie 0 starts; returns 1 with an entry, 0 at the end. "." and ".." are returned first. */
int  zxfs_readdir(struct zxfs *fs, uint32_t dir, uint64_t *cookie, char *name, uint32_t *ino, uint8_t *type);
int  zxfs_read(struct zxfs *fs, uint32_t ino, uint64_t off, void *buf, uint32_t len);    /* bytes read */
int  zxfs_write(struct zxfs *fs, uint32_t ino, uint64_t off, const void *buf, uint32_t len);
int  zxfs_truncate(struct zxfs *fs, uint32_t ino, uint64_t size);
/* an inode that is unlinked while open: call zxfs_orphan_add when the last name goes and the file is open,
 * zxfs_orphan_done when the last close happens (frees it). zxfs_unlink never frees an orphan-flagged inode. */
int  zxfs_orphan_add(struct zxfs *fs, uint32_t ino);
int  zxfs_orphan_done(struct zxfs *fs, uint32_t ino);

/* check the whole volume: prints problems through log (may be NULL); returns the number of problems. */
int  zxfs_fsck(struct zxfs *fs, void (*log)(void *ctx, const char *msg), void *ctx);

/* statistics for tests and benchmarks */
/* mount_damage: commit records or roots that failed their checks at mount. Not 0 means the volume was mounted from an
 * OLDER commit (recent changes are gone) or a record was damaged: the caller must tell the user. */
struct zxfs_stats { uint32_t reads, writes, flushes, commits, cache_hits, cache_misses, mount_damage; };
void zxfs_get_stats(struct zxfs *fs, struct zxfs_stats *st);

#endif
