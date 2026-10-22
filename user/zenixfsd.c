/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/fsproto.h"
#include "../kernel/diskproto.h"
#include "zenixfs_disk.h"

/* mount.zenixfs DEVICE PORTNAME: the ZenixFS filesystem server (docs/zenixfs.md). Speaks the same protocol as the ext2
 * server (kernel/fsproto.h) on port PORTNAME; started by mount(8). Files are named by ZenixFS inode number.
 * The library keeps changes in memory and commits them atomically: here every 3 seconds after the first change, on
 * sync(8), and at umount. A power cut loses at most the last few seconds and never leaves a damaged volume.
 * The permission rules are the ones of the ext2 server (docs/architecture.md section 14). */

#define ROOT ZENIXFS_ROOT_INO
#define COMMIT_TICKS 300                          /* 3 s at 100 Hz */

static struct zenixfs *fs;
static struct zenixfs_dev zdev;
static unsigned cuid, cgid, cumask;               /* the caller of the request being handled */
static int denied, quitting, dirty;
static u32 dirty_since;
static char devname[16];

/* ---- permissions ---- */
static int access_ok(const struct zenixfs_inode *in, int want)       /* want: 4 read, 2 write, 1 execute */
{
    int ok;
    if (cuid == 0) ok = !(want & 1) || (in->mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR || (in->mode & 0111);
    else {
        int sh = cuid == in->uid ? 6 : cgid == in->gid ? 3 : 0;
        ok = (((in->mode >> sh) & 7) & (unsigned)want) == (unsigned)want;
    }
    if (!ok) denied = 1;
    return ok;
}
static int owner_only(const struct zenixfs_inode *in)
{
    if (cuid == 0 || cuid == in->uid) return 1;
    denied = 1;
    return 0;
}
static int is_dir(const struct zenixfs_inode *in) { return (in->mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR; }
static int is_reg(const struct zenixfs_inode *in) { return (in->mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFREG; }
static int is_lnk(const struct zenixfs_inode *in) { return (in->mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFLNK; }

/* ---- paths ---- */
/* resolve PATH starting at directory CWD (or the root if it begins with /); symbolic links are followed (not the last one
 * unless follow_last). Every directory searched needs x. Returns 0 and the inode, or a negative number. */
static int namei(uint32_t cwd, const char *path, int follow_last, int depth, uint32_t *out)
{
    uint32_t cur = path[0] == '/' ? ROOT : cwd;
    const char *p = path;
    if (depth > 8) return -1;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) { *out = cur; return 0; }
        const char *e = p;
        while (*e && *e != '/') e++;
        const char *q = e;
        while (*q == '/') q++;
        int last = !*q;
        struct zenixfs_inode dn;
        if (zenixfs_getattr(fs, cur, &dn) || !is_dir(&dn)) return -1;
        if (!access_ok(&dn, 1)) return -13;
        uint32_t ino;
        if (zenixfs_lookup(fs, cur, p, (int)(e - p), &ino)) return -1;
        struct zenixfs_inode in;
        if (zenixfs_getattr(fs, ino, &in)) return -1;
        if (is_lnk(&in) && (!last || follow_last)) {
            char target[256];
            int n = zenixfs_readlink(fs, ino, target, 255);
            if (n < 0) return -1;
            target[n] = 0;
            uint32_t t;
            int r = namei(cur, target, 1, depth + 1, &t);
            if (r) return r;
            ino = t;
        }
        if (last) { *out = ino; return 0; }
        cur = ino;
        p = q;
    }
}

/* the directory that holds the last component of PATH, and that last name */
static int parent(uint32_t cwd, const char *path, uint32_t *dir, const char **name, int *len)
{
    int n = (int)strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    int s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    if (n - s <= 0) return -1;
    if ((n - s == 1 && path[s] == '.') || (n - s == 2 && path[s] == '.' && path[s + 1] == '.')) return -1;
    char dpath[256];
    if (s > 255) return -1;
    memcpy(dpath, path, (u32)s);
    dpath[s] = 0;
    if (s == 0) { dpath[0] = '.'; dpath[1] = 0; }
    *name = path + s;
    *len = n - s;
    return namei(cwd, dpath, 1, 0, dir);
}

static int dir_may(uint32_t dir)                  /* add or remove entries: w and x */
{
    struct zenixfs_inode d;
    if (zenixfs_getattr(fs, dir, &d)) return 0;
    return access_ok(&d, 3);
}
static int sticky_ok(uint32_t dir, const char *name, int len)    /* in a sticky directory only owners may remove */
{
    struct zenixfs_inode d, t;
    uint32_t ino;
    if (zenixfs_getattr(fs, dir, &d)) return 0;
    if (cuid == 0 || !(d.mode & 01000) || cuid == d.uid) return 1;
    if (zenixfs_lookup(fs, dir, name, len, &ino) || zenixfs_getattr(fs, ino, &t)) return 1;
    if (t.uid == cuid) return 1;
    denied = 1;
    return 0;
}

static void fill_stat(uint32_t ino, const struct zenixfs_inode *in, struct ustat *st)
{
    st->ino = ino; st->mode = in->mode; st->size = in->size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)in->size;
    st->mtime = (u32)in->mtime; st->uid = in->uid; st->gid = in->gid; st->links = in->nlink;
}

static void touched(void)
{
    if (!dirty) { dirty = 1; dirty_since = sys_ticks(); }
}

static int handle1(struct umsg *m)
{
    struct zenixfs_inode in;
    uint32_t ino, dir, dir2;
    const char *name, *name2;
    int len, len2, r;
    switch (m->type) {
    case FS_OPEN: {
        const char *path = (const char *)m->a[0];
        u32 flags = m->a[2];
        int writing = (flags & (O_WRONLY | O_RDWR)) != 0;
        r = namei(m->a[1], path, 1, 0, &ino);
        if (r) {
            if (denied || !(flags & O_CREAT) || parent(m->a[1], path, &dir, &name, &len) || !dir_may(dir)) return -1;
            if (zenixfs_create(fs, dir, name, len, 0666 & ~cumask, cuid, cgid, &ino)) return -1;
            touched();
        } else {
            if (zenixfs_getattr(fs, ino, &in)) return -1;
            int want = ((flags & O_WRONLY) ? 2 : (flags & O_RDWR) ? 6 : 4) | ((flags & O_EXEC) ? 1 : 0);
            if (!access_ok(&in, want)) return -1;
            if (writing) {
                if (!is_reg(&in)) return -1;
                if (flags & O_TRUNC) {
                    if (zenixfs_truncate(fs, ino, 0)) return -1;
                    touched();
                }
            }
        }
        if (zenixfs_getattr(fs, ino, &in)) return -1;
        fill_stat(ino, &in, (struct ustat *)m->a[3]);
        return (int)ino;
    }
    case FS_READ:
        r = zenixfs_read(fs, m->a[0], m->a[1], (void *)m->a[2], m->a[3]);
        return r < 0 ? -1 : r;
    case FS_WRITE:
        r = zenixfs_write(fs, m->a[0], m->a[1], (const void *)m->a[2], m->a[3]);
        touched();
        return r < 0 ? -1 : r;
    case FS_STAT: case FS_LSTAT:
        if (namei(m->a[1], (const char *)m->a[0], m->type == FS_STAT, 0, &ino) || zenixfs_getattr(fs, ino, &in)) return -1;
        fill_stat(ino, &in, (struct ustat *)m->a[2]);
        return 0;
    case FS_FSTAT:
        if (zenixfs_getattr(fs, m->a[0], &in)) return -1;
        fill_stat(m->a[0], &in, (struct ustat *)m->a[1]);
        return 0;
    case FS_READDIR: {
        struct udirent *ud = (struct udirent *)m->a[2];
        uint64_t cookie = m->a[1];
        char nm[300]; uint8_t type;
        for (;;) {
            r = zenixfs_readdir(fs, m->a[0], &cookie, nm, &ino, &type);
            if (r < 0) return -1;
            if (r == 0) break;
            if (strlen(nm) < sizeof ud->name) {         /* longer names (from other systems' volumes) do not fit a udirent */
                ud->ino = ino; ud->type = type;
                strcpy(ud->name, nm);
                break;
            }
        }
        *(u32 *)m->a[3] = (u32)cookie;
        return r;
    }
    case FS_STATFS: {
        struct zenixfs_statfs sf;
        struct ustatfs *f = (struct ustatfs *)m->a[0];
        zenixfs_statfs(fs, &sf);
        f->bsize = sf.bsize; f->blocks = sf.blocks; f->bfree = sf.bfree; f->files = sf.files; f->ffree = 1u << 24;
        return 0;
    }
    case FS_SYNC:
        r = zenixfs_sync(fs);
        dirty = 0;
        return r ? -1 : 0;
    case FS_CHMOD: case FS_UTIMES: {
        if (namei(m->a[1], (const char *)m->a[0], 1, 0, &ino) || zenixfs_getattr(fs, ino, &in) || !owner_only(&in)) return -1;
        if (m->type == FS_CHMOD) in.mode = (in.mode & ZENIXFS_S_IFMT) | (m->a[2] & 07777);
        else in.mtime = m->a[2];
        touched();
        return zenixfs_setattr(fs, ino, &in) ? -1 : 0;
    }
    case FS_CHOWN:
        if (cuid != 0) { denied = 1; return -1; }
        if (namei(m->a[1], (const char *)m->a[0], 1, 0, &ino) || zenixfs_getattr(fs, ino, &in)) return -1;
        if (m->a[2] != 0xFFFFFFFFu) in.uid = m->a[2];
        if (m->a[3] != 0xFFFFFFFFu) in.gid = m->a[3];
        touched();
        return zenixfs_setattr(fs, ino, &in) ? -1 : 0;
    case FS_LINK:
        if (namei(m->a[2], (const char *)m->a[0], 1, 0, &ino) || zenixfs_getattr(fs, ino, &in) || is_dir(&in)) return -1;
        if (parent(m->a[2], (const char *)m->a[1], &dir, &name, &len) || !dir_may(dir)) return -1;
        touched();
        return zenixfs_link(fs, dir, name, len, ino) ? -1 : 0;
    case FS_SYMLINK:
        if (parent(m->a[2], (const char *)m->a[1], &dir, &name, &len) || !dir_may(dir)) return -1;
        touched();
        return zenixfs_symlink(fs, dir, name, len, (const char *)m->a[0], cuid, cgid, &ino) ? -1 : 0;
    case FS_READLINK:
        if (namei(m->a[1], (const char *)m->a[0], 0, 0, &ino) || zenixfs_getattr(fs, ino, &in) || !is_lnk(&in)) return -1;
        r = zenixfs_readlink(fs, ino, (char *)m->a[2], (int)m->a[3]);
        return r < 0 ? -1 : r;
    case FS_ROOT: {
        struct fsinfo *fi = (struct fsinfo *)m->a[0];
        strncpy(fi->dev, devname, sizeof fi->dev - 1);
        strcpy(fi->type, "zenixfs");
        return (int)ROOT;
    }
    case FS_QUIT:
        quitting = 1;
        return 0;
    case FS_CHDIR:
        if (namei(m->a[1], (const char *)m->a[0], 1, 0, &ino) || zenixfs_getattr(fs, ino, &in) || !is_dir(&in)) return -1;
        if (!access_ok(&in, 1)) return -1;
        return (int)ino;
    case FS_MKDIR:
        if (parent(m->a[1], (const char *)m->a[0], &dir, &name, &len) || !dir_may(dir)) return -1;
        touched();
        return zenixfs_mkdir(fs, dir, name, len, 0777 & ~cumask, cuid, cgid, &ino) ? -1 : 0;
    case FS_UNLINK:
        if (parent(m->a[1], (const char *)m->a[0], &dir, &name, &len) || !dir_may(dir) || !sticky_ok(dir, name, len)) return -1;
        touched();
        return zenixfs_unlink(fs, dir, name, len) ? -1 : 0;
    case FS_RMDIR:
        if (parent(m->a[1], (const char *)m->a[0], &dir, &name, &len) || !dir_may(dir) || !sticky_ok(dir, name, len)) return -1;
        touched();
        return zenixfs_rmdir(fs, dir, name, len) ? -1 : 0;
    case FS_RENAME:
        if (parent(m->a[2], (const char *)m->a[0], &dir, &name, &len) ||
            parent(m->a[2], (const char *)m->a[1], &dir2, &name2, &len2) ||
            !dir_may(dir) || !dir_may(dir2) || !sticky_ok(dir, name, len) || !sticky_ok(dir2, name2, len2)) return -1;
        touched();
        return zenixfs_rename(fs, dir, name, len, dir2, name2, len2) ? -1 : 0;
    }
    return -1;
}

static int handle(struct umsg *m)
{
    cuid = m->uid; cgid = m->gid; cumask = m->umask;
    denied = 0;
    zenixfs_set_time(fs, sys_time());
    int r = handle1(m);
    return r < 0 && denied ? -13 : r;
}

static int try_mount(const char *name)
{
    if (zdisk_open(name, DISK_MOUNT) < 0) return -1;
    for (unsigned bs = 10; bs <= 12; bs++) {
        zdisk_dev(&zdev, bs);
        if (zenixfs_mount(&fs, &zdev, 48) == 0) { strncpy(devname, name, sizeof devname - 1); return 0; }
        fs = 0;
    }
    zdisk_close();
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: mount.zenixfs device portname   (started by mount)\n"); return 2; }
    int tries = 0;
    while (sys_port_find("disk") < 0) {
        if (++tries > 600) { printf("mount.zenixfs: no disk server\n"); return 1; }
        sys_sleep(5);
    }
    if (try_mount(argv[1])) { printf("mount.zenixfs: %s is not a ZenixFS volume, or is in use\n", argv[1]); return 1; }
    struct zenixfs_stats st;
    zenixfs_get_stats(fs, &st);
    if (st.mount_damage)
        printf("mount.zenixfs: %s: %u commit record(s) were unreadable; mounted from the last good commit\n", argv[1], st.mount_damage);
    int port = sys_port_create(argv[2]);
    if (port < 0) { printf("mount.zenixfs: cannot create the port %s\n", argv[2]); return 1; }
    struct umsg *m = sys_recv(port, 1);
    while ((int)m != -1) {
        int r = handle(m);
        if (quitting) {                                 /* umount: commit, answer, let go of the device */
            zenixfs_unmount(fs);
            sys_reply(m, r);
            zdisk_close();
            return 0;
        }
        if (!dirty) {
            m = sys_replyrecv(m, r, port);              /* nothing to write out: reply and wait */
            continue;
        }
        sys_reply(m, r);
        for (;;) {                                      /* changes are waiting: wait for the next request, but commit on time */
            m = sys_recv(port, 0);
            if (m) break;                               /* a request, or -1: the port is gone */
            if (sys_ticks() - dirty_since >= COMMIT_TICKS) {
                zenixfs_sync(fs);
                dirty = 0;
                m = sys_recv(port, 1);
                break;
            }
            sys_sleep(10);
        }
    }
    zenixfs_unmount(fs);
    return 1;
}
