#pragma once
/* ext2 definitions shared by the filesystem server (user mode) and the kernel.
 * Needs u8/u16/u32 from the including file. */

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
struct inode {
    u32 ino, size, mtime, nblocks, block[15];
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
int ext2_mount(void);        /* needs fs_blk_read/fs_blk_write from the environment */
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
