/* SPDX-License-Identifier: GPL-2.0-only */
#include "fsenv.h"
#include "../kernel/fsproto.h"
#include "../kernel/diskproto.h"
#include "diskcall.h"

/* The filesystem server: kernel/ext2.c in user mode behind the "fs" port.
 * Embedded in the kernel image (the kernel needs it before any disk is
 * readable); the kernel's VFS layer talks to it with messages. */

static int disk_port, fs_dev = -1;
static char devname[16];
static int quitting;

static int disk_rw(u32 op, u32 lba, u32 count, const void *buf)
{
    struct umsg m;
    m.type = op; m.a[0] = (u32)fs_dev; m.a[1] = lba; m.a[2] = count; m.a[3] = (u32)buf;
    return disk_call(disk_port, &m);
}
int fs_blk_read(u32 lba, u32 count, void *buf) { return disk_rw(DISK_READ, lba, count, buf); }
int fs_blk_write(u32 lba, u32 count, const void *buf) { return disk_rw(DISK_WRITE, lba, count, buf); }

static int disk_open(const char *name, u32 flags)
{
    struct umsg m;
    m.type = DISK_OPEN; m.a[0] = (u32)name; m.a[1] = flags;
    return disk_call(disk_port, &m);
}
static void disk_unmount(int h)
{
    struct umsg m;
    m.type = DISK_UNMOUNT; m.a[0] = (u32)h;
    disk_call(disk_port, &m);
}

/* A device is opened plain to read its superblock and only mounted (made ours alone) once ext2_mount_as accepts it.
 * mode EXT2_RW: refuse a volume Zenix could only read (the root must be writable); EXT2_ANY: read-only is allowed. */
static int try_mount(const char *name, int mode)
{
    fs_dev = disk_open(name, 0);
    if (fs_dev < 0 || !ext2_probe()) return -1;
    if (ext2_mount_as(mode)) {
        printf("fsd: %s: %s\n", name, ext2_why);
        return -1;
    }
    int h = disk_open(name, DISK_MOUNT);
    if (h < 0) return -1;
    fs_dev = h;
    strncpy(devname, name, sizeof devname - 1);
    return 0;
}

/* Open inodes (FS_OPEN counts one, FS_CLOSE from the kernel one less): an inode whose last name goes while it is
 * open keeps its blocks until the last close (ext2_busy / ext2_release), so an old handle never reaches a file that
 * reused the number. After a crash such an inode is left with no links: fsck.ext2 (or e2fsck) frees it. */
#define NOPEN 96                                      /* the kernel's file table (NFILE 64) plus programs being loaded */
static struct { u32 ino, count; } opens[NOPEN];
static int open_busy(u32 ino)
{
    for (int i = 0; i < NOPEN; i++) if (opens[i].count && opens[i].ino == ino) return 1;
    return 0;
}
static int open_ref(u32 ino)
{
    int fr = -1;
    for (int i = 0; i < NOPEN; i++) {
        if (opens[i].count && opens[i].ino == ino) { opens[i].count++; return 0; }
        if (!opens[i].count && fr < 0) fr = i;
    }
    if (fr < 0) return -ENFILE;                       /* should not happen (the kernel has fewer files) */
    opens[fr].ino = ino; opens[fr].count = 1;
    return 0;
}
/* make what was written durable: the cache is write-through, so only the device's own cache is left. A device
 * that cannot flush (-95, no DISKCAP_FLUSH) has had everything sent to it: that is all that can be done */
static int disk_flush(void)
{
    struct umsg m;
    m.type = DISK_FLUSH; m.a[0] = (u32)fs_dev;
    int r = disk_call(disk_port, &m);
    return r == -95 ? 0 : r;
}
static void open_unref(u32 ino)
{
    for (int i = 0; i < NOPEN; i++)
        if (opens[i].count && opens[i].ino == ino) {
            if (--opens[i].count == 0 && !ext2_readonly) ext2_release(ino);   /* frees it only if it has no names */
            return;
        }
}

static void fill_stat(const struct inode *in, struct ustat *st)
{
    st->ino = in->ino; st->mode = in->mode; st->size = in->size;
    st->mtime = in->mtime; st->uid = in->uid; st->gid = in->gid; st->links = in->links;
}

/* parent directory + last name of a path (kernel-copied, so trusted) */
static int parent(const char *path, u32 cwd, struct inode *dir, char *name)
{
    return ext2_namei_parent(path, cwd, dir, name);
}

/* permission helpers: the caller is in ext2_uid/ext2_gid (set per request in handle) */
static int deny(void) { ext2_denied = 1; return 0; }
static int dir_may(const struct inode *dir)                /* add or remove entries: w and x */
{
    return !ext2_enforce || fsp_dir_modify(ext2_uid, ext2_gid, dir->mode, dir->uid, dir->gid) || deny();
}
static int owner_only(const struct inode *in)
{
    return !ext2_enforce || fsp_owner_or_root(ext2_uid, in->uid) || deny();
}
static int sticky_ok(const struct inode *dir, const char *name)  /* in a sticky directory only owners may remove */
{
    u32 ino; struct inode t;
    if (!ext2_enforce || ext2_lookup(dir, name, strlen(name), &ino) || ext2_iget(ino, &t)) return 1;
    return fsp_sticky_ok(ext2_uid, dir->mode, dir->uid, t.uid) || deny();
}

/* a write or truncate by someone else than the owner and root: set-user-id (and set-group-id) go (fsperm.h rule 9) */
static int clear_setid(struct inode *in)
{
    u16 nm = (u16)fsp_write_clear(ext2_uid, in->uid, in->mode);
    if (nm == in->mode) return 0;
    in->mode = nm;
    return ext2_iput(in);
}

/* One request. The result is >= 0, or a negative errno (kernel/errno.h) - the same numbers the ZenixFS server returns
 * for the same cases (audit E). A permission failure becomes -EACCES in handle(). */
static int handle1(struct umsg *m)
{
    struct inode in, dir, dir2;
    char name[64], name2[64];
    int r;
    switch (m->type) {
    case FS_OPEN: {
        const char *path = (const char *)m->a[0];
        u32 flags = m->a[2];
        int writing = (flags & (O_WRONLY | O_RDWR)) != 0;
        if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {   /* exclusive: the name must not exist, not even as a link */
            if (!ext2_namei_ex(path, m->a[1], &in, 0, 0)) return -EEXIST;
            if (ext2_denied) return -EACCES;
        }
        if ((r = ext2_namei(path, m->a[1], &in, 0))) {
            if (ext2_denied || !(flags & O_CREAT) || r != -ENOENT) return r;
            if (!ext2_namei_ex(path, m->a[1], &in, 0, 0))
                return -ENOENT;                     /* a symbolic link to nothing: not a second entry of that name */
            if (ext2_readonly) return -EROFS;
            if ((r = parent(path, m->a[1], &dir, name))) return r;
            if (!dir_may(&dir)) return -EACCES;
            if ((r = ext2_create(&dir, name, &in))) return r;
        } else {
            if (!ext2_access(&in, fsp_open_want(flags))) return -EACCES;
            if (writing) {
                if (ext2_readonly) return -EROFS;
                if (S_ISDIR(in.mode)) return -EISDIR;
                if (!S_ISREG(in.mode)) return -EACCES;
                if ((flags & O_TRUNC) && ((r = clear_setid(&in)) || (r = ext2_truncate(&in)))) return r;
            }
        }
        int e = open_ref(in.ino);
        if (e) return e;
        fill_stat(&in, (struct ustat *)m->a[3]);
        return (int)in.ino;
    }
    case FS_READ:
        if ((r = ext2_iget(m->a[0], &in))) return r;
        if (S_ISDIR(in.mode)) return -EISDIR;
        return ext2_read(&in, m->a[1], (void *)m->a[2], m->a[3]);
    case FS_WRITE:
        if ((r = ext2_iget(m->a[0], &in))) return r;
        if (S_ISDIR(in.mode)) return -EISDIR;
        in.mode = (u16)fsp_write_clear(ext2_uid, in.uid, in.mode);    /* stored by ext2_write with the new size */
        return ext2_write(&in, m->a[1], (const void *)m->a[2], m->a[3]);
    case FS_STAT:
        if ((r = ext2_namei((const char *)m->a[0], m->a[1], &in, 0))) return r;
        fill_stat(&in, (struct ustat *)m->a[2]);
        return 0;
    case FS_FSTAT:
        if ((r = ext2_iget(m->a[0], &in))) return r;
        fill_stat(&in, (struct ustat *)m->a[1]);
        return 0;
    case FS_READDIR: {
        u32 cookie = m->a[1];
        if ((r = ext2_iget(m->a[0], &in))) return r;
        if (!S_ISDIR(in.mode)) return -ENOTDIR;
        r = ext2_readdir(&in, &cookie, (struct udirent *)m->a[2]);
        *(u32 *)m->a[3] = cookie;
        return r;
    }
    case FS_STATFS: {
        struct ustatfs *f = (struct ustatfs *)m->a[0];
        return ext2_statfs(&f->bsize, &f->blocks, &f->bfree, &f->files, &f->ffree);
    }
    case FS_CLOSE:
        open_unref(m->a[0]);
        return 0;
    case FS_SYNC:
        return disk_flush() < 0 ? -EIO : 0;           /* writes went straight to the disk; the drive's cache is left */
    case FS_CHMOD:
        if ((r = ext2_namei((const char *)m->a[0], m->a[1], &in, 0))) return r;
        if (!owner_only(&in)) return -EACCES;
        return ext2_chmod(in.ino, fsp_chmod_mode(ext2_uid, ext2_gid, in.gid, m->a[2]));
    case FS_CHOWN:
        if ((r = ext2_namei((const char *)m->a[0], m->a[1], &in, 0))) return r;
        if (ext2_enforce && !fsp_is_root(ext2_uid)) { ext2_denied = 1; return -EACCES; }
        return ext2_chown(in.ino, m->a[2], m->a[3]);
    case FS_UTIMES: {
        if ((r = ext2_namei((const char *)m->a[0], m->a[1], &in, 0))) return r;
        int given = m->a[2] != FS_UTIME_NOW;
        if (ext2_enforce && !fsp_utimes_ok(ext2_uid, in.uid, given, fsp_access(ext2_uid, ext2_gid, in.mode, in.uid, in.gid, FSP_W)))
            { deny(); return -EACCES; }
        return ext2_utimes(in.ino, given ? m->a[2] : fs_time());
    }
    case FS_LSTAT:
        if ((r = ext2_namei_ex((const char *)m->a[0], m->a[1], &in, 0, 0))) return r;
        fill_stat(&in, (struct ustat *)m->a[2]);
        return 0;
    case FS_LINK:
        if ((r = ext2_namei((const char *)m->a[0], m->a[2], &in, 0))) return r;
        if (S_ISDIR(in.mode)) return -EPERM;          /* no hard links to directories */
        if ((r = parent((const char *)m->a[1], m->a[2], &dir, name))) return r;
        if (!dir_may(&dir)) return -EACCES;
        return ext2_hardlink(&dir, name, in.ino);
    case FS_SYMLINK:
        if ((r = parent((const char *)m->a[1], m->a[2], &dir, name))) return r;
        if (!dir_may(&dir)) return -EACCES;
        return ext2_symlink(&dir, name, (const char *)m->a[0]);
    case FS_READLINK:
        if ((r = ext2_namei_ex((const char *)m->a[0], m->a[1], &in, 0, 0))) return r;
        if (!S_ISLNK(in.mode)) return -EINVAL;
        return ext2_readlink(&in, (char *)m->a[2], m->a[3]);
    case FS_ROOT: {
        struct fsinfo *fi = (struct fsinfo *)m->a[0];
        strncpy(fi->dev, devname, sizeof fi->dev - 1);
        strcpy(fi->type, "ext2");
        return 2;                                     /* EXT2_ROOT_INO */
    }
    case FS_QUIT:
        quitting = 1;
        return 0;
    case FS_CHDIR:
        if ((r = ext2_namei((const char *)m->a[0], m->a[1], &in, 0))) return r;
        if (!S_ISDIR(in.mode)) return -ENOTDIR;
        if (!ext2_access(&in, 1)) return -EACCES;
        return (int)in.ino;
    case FS_MKDIR:
        if ((r = parent((const char *)m->a[0], m->a[1], &dir, name))) return r == -EINVAL ? -EEXIST : r;   /* "." ".." exist */
        if (!dir_may(&dir)) return -EACCES;
        return ext2_mkdir(&dir, name);
    case FS_UNLINK:
        if ((r = parent((const char *)m->a[0], m->a[1], &dir, name))) return r == -EINVAL ? -EISDIR : r;
        if (!dir_may(&dir) || !sticky_ok(&dir, name)) return -EACCES;
        return ext2_unlink(&dir, name);
    case FS_RMDIR:
        if ((r = parent((const char *)m->a[0], m->a[1], &dir, name))) return r;
        if (!dir_may(&dir) || !sticky_ok(&dir, name)) return -EACCES;
        return ext2_rmdir(&dir, name);
    case FS_RENAME:
        if ((r = parent((const char *)m->a[0], m->a[2], &dir, name)) ||
            (r = parent((const char *)m->a[1], m->a[2], &dir2, name2))) return r;
        if (!dir_may(&dir) || !dir_may(&dir2) || !sticky_ok(&dir, name) || !sticky_ok(&dir2, name2)) return -EACCES;
        return ext2_rename(&dir, name, &dir2, name2);
    }
    return -ENOSYS;
}

/* requests that change the volume: refused at once on a read-only mount (ext2.c also refuses every block write) */
static int changes(const struct umsg *m)
{
    switch (m->type) {
    case FS_WRITE: case FS_CHMOD: case FS_CHOWN: case FS_UTIMES: case FS_LINK: case FS_SYMLINK:
    case FS_MKDIR: case FS_UNLINK: case FS_RMDIR: case FS_RENAME: return 1;
    }
    return 0;
}

static int handle(struct umsg *m)
{
    if (!(m->kflags & UMSG_KERNEL) && m->uid != 0) return -EACCES; /* raw IPC from a user: inode numbers would bypass every check */
    if (ext2_readonly && changes(m)) return -EROFS;
    ext2_uid = m->uid; ext2_gid = m->gid; ext2_umask = m->umask;
    ext2_enforce = 1; ext2_denied = 0;
    int r = handle1(m);
    return r < 0 && ext2_denied ? -EACCES : r;
}

/* fsd DEVICE PORTNAME [-root] [-id ID]   the ext2 filesystem on DEVICE, served on port PORTNAME. Built into the kernel
 * and started as a service (DESIGN D11): boot registers the root's ("fs", -root: it must be writable, as the root is
 * - a refused or read-only volume, ext3, ext4, errors, is never the root); also /bin/mount.ext2, started by mount. */
int main(int argc, char **argv)
{
    ext2_busy = open_busy;
    disk_port = sys_port_find("disk");                /* a registered service: calling it starts diskd if need be */
    int root = argc > 3 && !strcmp(argv[3], "-root");
    if (argc < 3) { printf("usage: fsd DEVICE PORT [-root] [-id ID]\n"); return 2; }
    const char *pname = argv[2];
    if (try_mount(argv[1], root ? EXT2_RW : EXT2_ANY)) {
        printf(root ? "fsd: no writable ext2 filesystem on %s for the root\n"
                    : "mount.ext2: %s is not an ext2 filesystem Zenix can mount, or is in use\n", argv[1]);
        return 1;
    }
    int port = sys_port_create(pname);
    if (port < 0) { printf("fsd: cannot create the port %s\n", pname); return 1; }
    struct umsg *m = sys_recv(port, 1);
    while ((int)m != -1) {
        int r = handle(m);
        if (quitting) {                                 /* umount: flush, answer, then let go of the device */
            for (int i = 0; i < NOPEN; i++)             /* nothing is open at umount; deletions still waiting are done */
                if (opens[i].count && !ext2_readonly) { opens[i].count = 0; ext2_release(opens[i].ino); }
            disk_flush();
            sys_reply(m, r);
            disk_unmount(fs_dev);
            return 0;
        }
        m = sys_replyrecv(m, r, port);                  /* reply and wait in one trap */
    }
    return 1;
}
