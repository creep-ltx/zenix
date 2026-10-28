/* SPDX-License-Identifier: GPL-2.0-only */
#pragma GCC optimize("Os")              /* built into the kernel image: size first (ext2.c is -Os too) */
#include "fsenv.h"
#include "../kernel/fsproto.h"
#include "../kernel/diskproto.h"
#include "diskcall.h"
#include "volid.h"

/* The filesystem server: kernel/ext2.c in user mode behind the "fs" port.
 * Embedded in the kernel image (the kernel needs it before any disk is
 * readable); the kernel's VFS layer talks to it with messages. */

static int disk_port, fs_dev = -1;
static char devname[16];
static int quitting;
/* The clean mark (DESIGN D17, docs/d17-ext2-clean-mark.md): bit 0 of the superblock's state is cleared on the disk
 * (and flushed) before the first change after a clean moment, and set again 1 s after the last change, at FS_SYNC and
 * at FS_QUIT - only between requests, with no orphan open, and when nothing keeps the volume marked. Between two
 * requests the volume is consistent (write-through, the F5 write order), so the bit never says clean over a half-done
 * change; a failed write or damage met (a request that may have stopped half way) is a need for a check. */
static int marked;                      /* bit 0 is clear on the disk */
static int need;                        /* it needs a check (sticky): mounted not clean, a crash in a request, a
                                         * failed write, damage met */
static int hold;                        /* zup's FS_MARK 1, until its FS_MARK 0 */
static int norphans;                    /* open inodes whose last name went (kept until their last close) */
static u32 changed_at;                  /* the tick of the last writing request */
#define CACHE_CHIPONLY (24 * 1024)      /* the block cache on a machine without fast RAM (DESIGN D18: I/O measured) */
#define QUIET_TICKS 100                 /* 1 s after the last change: clean again (AmigaOS FFS's bitmap timer) */
#ifdef ZHOST
int fsd_marking;                        /* host tests: a block write outside the mark is counted */
unsigned long fsd_unmarked_writes;
#endif

static int disk_rw(u32 op, u32 lba, u32 count, const void *buf)
{
    struct umsg m;
    m.type = op; m.a[0] = (u32)fs_dev; m.a[1] = lba; m.a[2] = count; m.a[3] = (u32)buf;
    return disk_call(disk_port, &m);
}
int fs_blk_read(u32 lba, u32 count, void *buf) { return disk_rw(DISK_READ, lba, count, buf); }
int fs_blk_write(u32 lba, u32 count, const void *buf)
{
#ifdef ZHOST
    if (!marked && !fsd_marking) fsd_unmarked_writes++;
#endif
    int r = disk_rw(DISK_WRITE, lba, count, buf);
    if (r) need = 1;                    /* a request may have stopped half way */
    return r;
}

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
 * mode EXT2_RW: refuse a volume Zenix could only read (the root must be writable); EXT2_ANY: read-only is allowed.
 * share: served read-only on the plain handle, so fsck.ext2 may open the device raw (the root's check, D17). */
static int try_mount(const char *name, int mode, int share)
{
    fs_dev = disk_open(name, 0);
    if (fs_dev < 0 || !ext2_probe()) return -1;
    if (ext2_mount_as(mode)) {
        printf("fsd: %s: %s\n", name, ext2_why);
        return -1;
    }
    strncpy(devname, name, sizeof devname - 1);
    need = marked = !ext2_readonly && !share && !ext2_clean;   /* not clean already: it needs a check (D17) */
    if (share) { ext2_readonly = 1; return 0; }
    int h = disk_open(name, DISK_MOUNT);
    if (h < 0) return -1;
    fs_dev = h;
    return 0;
}

/* Open inodes (FS_OPEN counts one, FS_CLOSE from the kernel one less): an inode whose last name goes while it is
 * open keeps its blocks until the last close (ext2_busy / ext2_release), so an old handle never reaches a file that
 * reused the number. After a crash such an inode is left with no links: fsck.ext2 (or e2fsck) frees it. */
#define NOPEN 96                                      /* the kernel's file table (NFILE 64) plus programs being loaded */
static struct { u32 ino, count; } opens[NOPEN];
#define ORPHAN 0x80000000u                            /* in count: its last name went while it was open (D17) */
/* ext2.c asks this exactly when an inode loses its last name: an open one is kept and becomes an orphan */
static int open_busy(u32 ino)
{
    for (int i = 0; i < NOPEN; i++)
        if (opens[i].count && opens[i].ino == ino) {
            if (!(opens[i].count & ORPHAN)) { opens[i].count |= ORPHAN; norphans++; }
            return 1;
        }
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
            if (--opens[i].count & ~ORPHAN) return;
            if (opens[i].count) norphans--;
            opens[i].count = 0;
            if (!ext2_readonly) ext2_release(ino);  /* frees it only if it has no names */
            return;
        }
}

/* the clean bit written and made durable: 0, or -EIO */
static int set_bit(int clean)
{
#ifdef ZHOST
    fsd_marking = 1;
#endif
    int r = ext2_set_clean(clean);
#ifdef ZHOST
    fsd_marking = 0;
#endif
    return r < 0 || disk_flush() < 0 ? -EIO : 0;
}
static int mark(void)                   /* before a change: "not clean" on the disk first */
{
    if (!marked) {
        if (set_bit(0)) return -EIO;
        marked = 1;
    }
    return 0;
}
static int cleanable(void) { return marked && !need && !hold && !norphans && !ext2_ndamage; }
static void quiet(void)                 /* between requests: clean again, if nothing keeps it marked */
{
    if (!cleanable()) return;
    if (disk_flush() < 0 || set_bit(1)) need = 1;       /* the disk failed: it stays marked */
    else marked = 0;
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
            if (!(r = ext2_namei_ex(path, m->a[1], &in, 0, 0))) return -EEXIST;
            if (r == -FS_LINKED) return r;
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
    case FS_SYNC:                                     /* writes went straight to the disk; the drive's cache is left */
        r = disk_flush();
        quiet();                                      /* reboot and the reset warning sync: the volume clean again */
        return r < 0 ? -EIO : 0;
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
    case FS_MARK:                                       /* root only (handle): zup's "needs a check" mark (D15) */
        r = !need && !hold && !ext2_ndamage;            /* the state before: did it need a check (D17 5) */
        hold = m->a[0] != 0;                            /* (never clears need) */
        if (hold && mark()) return -EIO;
        quiet();
        return r;
    }
    return -ENOSYS;
}

/* requests that change the volume: refused at once on a read-only mount (ext2.c also refuses every block write).
 * Those, FS_OPEN with O_CREAT or O_TRUNC and a close while an orphan is open may write: the mark comes first. */
static int changes(const struct umsg *m)
{
    switch (m->type) {
    case FS_WRITE: case FS_CHMOD: case FS_CHOWN: case FS_UTIMES: case FS_LINK: case FS_SYMLINK:
    case FS_MKDIR: case FS_UNLINK: case FS_RMDIR: case FS_RENAME: case FS_MARK: return 1;
    }
    return 0;
}

static int writes(const struct umsg *m)
{
    if (m->type == FS_OPEN) return (m->a[2] & (O_CREAT | O_TRUNC)) != 0;
    if (m->type == FS_CLOSE) return norphans;
    return m->type != FS_MARK && changes(m);
}

/* DESIGN D26: the walks a request that changes something makes, without changing anything: -FS_LINKED if one meets a
 * link to follow (the kernel's UMSG_LINKS), else anything (handle1 walks again and gives its own answer) */
static int prewalk(struct umsg *m)
{
    struct inode in;
    char name[64];
    const char *a0 = (const char *)m->a[0], *a1 = (const char *)m->a[1];
    int r = 0;
    switch (m->type) {
    case FS_OPEN:
        return (m->a[2] & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL) ? ext2_namei_ex(a0, m->a[1], &in, 0, 0)
                                                                     : ext2_namei(a0, m->a[1], &in, 0);
    case FS_CHMOD: case FS_CHOWN: case FS_UTIMES:
        return ext2_namei(a0, m->a[1], &in, 0);
    case FS_MKDIR: case FS_UNLINK: case FS_RMDIR:
        return parent(a0, m->a[1], &in, name);
    case FS_LINK:
        r = ext2_namei(a0, m->a[2], &in, 0);
        break;
    case FS_RENAME:
        r = parent(a0, m->a[2], &in, name);
        break;
    case FS_SYMLINK:
        break;
    default:
        return 0;
    }
    return r ? r : parent(a1, m->a[2], &in, name);
}

/* the FS_LINKED answer (kernel/fsproto.h): the continuation written over the path argument the walk stopped in */
static int linked(struct umsg *m)
{
    int a1 = (m->type == FS_RENAME || m->type == FS_LINK || m->type == FS_SYMLINK) && ext2_lk_path == (const char *)m->a[1];
    char *p = (char *)m->a[a1];
    struct inode l;
    char t[256], out[FS_PATHMAX];
    int r = ext2_iget(ext2_lk_ino, &l);
    if (r) return r;
    if (l.size > 255) return -ENAMETOOLONG;
    int n = ext2_readlink(&l, t, 255);
    if (n < 0) return n;
    if ((r = fsl_text(out, p, ext2_lk_s, ext2_lk_e, t, (unsigned)n))) return r;
    u32 e = ext2_lk_e;
    while (p[e] == '/') e++;
    int last = m->type == FS_OPEN && !p[e];             /* the link was the last name of the original path */
    strcpy(p, out);
    return -(FS_LINKED + (a1 ? FSL_A1 : 0) + (last ? FSL_LAST : 0));
}

static int handle(struct umsg *m)
{
    if (!(m->kflags & UMSG_KERNEL) && m->uid != 0) return -EACCES; /* raw IPC from a user: inode numbers would bypass every check */
    ext2_uid = m->uid; ext2_gid = m->gid; ext2_umask = m->umask;
    ext2_enforce = 1; ext2_denied = 0;
    ext2_stoplinks = (m->kflags & UMSG_LINKS) != 0;
    int r;
    /* D26: the answer "this path continues elsewhere" comes before the read-only refusal and the clean mark */
    if (ext2_stoplinks && (ext2_readonly ? changes(m) : writes(m)) && (r = prewalk(m)) == -FS_LINKED)
        goto out;
    ext2_denied = 0;
    if (ext2_readonly && changes(m)) return -EROFS;
    if (!ext2_readonly && writes(m)) {
        if (mark()) return -EIO;                        /* the mark could not be written: nothing is changed */
        changed_at = sys_ticks();
    }
    r = handle1(m);
out:
    if (r == -FS_LINKED)
        return linked(m);
    return r < 0 && ext2_denied ? -EACCES : r;
}

/* fsd DEVICE PORTNAME [-root|-fsck] [-id ID]   the ext2 filesystem on DEVICE, served on port PORTNAME. Built into the
 * kernel and started as a service (DESIGN D11): boot registers the root's ("fs", -root: it must be writable, as the root
 * is - a refused or read-only volume, ext3, ext4, errors, is never the root); mount registers a mount's ("fs.DEVICE").
 * -fsck: the root's rules, but served read-only without taking the device, while boot runs fsck.ext2 on it (D17). */
/* the block cache by the machine (the 2 MB diet, DESIGN D18): no fast RAM, 24 KB; with fast RAM 48 blocks, as before */
static void cache_by_memory(void)
{
    u32 mi[4];
    if (sys_meminfo(mi) == 0 && mi[1] == 0)
        ext2_cache_bytes = CACHE_CHIPONLY;
}

int main(int argc, char **argv)
{
    ext2_busy = open_busy;
    disk_port = sys_port_find("disk");                /* a registered service: calling it starts diskd if need be */
    int share = argc > 3 && !strcmp(argv[3], "-fsck"), root = share || (argc > 3 && !strcmp(argv[3], "-root"));
    if (argc < 3) { printf("usage: fsd DEVICE PORT [-root|-fsck] [-id ID]\n"); return 2; }
    const char *pname = argv[2], *dev = argv[1], *who = root ? "fsd" : pname;
    char name[16];
    int self = sys_svc_self(), restart = (self & 0xFFFF) > 1;
    cache_by_memory();
    if (argc > 4 && !strcmp(argv[argc - 2], "-id")) {      /* the volume by its place and uuid (volid.h) */
        if (vol_find(disk_port, dev, argv[argc - 1], name, who, restart)) return 1;
        dev = name;
    }
    if (try_mount(dev, root ? EXT2_RW : EXT2_ANY, share)) {
        if (root) printf("fsd: no writable ext2 filesystem on %s for the root\n", dev);
        else printf("%s: %s is not an ext2 filesystem Zenix can mount, or is in use\n", who, dev);
        return 1;
    }
    if ((self & SVC_MIDREQ) && !ext2_readonly) {        /* ext2 has no journal: the change may be half done (D11 5.4) */
        int e = ext2_mark_unclean();
        need = marked = 1;
        printf("%s: %s was being changed when its server ended: check it with fsck.ext2\n", who, dev);
        if (e < 0)
            printf("%s: %s: could not mark it \"needs a check\" on the disk (error %d): run fsck.ext2 by hand\n", who, dev, e);
    }
    int port = sys_port_create(pname);
    if (port < 0) { printf("%s: cannot create the port %s\n", who, pname); return 1; }
    if (restart) {                                      /* the inodes the kernel holds open on us (D11 5.4) */
        static u32 ino[160];
        int n = sys_svc_opens(ino, sizeof ino);
        for (int i = 0; i < n; i++) {
            struct inode t;
            open_ref(ino[i]);
            if (!ext2_iget(ino[i], &t) && !t.links) open_busy(ino[i]);  /* an orphan of the server before */
        }
        if (n < 0) printf("%s: %s: restart: the open files are unknown (%d): space of deleted open files stays used until fsck.ext2\n", who, dev, n);
    }
    struct umsg *m = sys_recv(port, 1);
    while ((int)m != -1) {
        int r = handle(m);
        if (quitting) {                                 /* umount: flush, mark clean, answer, then let go of the device */
            for (int i = 0; i < NOPEN; i++)             /* nothing is open at umount; deletions still waiting are done */
                if ((opens[i].count & ORPHAN) && !mark()) {  /* (only a writable volume has orphans) */
                    opens[i].count = 0;
                    norphans--;
                    ext2_release(opens[i].ino);
                }
            disk_flush();
            quiet();
            sys_reply(m, r);
            disk_unmount(fs_dev);
            return 0;
        }
        u32 age = sys_ticks() - changed_at;
        if (cleanable() && age >= QUIET_TICKS)          /* 1 s after the last change, also under a stream of reads */
            quiet();
        if (!cleanable()) {
            m = sys_replyrecv(m, r, port);              /* reply and wait in one trap */
            continue;
        }
        sys_reply(m, r);                                /* marked by us: the next request, or the clean moment */
        m = sys_recv(port, 1 + QUIET_TICKS - age);
        if (!m) {
            quiet();
            m = sys_recv(port, 1);
        }
    }
    return 1;
}
