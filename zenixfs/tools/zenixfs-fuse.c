/* SPDX-License-Identifier: GPL-2.0-only */
/* zenixfs-fuse: mount a ZenixFS image file as a folder in Linux.
 *   zenixfs-fuse IMAGE MOUNTPOINT [fuse options]      unmount with: fusermount3 -u MOUNTPOINT
 * Single-threaded (the library is not thread-safe). Changes are committed on fsync, close, when the cache
 * fills, and at unmount; a kill -9 or power cut leaves the last commit, which is the point of ZenixFS. */
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "../zenixfs.h"
#include "../host/zdev.h"

static struct zfile img;
static struct zenixfs *fs;

#define ENTER() zenixfs_set_time(fs, (uint64_t)time(NULL))

/* resolve PATH to an inode; with want_parent stop at the parent and return the last name */
static int resolve(const char *path, int want_parent, uint32_t *ino, const char **last, int *lastlen)
{
    uint32_t cur = ZENIXFS_ROOT_INO; const char *p = path;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) { if (want_parent) return -EINVAL; *ino = cur; return 0; }
        const char *e = p; while (*e && *e != '/') e++;
        const char *q = e; while (*q == '/') q++;
        if (want_parent && !*q) { *ino = cur; *last = p; *lastlen = (int)(e - p); return 0; }
        int r = zenixfs_lookup(fs, cur, p, (int)(e - p), &cur);
        if (r) return r;
        p = e;
    }
}
static void fill_stat(uint32_t ino, const struct zenixfs_inode *in, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_ino = ino; st->st_mode = in->mode; st->st_nlink = in->nlink; st->st_uid = in->uid; st->st_gid = in->gid;
    st->st_size = (off_t)in->size; st->st_blksize = 4096; st->st_blocks = (in->size + 511) / 512;
    st->st_mtim.tv_sec = st->st_atim.tv_sec = (time_t)in->mtime; st->st_ctim.tv_sec = (time_t)in->ctime;
}

static int z_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi; ENTER(); uint32_t ino; int r = resolve(path, 0, &ino, NULL, NULL); if (r) return r;
    struct zenixfs_inode in; r = zenixfs_getattr(fs, ino, &in); if (r) return r;
    fill_stat(ino, &in, st); return 0;
}
static int z_readdir(const char *path, void *buf, fuse_fill_dir_t fill, off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags fl)
{
    (void)off; (void)fi; (void)fl; ENTER(); uint32_t dir; int r = resolve(path, 0, &dir, NULL, NULL); if (r) return r;
    uint64_t cookie = 0; char name[300]; uint32_t ino; uint8_t type;
    while ((r = zenixfs_readdir(fs, dir, &cookie, name, &ino, &type)) > 0) fill(buf, name, NULL, 0, 0);
    return r < 0 ? r : 0;
}
static int z_open(const char *path, struct fuse_file_info *fi)
{
    (void)fi; uint32_t ino; return resolve(path, 0, &ino, NULL, NULL);
}
static int z_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
    (void)fi; ENTER(); uint32_t ino; int r = resolve(path, 0, &ino, NULL, NULL); if (r) return r;
    return zenixfs_read(fs, ino, (uint64_t)off, buf, (uint32_t)size);
}
static int z_write(const char *path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
    (void)fi; ENTER(); uint32_t ino; int r = resolve(path, 0, &ino, NULL, NULL); if (r) return r;
    return zenixfs_write(fs, ino, (uint64_t)off, buf, (uint32_t)size);
}
static int z_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi; ENTER(); uint32_t dir, ino; const char *nm; int nl; int r = resolve(path, 1, &dir, &nm, &nl); if (r) return r;
    return zenixfs_create(fs, dir, nm, nl, mode & 07777, fuse_get_context()->uid, fuse_get_context()->gid, &ino);
}
static int z_mkdir(const char *path, mode_t mode)
{
    ENTER(); uint32_t dir, ino; const char *nm; int nl; int r = resolve(path, 1, &dir, &nm, &nl); if (r) return r;
    return zenixfs_mkdir(fs, dir, nm, nl, mode & 07777, fuse_get_context()->uid, fuse_get_context()->gid, &ino);
}
static int z_unlink(const char *path)
{
    ENTER(); uint32_t dir; const char *nm; int nl; int r = resolve(path, 1, &dir, &nm, &nl); if (r) return r;
    return zenixfs_unlink(fs, dir, nm, nl);
}
static int z_rmdir(const char *path)
{
    ENTER(); uint32_t dir; const char *nm; int nl; int r = resolve(path, 1, &dir, &nm, &nl); if (r) return r;
    return zenixfs_rmdir(fs, dir, nm, nl);
}
static int z_rename(const char *from, const char *to, unsigned flags)
{
    if (flags) return -EINVAL;
    ENTER(); uint32_t d1, d2; const char *n1, *n2; int l1, l2;
    int r = resolve(from, 1, &d1, &n1, &l1); if (r) return r;
    r = resolve(to, 1, &d2, &n2, &l2); if (r) return r;
    return zenixfs_rename(fs, d1, n1, l1, d2, n2, l2);
}
static int z_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi; ENTER(); uint32_t ino; int r = resolve(path, 0, &ino, NULL, NULL); if (r) return r;
    return zenixfs_truncate(fs, ino, (uint64_t)size);
}
static int z_symlink(const char *target, const char *path)
{
    ENTER(); uint32_t dir, ino; const char *nm; int nl; int r = resolve(path, 1, &dir, &nm, &nl); if (r) return r;
    return zenixfs_symlink(fs, dir, nm, nl, target, fuse_get_context()->uid, fuse_get_context()->gid, &ino);
}
static int z_readlink(const char *path, char *buf, size_t size)
{
    ENTER(); uint32_t ino; int r = resolve(path, 0, &ino, NULL, NULL); if (r) return r;
    r = zenixfs_readlink(fs, ino, buf, (int)size - 1); if (r < 0) return r;
    buf[r] = 0; return 0;
}
static int z_link(const char *from, const char *to)
{
    ENTER(); uint32_t ino, dir; const char *nm; int nl; int r = resolve(from, 0, &ino, NULL, NULL); if (r) return r;
    r = resolve(to, 1, &dir, &nm, &nl); if (r) return r;
    return zenixfs_link(fs, dir, nm, nl, ino);
}
static int z_setattr(const char *path, int what, mode_t mode, uid_t uid, gid_t gid, time_t mtime)
{
    ENTER(); uint32_t ino; int r = resolve(path, 0, &ino, NULL, NULL); if (r) return r;
    struct zenixfs_inode in; r = zenixfs_getattr(fs, ino, &in); if (r) return r;
    if (what & 1) in.mode = mode & 07777;
    if (what & 2) { in.uid = uid; in.gid = gid; }
    if (what & 4) in.mtime = (uint64_t)mtime;
    return zenixfs_setattr(fs, ino, &in);
}
static int z_chmod(const char *p, mode_t m, struct fuse_file_info *fi) { (void)fi; return z_setattr(p, 1, m, 0, 0, 0); }
static int z_chown(const char *p, uid_t u, gid_t g, struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino; int r = resolve(p, 0, &ino, NULL, NULL); if (r) return r;
    struct zenixfs_inode in; r = zenixfs_getattr(fs, ino, &in); if (r) return r;
    return z_setattr(p, 2, 0, u == (uid_t)-1 ? in.uid : u, g == (gid_t)-1 ? in.gid : g, 0);
}
static int z_utimens(const char *p, const struct timespec ts[2], struct fuse_file_info *fi)
{
    (void)fi; return z_setattr(p, 4, 0, 0, 0, ts[1].tv_nsec == UTIME_NOW ? time(NULL) : ts[1].tv_sec);
}
static int z_statfs(const char *path, struct statvfs *sv)
{
    (void)path; struct zenixfs_statfs st; zenixfs_statfs(fs, &st);
    memset(sv, 0, sizeof *sv);
    sv->f_bsize = sv->f_frsize = st.bsize; sv->f_blocks = st.blocks; sv->f_bfree = sv->f_bavail = st.bfree; sv->f_files = st.files; sv->f_namemax = 255;
    return 0;
}
static int z_fsync(const char *path, int datasync, struct fuse_file_info *fi) { (void)path; (void)datasync; (void)fi; return zenixfs_sync(fs); }
static int z_flush(const char *path, struct fuse_file_info *fi) { (void)path; (void)fi; return zenixfs_sync(fs); }
static void *z_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn; cfg->use_ino = 1; cfg->kernel_cache = 0; cfg->entry_timeout = 0; cfg->attr_timeout = 0;   /* always current: a link count or size changed by another path must show at once */ return NULL;
}
static void z_destroy(void *p) { (void)p; zenixfs_unmount(fs); zfile_close(&img); }

static const struct fuse_operations ops = {
    .getattr = z_getattr, .readdir = z_readdir, .open = z_open, .read = z_read, .write = z_write, .create = z_create,
    .mkdir = z_mkdir, .unlink = z_unlink, .rmdir = z_rmdir, .rename = z_rename, .truncate = z_truncate, .symlink = z_symlink,
    .readlink = z_readlink, .link = z_link, .chmod = z_chmod, .chown = z_chown, .utimens = z_utimens, .statfs = z_statfs,
    .fsync = z_fsync, .flush = z_flush, .init = z_init, .destroy = z_destroy,
};

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: zenixfs-fuse IMAGE MOUNTPOINT [fuse options]\n"); return 2; }
    const char *path = argv[1];
    int ok = 0;
    for (uint32_t bs = 10; bs <= 12 && !ok; bs++) {
        if (zfile_open(&img, path, bs, 0) < 0) { perror(path); return 1; }
        if (zenixfs_mount(&fs, &img.dev, 1024) == 0) ok = 1; else zfile_close(&img);
    }
    if (!ok) { fprintf(stderr, "zenixfs-fuse: %s is not a ZenixFS image (or it is damaged)\n", path); return 1; }
    struct zenixfs_stats ms; zenixfs_get_stats(fs, &ms);
    if (ms.mount_damage) fprintf(stderr, "zenixfs-fuse: WARNING: %u damaged commit record(s) at mount; mounted from an OLDER commit\n", ms.mount_damage);
    char **fargv = malloc((argc + 2) * sizeof *fargv); int fa = 0;
    fargv[fa++] = argv[0];
    for (int i = 2; i < argc; i++) fargv[fa++] = argv[i];
    fargv[fa++] = (char *)"-s";                 /* single-threaded */
    return fuse_main(fa, fargv, &ops, NULL);
}
