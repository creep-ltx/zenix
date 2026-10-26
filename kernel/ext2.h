/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* ext2 definitions shared by the filesystem server (user mode) and the kernel.
 * Needs u8/u16/u32 from the including file. */

#include "fsperm.h"                 /* the permission rules shared by every filesystem server */

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
struct inode {
    u32 ino, size, mtime, nblocks, block[15];
    u32 flags, acl, size_high;          /* i_flags, i_file_acl (extended-attribute block), i_size_high of a regular file */
    u16 mode, uid, gid, links;
};
#ifndef USYS_STRUCTS
struct udirent {                /* returned by SYS_readdir */
    u32 ino;
    u32 type;
    char name[64];
};
struct ustat {                  /* returned by SYS_stat / SYS_fstat */
    u32 ino, mode, size, mtime;
    u32 uid, gid, links;
};
#endif
int ext2_probe(void);        /* does the open device hold an ext2 filesystem? */
int ext2_mount(void);        /* needs fs_blk_read/fs_blk_write from the environment; = ext2_mount_as(EXT2_RW) */
#define EXT2_ANY 0           /* mount read-only when the volume needs it (journal, unknown read-only features, errors) */
#define EXT2_RW  1           /* the volume must be writable (the root, fsck): otherwise it is refused */
int ext2_mount_as(int mode); /* 0, or -1 (refused: ext2_why says why) */
extern int ext2_readonly;    /* the mounted volume is read-only: writes fail with -30 (EROFS) */
extern const char *ext2_why; /* why the last mount was refused or read-only */
extern int (*ext2_busy)(u32 ino);   /* set by the server: is the inode open? Its deletion then waits for ext2_release */
int ext2_mark_unclean(void); /* clear the "cleanly unmounted" state bit (a restart after a crash mid-request, D11) */
int ext2_set_clean(int clean); /* FS_MARK (D15): the clean bit set or cleared; the bit before (1 clean), or -EROFS/-EIO */
int ext2_release(u32 ino);   /* the last close of an inode that lost its last name while open: free it now */
int ext2_iget(u32 ino, struct inode *out);
u32 ext2_bmap(const struct inode *in, u32 n);
int ext2_read(const struct inode *in, u32 off, void *dst, u32 n);
int ext2_readdir(const struct inode *dir, u32 *cookie, struct udirent *out);
int ext2_lookup(const struct inode *dir, const char *name, u32 len, u32 *ino);
int ext2_namei(const char *path, u32 cwd, struct inode *out, int depth);
int ext2_iput(const struct inode *in);
int ext2_write(struct inode *in, u32 off, const void *src, u32 n);
int ext2_truncate(struct inode *in);
int ext2_namei_parent(const char *path, u32 cwd, struct inode *dir, char *name);
int ext2_create(struct inode *dir, const char *name, struct inode *out);
int ext2_mkdir(struct inode *dir, const char *name);
int ext2_unlink(struct inode *dir, const char *name);
int ext2_rmdir(struct inode *dir, const char *name);
int ext2_rename(struct inode *od, const char *on, struct inode *nd, const char *nn);

int ext2_link(struct inode *dir, const char *name, u32 ino);
int ext2_statfs(u32 *bs, u32 *blocks, u32 *bfree, u32 *files, u32 *ffree);
int ext2_namei_ex(const char *path, u32 cwd, struct inode *out, int depth, int follow_last);
int ext2_readlink(const struct inode *in, char *buf, u32 max);
int ext2_chmod(u32 ino, u32 mode);
int ext2_chown(u32 ino, u32 uid, u32 gid);
int ext2_access(const struct inode *in, int want);
extern u32 ext2_uid, ext2_gid, ext2_umask;
extern int ext2_enforce, ext2_denied;
int ext2_utimes(u32 ino, u32 t);
int ext2_hardlink(struct inode *dir, const char *name, u32 ino);
int ext2_symlink(struct inode *dir, const char *name, const char *target);
