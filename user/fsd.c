#include "fsenv.h"
#include "../kernel/fsproto.h"
#include "../kernel/diskproto.h"
#include "rdb.h"
#include "rootpick.h"

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
    return sys_call(disk_port, &m);
}
int fs_blk_read(u32 lba, u32 count, void *buf) { return disk_rw(DISK_READ, lba, count, buf); }
int fs_blk_write(u32 lba, u32 count, const void *buf) { return disk_rw(DISK_WRITE, lba, count, buf); }

static int disk_open(const char *name, u32 flags)
{
    struct umsg m;
    m.type = DISK_OPEN; m.a[0] = (u32)name; m.a[1] = flags;
    return sys_call(disk_port, &m);
}
static void disk_unmount(int h)
{
    struct umsg m;
    m.type = DISK_UNMOUNT; m.a[0] = (u32)h;
    sys_call(disk_port, &m);
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

/* the root filesystem: a root image the boot floppy carried (the dev/test image) wins; otherwise the first partition
 * of type ext2 (RDB 'LNX\0', MBR 0x83) that holds a writable ext2 volume Zenix fully supports (a refused or read-only
 * one - ext3, ext4, errors - is never the root), else the bare disk. */
static int find_root(void)
{
    struct diskinfo di;
    if (!try_mount("ram0", EXT2_RW)) return 0;        /* only exists when the floppy carried a root image */
    for (u32 i = 1; i < 16; i++) {                    /* 0 is the whole disk */
        struct umsg m;
        m.type = DISK_LIST; m.a[0] = i; m.a[1] = (u32)&di;
        if (sys_call(disk_port, &m) < 0) break;
        if (di.dostype != DT_EXT2) continue;          /* ram0 and partitions of other types */
        if (!try_mount(di.name, EXT2_RW)) return 0;
    }
    return try_mount("ide0", EXT2_RW);
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
    if (fr < 0) return -23;                           /* ENFILE: should not happen (the kernel has fewer files) */
    opens[fr].ino = ino; opens[fr].count = 1;
    return 0;
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

static int handle1(struct umsg *m)
{
    struct inode in, dir, dir2;
    char name[64], name2[64];
    switch (m->type) {
    case FS_OPEN: {
        const char *path = (const char *)m->a[0];
        u32 flags = m->a[2];
        int writing = (flags & (O_WRONLY | O_RDWR)) != 0;
        if (ext2_namei(path, m->a[1], &in, 0)) {
            if (ext2_denied || !(flags & O_CREAT)) return -1;
            if (ext2_readonly) return -30;
            if (parent(path, m->a[1], &dir, name) || !dir_may(&dir) || ext2_create(&dir, name, &in))
                return -1;
        } else {
            if (!ext2_access(&in, fsp_open_want(flags))) return -1;
            if (writing) {
                if (ext2_readonly) return -30;
                if (!S_ISREG(in.mode)) return -1;
                if ((flags & O_TRUNC) && ext2_truncate(&in)) return -1;
            }
        }
        int e = open_ref(in.ino);
        if (e) return e;
        fill_stat(&in, (struct ustat *)m->a[3]);
        return (int)in.ino;
    }
    case FS_READ:
        if (ext2_iget(m->a[0], &in)) return -1;
        return ext2_read(&in, m->a[1], (void *)m->a[2], m->a[3]);
    case FS_WRITE:
        if (ext2_iget(m->a[0], &in)) return -1;
        return ext2_write(&in, m->a[1], (const void *)m->a[2], m->a[3]);
    case FS_STAT:
        if (ext2_namei((const char *)m->a[0], m->a[1], &in, 0)) return -1;
        fill_stat(&in, (struct ustat *)m->a[2]);
        return 0;
    case FS_FSTAT:
        if (ext2_iget(m->a[0], &in)) return -1;
        fill_stat(&in, (struct ustat *)m->a[1]);
        return 0;
    case FS_READDIR: {
        u32 cookie = m->a[1];
        if (ext2_iget(m->a[0], &in) || !S_ISDIR(in.mode)) return -1;
        int r = ext2_readdir(&in, &cookie, (struct udirent *)m->a[2]);
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
        return 0;                                     /* writes go straight to the disk */
    case FS_CHMOD:
        if (ext2_namei((const char *)m->a[0], m->a[1], &in, 0) || !owner_only(&in)) return -1;
        return ext2_chmod(in.ino, m->a[2]);
    case FS_CHOWN:
        if (ext2_namei((const char *)m->a[0], m->a[1], &in, 0)) return -1;
        if (ext2_enforce && !fsp_is_root(ext2_uid)) { ext2_denied = 1; return -1; }
        return ext2_chown(in.ino, m->a[2], m->a[3]);
    case FS_UTIMES:
        if (ext2_namei((const char *)m->a[0], m->a[1], &in, 0)) return -1;
        if (!owner_only(&in) && !(ext2_denied = 0, ext2_access(&in, 2))) return -1;
        return ext2_utimes(in.ino, m->a[2]);
    case FS_LSTAT:
        if (ext2_namei_ex((const char *)m->a[0], m->a[1], &in, 0, 0)) return -1;
        fill_stat(&in, (struct ustat *)m->a[2]);
        return 0;
    case FS_LINK:
        if (ext2_namei((const char *)m->a[0], m->a[2], &in, 0) || S_ISDIR(in.mode)) return -1;
        if (parent((const char *)m->a[1], m->a[2], &dir, name) || !dir_may(&dir)) return -1;
        return ext2_hardlink(&dir, name, in.ino);
    case FS_SYMLINK:
        if (parent((const char *)m->a[1], m->a[2], &dir, name) || !dir_may(&dir)) return -1;
        return ext2_symlink(&dir, name, (const char *)m->a[0]);
    case FS_READLINK:
        if (ext2_namei_ex((const char *)m->a[0], m->a[1], &in, 0, 0) || !S_ISLNK(in.mode)) return -1;
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
        if (ext2_namei((const char *)m->a[0], m->a[1], &in, 0) || !S_ISDIR(in.mode) || !ext2_access(&in, 1)) return -1;
        return (int)in.ino;
    case FS_MKDIR:
        if (parent((const char *)m->a[0], m->a[1], &dir, name) || !dir_may(&dir)) return -1;
        return ext2_mkdir(&dir, name);
    case FS_UNLINK:
        if (parent((const char *)m->a[0], m->a[1], &dir, name) || !dir_may(&dir) || !sticky_ok(&dir, name)) return -1;
        return ext2_unlink(&dir, name);
    case FS_RMDIR:
        if (parent((const char *)m->a[0], m->a[1], &dir, name) || !dir_may(&dir) || !sticky_ok(&dir, name)) return -1;
        return ext2_rmdir(&dir, name);
    case FS_RENAME:
        if (parent((const char *)m->a[0], m->a[2], &dir, name) ||
            parent((const char *)m->a[1], m->a[2], &dir2, name2) ||
            !dir_may(&dir) || !dir_may(&dir2) || !sticky_ok(&dir, name) || !sticky_ok(&dir2, name2)) return -1;
        return ext2_rename(&dir, name, &dir2, name2);
    }
    return -1;
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
    if (ext2_readonly && changes(m)) return -30;      /* EROFS */
    ext2_uid = m->uid; ext2_gid = m->gid; ext2_umask = m->umask;
    ext2_enforce = 1; ext2_denied = 0;
    int r = handle1(m);
    return r < 0 && ext2_denied ? -13 : r;
}

/* fsd                      the root filesystem server (built into the kernel): finds the root device, port "fs"
 * fsd DEVICE PORTNAME       a second ext2 filesystem on DEVICE, served on port PORTNAME (started by mount) */
int main(int argc, char **argv)
{
    const char *pname = "fs";
    ext2_busy = open_busy;
    int tries = 0;
    while ((disk_port = sys_port_find("disk")) < 0) {
        if (++tries > 600) { printf("fsd: no disk server\n"); return 1; }
        sys_sleep(5);
    }
    if (argc >= 3) {
        pname = argv[2];
        if (try_mount(argv[1], EXT2_ANY)) { printf("mount.ext2: %s is not an ext2 filesystem Zenix can mount, or is in use\n", argv[1]); return 1; }
    } else {
        struct rootpick rp;
        rootpick(disk_port, &rp);
        if (!rp.have_ram && rp.found && rp.dostype == DT_ZENIXFS) return 0;   /* the root is ZenixFS: mount.zenixfs serves it */
        if (find_root()) { printf("fsd: no ext2 filesystem\n"); return 1; }
    }
    int port = sys_port_create(pname);
    if (port < 0) { printf("fsd: cannot create the port %s\n", pname); return 1; }
    struct umsg *m = sys_recv(port, 1);
    while ((int)m != -1) {
        int r = handle(m);
        if (quitting) {                                 /* umount: answer, then let go of the device */
            for (int i = 0; i < NOPEN; i++)             /* nothing is open at umount; deletions still waiting are done */
                if (opens[i].count && !ext2_readonly) { opens[i].count = 0; ext2_release(opens[i].ino); }
            sys_reply(m, r);
            disk_unmount(fs_dev);
            return 0;
        }
        m = sys_replyrecv(m, r, port);                  /* reply and wait in one trap */
    }
    return 1;
}
