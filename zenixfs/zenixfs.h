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

/* check the whole volume: prints problems through log (may be NULL); returns the number of problems. */
int  zenixfs_fsck(struct zenixfs *fs, void (*log)(void *ctx, const char *msg), void *ctx);

/* statistics for tests and benchmarks */
/* mount_damage: commit records or roots that failed their checks at mount. Not 0 means the volume was mounted from an
 * OLDER commit (recent changes are gone) or a record was damaged: the caller must tell the user. */
struct zenixfs_stats { uint32_t reads, writes, flushes, commits, cache_hits, cache_misses, mount_damage; };
void zenixfs_get_stats(struct zenixfs *fs, struct zenixfs_stats *st);

#endif
