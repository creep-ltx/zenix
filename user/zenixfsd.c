/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef ZHOST
#include "../tools/host/zhost.h"           /* the host tests (tools/host/fsd-host.c built with -DZFSD) */
#else
#include "libz.h"
#endif
#include "../kernel/fsproto.h"
#include "../kernel/diskproto.h"
#include "../kernel/fsperm.h"
#include "rdb.h"
#include "zenixfs_disk.h"
#include "volid.h"

/* zfsd DEVICE [PORTNAME] [-root|-fsck] [-id ID]: the ZenixFS filesystem server (docs/zenixfs.md), started as a service
 * (DESIGN D11): the root's ("fs", built into the ZenixFS kernel, DESIGN D34) registered by boot, a mount's ("fs.DEVICE",
 * the file /sbin/zfsd) by mount(8). -fsck: served read-only without taking the device, while the repair tool works on it
 * (the same word as fsd's, D17). Speaks the same protocol as the ext2 server (kernel/fsproto.h) on port PORTNAME. Files are
 * named by ZenixFS inode number.
 * The library keeps changes in memory and commits them atomically: here every 3 seconds after the first change, on
 * sync(8), and at umount. A power cut loses at most the last few seconds and never leaves a damaged volume.
 * The permission rules are the shared ones of kernel/fsperm.h, the same as in the ext2 server. */

#define ROOT ZENIXFS_ROOT_INO
#define COMMIT_TICKS 300                          /* 3 s at 100 Hz */

static struct zenixfs *fs;
static struct zenixfs_dev zdev;
static unsigned cuid, cgid, cumask;               /* the caller of the request being handled */
static unsigned cng;                              /* and its supplementary groups (UMSG_GROUPS) */
static unsigned short cgs[NGROUPS];
static int denied, quitting, dirty;
static u32 dirty_since;
static char devname[16];

/* ---- permissions (kernel/fsperm.h) ---- */
static int access_ok(const struct zenixfs_inode *in, int want)       /* want: FSP_R, FSP_W, FSP_X */
{
    if (fsp_access(cuid, cgid, cng, cgs, in->mode, in->uid, in->gid, want)) return 1;
    denied = 1;
    return 0;
}
static int owner_only(const struct zenixfs_inode *in)
{
    if (fsp_owner_or_root(cuid, in->uid)) return 1;
    denied = 1;
    return 0;
}
static int is_dir(const struct zenixfs_inode *in) { return (in->mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR; }
static int is_reg(const struct zenixfs_inode *in) { return (in->mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFREG; }
static int is_lnk(const struct zenixfs_inode *in) { return (in->mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFLNK; }

/* ---- paths ---- */
/* resolve PATH starting at directory CWD (or the root if it begins with /); symbolic links are followed (not the last one
 * unless follow_last). Every directory searched needs x. Returns 0 and the inode, or a negative number. */
/* DESIGN D26: with stoplinks set (a request with UMSG_LINKS) a walk of an absolute path follows no link: at the first one
 * it would follow it answers -FS_LINKED and leaves where (the path, the link's name [lk_s, lk_e), its inode) for
 * linked(), which builds the continuation from the request's own path */
static int stoplinks;
static const char *lk_path;
static uint32_t lk_s, lk_e, lk_ino;

static int namei(uint32_t cwd, const char *path, int follow_last, int depth, uint32_t *out)
{
    uint32_t cur = path[0] == '/' ? ROOT : cwd;
    const char *p = path;
    int r;
    if (depth > 8) return -ELOOP;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) { *out = cur; return 0; }
        const char *e = p;
        while (*e && *e != '/') e++;
        const char *q = e;
        while (*q == '/') q++;
        int last = !*q;
        struct zenixfs_inode dn;
        if ((r = zenixfs_getattr(fs, cur, &dn))) return r;
        if (!is_dir(&dn)) return -ENOTDIR;
        if (!access_ok(&dn, FSP_X)) return -EACCES;
        uint32_t ino;
        if (e - p > 255) return -ENAMETOOLONG;
        if ((r = zenixfs_lookup(fs, cur, p, (int)(e - p), &ino))) return r;
        struct zenixfs_inode in;
        if ((r = zenixfs_getattr(fs, ino, &in))) return r;
        if (is_lnk(&in) && (!last || follow_last)) {
            if (stoplinks && path[0] == '/') {
                lk_path = path; lk_s = (uint32_t)(p - path); lk_e = (uint32_t)(e - path); lk_ino = ino;
                return -FS_LINKED;
            }
            char target[256];
            if (in.size > 255) return -ENAMETOOLONG;    /* audit F15: never follow a prefix of a longer target */
            int n = zenixfs_readlink(fs, ino, target, 255);
            if (n < 0) return n;
            target[n] = 0;
            uint32_t t;
            r = namei(cur, target, 1, depth + 1, &t);
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
    if (n - s <= 0) return -EINVAL;
    if ((n - s == 1 && path[s] == '.') || (n - s == 2 && path[s] == '.' && path[s + 1] == '.')) return -EINVAL;
    if (n - s > 63) return -ENAMETOOLONG;             /* as ext2: a name in a directory entry is at most 63 here */
    char dpath[256];
    if (s > 255) return -ENAMETOOLONG;
    memcpy(dpath, path, (u32)s);
    dpath[s] = 0;
    if (s == 0) { dpath[0] = '.'; dpath[1] = 0; }
    *name = path + s;
    *len = n - s;
    int r = namei(cwd, dpath, 1, 0, dir);
    if (r == -FS_LINKED) lk_path = path;              /* dpath is a copy of PATH's start: the same offsets */
    return r;
}

static int dir_may(uint32_t dir)                  /* add or remove entries: w and x */
{
    struct zenixfs_inode d;
    if (zenixfs_getattr(fs, dir, &d)) return 0;
    if (fsp_dir_modify(cuid, cgid, cng, cgs, d.mode, d.uid, d.gid)) return 1;
    denied = 1;
    return 0;
}
static int sticky_ok(uint32_t dir, const char *name, int len)    /* in a sticky directory only owners may remove */
{
    struct zenixfs_inode d, t;
    uint32_t ino;
    if (zenixfs_getattr(fs, dir, &d)) return 0;
    if (zenixfs_lookup(fs, dir, name, len, &ino) || zenixfs_getattr(fs, ino, &t)) return 1;
    if (fsp_sticky_ok(cuid, d.mode, d.uid, t.uid)) return 1;
    denied = 1;
    return 0;
}

static void fill_stat(uint32_t ino, const struct zenixfs_inode *in, struct ustat *st)
{
    st->ino = ino; st->mode = in->mode; st->size = in->size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)in->size;
    st->mtime = (u32)in->mtime; st->uid = in->uid; st->gid = in->gid; st->links = in->nlink;
}

/* Open inodes (FS_OPEN counts one, FS_CLOSE from the kernel one less). When the last name of an open file goes, the
 * file becomes an orphan (zenixfs_orphan_add: a marker in the tree, so a crash frees it at the next mount) and is
 * freed at the last close (zenixfs_orphan_done). ZenixFS never reuses an inode number, so even without this an old
 * handle could not reach another file; this keeps the data readable through it, as Unix does. */
#define NOPEN 96
static struct { u32 ino, count, orphan; } opens[NOPEN];
static int open_slot(u32 ino)
{
    for (int i = 0; i < NOPEN; i++) if (opens[i].count && opens[i].ino == ino) return i;
    return -1;
}
static int open_ref(u32 ino)
{
    int i = open_slot(ino);
    if (i >= 0) { opens[i].count++; return 0; }
    for (i = 0; i < NOPEN; i++)
        if (!opens[i].count) { opens[i].ino = ino; opens[i].count = 1; opens[i].orphan = 0; return 0; }
    return -ENFILE;                                   /* should not happen (the kernel has fewer files) */
}
static void touched(void);
static void open_unref(u32 ino)
{
    int i = open_slot(ino);
    if (i < 0) return;
    if (--opens[i].count == 0 && opens[i].orphan) { zenixfs_orphan_done(fs, ino); touched(); }
}
/* the entry NAME in DIR is about to go: if it names an open file, make that an orphan first */
static void before_remove(uint32_t dir, const char *name, int len)
{
    uint32_t ino;
    if (zenixfs_lookup(fs, dir, name, len, &ino)) return;
    int i = open_slot(ino);
    if (i >= 0 && !opens[i].orphan && zenixfs_orphan_add(fs, ino) == 0) opens[i].orphan = 1;
}

static void touched(void)
{
    if (!dirty) { dirty = 1; dirty_since = sys_ticks(); }
}

/* the group a new entry in DIR gets, and the extra mode bits of a new directory there (set-group-id directories,
 * kernel/fsperm.h rule 7) */
static unsigned new_gid(uint32_t dir, unsigned *dirbits)
{
    struct zenixfs_inode d;
    if (dirbits) *dirbits = 0;
    if (zenixfs_getattr(fs, dir, &d)) return cgid;
    if (dirbits) *dirbits = fsp_new_dir_bits(d.mode);
    return fsp_new_gid(cgid, d.mode, d.gid);
}
/* a write or truncate by someone else than the owner and root: set-user-id (and set-group-id) go (rule 9) */
static int clear_setid(uint32_t ino)
{
    struct zenixfs_inode in;
    int r = zenixfs_getattr(fs, ino, &in);
    if (r) return r;
    unsigned nm = fsp_write_clear(cuid, in.uid, in.mode);
    if (nm == in.mode) return 0;
    in.mode = nm;
    touched();
    return zenixfs_setattr(fs, ino, &in);
}

/* One request. The result is >= 0, or a negative errno (kernel/errno.h; the ZenixFS library uses the same numbers) -
 * the same the ext2 server returns for the same case (audit E). A permission failure becomes -EACCES in handle(). */
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
        if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {   /* exclusive: the name must not exist, not even as a link */
            if (!(r = namei(m->a[1], path, 0, 0, &ino))) return -EEXIST;
            if (r == -FS_LINKED) return r;
            if (denied) return -EACCES;
        }
        r = namei(m->a[1], path, 1, 0, &ino);
        if (r) {
            if (denied || !(flags & O_CREAT) || r != -ENOENT) return r;
            if (!namei(m->a[1], path, 0, 0, &ino)) return -ENOENT;     /* a symbolic link to nothing: as ext2 */
            if ((r = parent(m->a[1], path, &dir, &name, &len))) return r;
            if (!dir_may(dir)) return -EACCES;
            if ((r = zenixfs_create(fs, dir, name, len, fsp_new_file_mode(cumask), cuid, new_gid(dir, 0), &ino))) return r;
            touched();
        } else {
            if ((r = zenixfs_getattr(fs, ino, &in))) return r;
            if (!access_ok(&in, fsp_open_want(flags))) return -EACCES;
            if (writing) {
                if (is_dir(&in)) return -EISDIR;
                if (!is_reg(&in)) return -EACCES;
                if (flags & O_TRUNC) {
                    if ((r = clear_setid(ino)) || (r = zenixfs_truncate(fs, ino, 0))) return r;
                    touched();
                }
            }
        }
        if ((r = zenixfs_getattr(fs, ino, &in))) return r;
        if ((r = open_ref(ino))) return r;
        fill_stat(ino, &in, (struct ustat *)m->a[3]);
        return (int)ino;
    }
    case FS_CLOSE:
        open_unref(m->a[0]);
        return 0;
    case FS_READ:                                    /* a directory: -EISDIR from the library, as ext2 */
        return zenixfs_read(fs, m->a[0], m->a[1], (void *)m->a[2], m->a[3]);
    case FS_WRITE:
        if ((r = clear_setid(m->a[0]))) return r;
        r = zenixfs_write(fs, m->a[0], m->a[1], (const void *)m->a[2], m->a[3]);
        touched();
        return r;
    case FS_STAT: case FS_LSTAT:
        if ((r = namei(m->a[1], (const char *)m->a[0], m->type == FS_STAT, 0, &ino)) || (r = zenixfs_getattr(fs, ino, &in))) return r;
        fill_stat(ino, &in, (struct ustat *)m->a[2]);
        return 0;
    case FS_FSTAT:
        if ((r = zenixfs_getattr(fs, m->a[0], &in))) return r;
        fill_stat(m->a[0], &in, (struct ustat *)m->a[1]);
        return 0;
    case FS_READDIR: {
        struct udirent *ud = (struct udirent *)m->a[2];
        uint64_t cookie = m->a[1];
        char nm[300]; uint8_t type;
        for (;;) {                                       /* not a directory: -ENOTDIR from the library */
            r = zenixfs_readdir(fs, m->a[0], &cookie, nm, &ino, &type);
            if (r < 0) return r;
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
        return r < 0 ? r : 0;
    case FS_CHMOD: {
        if ((r = namei(m->a[1], (const char *)m->a[0], 1, 0, &ino)) || (r = zenixfs_getattr(fs, ino, &in))) return r;
        if (!owner_only(&in)) return -EACCES;
        in.mode = (in.mode & ZENIXFS_S_IFMT) | fsp_chmod_mode(cuid, cgid, cng, cgs, in.gid, m->a[2]);
        touched();
        return zenixfs_setattr(fs, ino, &in);
    }
    case FS_UTIMES: {
        if ((r = namei(m->a[1], (const char *)m->a[0], 1, 0, &ino)) || (r = zenixfs_getattr(fs, ino, &in))) return r;
        int given = m->a[2] != FS_UTIME_NOW;           /* a given time: the owner or root; "now": also anyone who may write */
        if (!fsp_utimes_ok(cuid, in.uid, given, fsp_access(cuid, cgid, cng, cgs, in.mode, in.uid, in.gid, FSP_W))) { denied = 1; return -EACCES; }
        in.mtime = given ? m->a[2] : (u32)sys_time();
        touched();
        return zenixfs_setattr(fs, ino, &in);
    }
    case FS_CHOWN:
        if ((r = namei(m->a[1], (const char *)m->a[0], 1, 0, &ino)) || (r = zenixfs_getattr(fs, ino, &in))) return r;
        if (!fsp_is_root(cuid)) { denied = 1; return -EACCES; }
        if (m->a[2] != 0xFFFFFFFFu) in.uid = m->a[2];
        if (m->a[3] != 0xFFFFFFFFu) in.gid = m->a[3];
        in.mode &= ~06000u;                              /* chown clears set-user-id and set-group-id */
        touched();
        return zenixfs_setattr(fs, ino, &in);
    case FS_LINK:
        if ((r = namei(m->a[2], (const char *)m->a[0], 1, 0, &ino)) || (r = zenixfs_getattr(fs, ino, &in))) return r;
        if (is_dir(&in)) return -EPERM;                  /* no hard links to directories */
        if ((r = parent(m->a[2], (const char *)m->a[1], &dir, &name, &len))) return r;
        if (!dir_may(dir)) return -EACCES;
        touched();
        return zenixfs_link(fs, dir, name, len, ino);
    case FS_SYMLINK:
        if ((r = parent(m->a[2], (const char *)m->a[1], &dir, &name, &len))) return r;
        if (!dir_may(dir)) return -EACCES;
        if (!((const char *)m->a[0])[0]) return -ENOENT;          /* an empty target, as ext2 and Linux */
        touched();
        return zenixfs_symlink(fs, dir, name, len, (const char *)m->a[0], cuid, new_gid(dir, 0), &ino);
    case FS_READLINK:
        if ((r = namei(m->a[1], (const char *)m->a[0], 0, 0, &ino)) || (r = zenixfs_getattr(fs, ino, &in))) return r;
        if (!is_lnk(&in)) return -EINVAL;
        return zenixfs_readlink(fs, ino, (char *)m->a[2], (int)m->a[3]);
    case FS_ROOT: {
        struct fsinfo *fi = (struct fsinfo *)m->a[0];
        strncpy(fi->dev, devname, sizeof fi->dev - 1);
        strcpy(fi->type, "zenixfs");
        return (int)ROOT;
    }
    case FS_HEALTH:
        return zenixfs_is_readonly(fs) ? 1 : 0;
    case FS_QUIT:
        quitting = 1;
        return 0;
    case FS_CHDIR:
        if ((r = namei(m->a[1], (const char *)m->a[0], 1, 0, &ino)) || (r = zenixfs_getattr(fs, ino, &in))) return r;
        if (!is_dir(&in)) return -ENOTDIR;
        if (!access_ok(&in, 1)) return -EACCES;
        return (int)ino;
    case FS_MKDIR: {
        if ((r = parent(m->a[1], (const char *)m->a[0], &dir, &name, &len))) return r == -EINVAL ? -EEXIST : r;   /* "." ".." exist */
        if (!dir_may(dir)) return -EACCES;
        unsigned bits, gid = new_gid(dir, &bits);
        touched();
        return zenixfs_mkdir(fs, dir, name, len, fsp_new_dir_mode(cumask) | bits, cuid, gid, &ino);
    }
    case FS_UNLINK:
        if ((r = parent(m->a[1], (const char *)m->a[0], &dir, &name, &len))) return r == -EINVAL ? -EISDIR : r;
        if (!dir_may(dir) || !sticky_ok(dir, name, len)) return -EACCES;
        touched();
        before_remove(dir, name, len);
        return zenixfs_unlink(fs, dir, name, len);
    case FS_RMDIR:
        if ((r = parent(m->a[1], (const char *)m->a[0], &dir, &name, &len))) return r;
        if (!dir_may(dir) || !sticky_ok(dir, name, len)) return -EACCES;
        touched();
        return zenixfs_rmdir(fs, dir, name, len);
    case FS_RENAME:
        if ((r = parent(m->a[2], (const char *)m->a[0], &dir, &name, &len)) ||
            (r = parent(m->a[2], (const char *)m->a[1], &dir2, &name2, &len2))) return r;
        if (!dir_may(dir) || !dir_may(dir2) || !sticky_ok(dir, name, len) || !sticky_ok(dir2, name2, len2)) return -EACCES;
        if (dir != dir2 && !zenixfs_lookup(fs, dir, name, len, &ino) && !zenixfs_getattr(fs, ino, &in) && is_dir(&in) &&
            !access_ok(&in, FSP_W)) return -EACCES;      /* a directory to another parent: its ".." changes (fsperm.h rule 10) */
        touched();
        before_remove(dir2, name2, len2);                /* a file renamed over may be open */
        return zenixfs_rename(fs, dir, name, len, dir2, name2, len2);
    }
    return -ENOSYS;
}

/* the FS_LINKED answer (kernel/fsproto.h): the continuation written over the path argument the walk stopped in. Every
 * request walks before it changes anything, so a bounced one changed nothing */
static int linked(struct umsg *m)
{
    int a1 = (m->type == FS_RENAME || m->type == FS_LINK || m->type == FS_SYMLINK) && lk_path == (const char *)m->a[1];
    char *p = (char *)m->a[a1];
    struct zenixfs_inode l;
    char t[256], out[FS_PATHMAX];
    int r = zenixfs_getattr(fs, lk_ino, &l);
    if (r) return r;
    if (l.size > 255) return -ENAMETOOLONG;
    int n = zenixfs_readlink(fs, lk_ino, t, 255);
    if (n < 0) return n;
    if ((r = fsl_text(out, p, lk_s, lk_e, t, (unsigned)n))) return r;
    uint32_t e = lk_e;
    while (p[e] == '/') e++;
    int last = m->type == FS_OPEN && !p[e];           /* the link was the last name of the original path */
    strcpy(p, out);
    return -(FS_LINKED + (a1 ? FSL_A1 : 0) + (last ? FSL_LAST : 0));
}

static int handle(struct umsg *m)
{
    if (!(m->kflags & UMSG_KERNEL) && m->uid != 0) return -EACCES; /* raw IPC from a user: inode numbers would bypass every check */
    cuid = m->uid; cgid = m->gid; cumask = m->umask;
    cng = 0;
    if (m->kflags & UMSG_GROUPS) {                  /* the caller's supplementary groups follow the message */
        const struct umsg_groups *g = UMSG_GROUPS_AT(m);
        cng = g->n < NGROUPS ? g->n : NGROUPS;
        for (unsigned i = 0; i < cng; i++) cgs[i] = g->g[i];
    }
    denied = 0;
    stoplinks = (m->kflags & UMSG_LINKS) != 0;
    zenixfs_set_time(fs, sys_time());
    int r = handle1(m);
    if (r == -FS_LINKED) return linked(m);
    return r < 0 && denied ? -EACCES : r;
}

/* restart: keep the orphans at mount (the kernel may still hold some of them open: D11 5.4). share: read-only on the
 * plain handle. The cache by the machine (the 2 MB diet, DESIGN D18): no fast RAM, 96 KB of blocks and two map pages; with
 * fast RAM 48 blocks and 8 pages. A commit comes when half of the cache is dirty, so a small cache means more commits */
static int try_mount(const char *name, int restart, int share)
{
    u32 mi[4];
    int chiponly = sys_meminfo(mi) == 0 && mi[1] == 0;
    struct zenixfs_mount_opts o = { 48, chiponly ? 2u : 0u, share ? ZENIXFS_MNT_READONLY : restart ? ZENIXFS_MNT_READONLYISH : 0, 0 };
    if (zdisk_open(name, share ? 0 : DISK_MOUNT) < 0) return -1;
    for (unsigned bs = 10; bs <= 12; bs++) {
        if (chiponly) o.cache_blocks = (96u << 10) >> bs;       /* (the library's floor is 32) */
        zdisk_dev(&zdev, bs);
        if (zenixfs_mount_opts(&fs, &zdev, &o) == 0) { strncpy(devname, name, sizeof devname - 1); return 0; }
        fs = 0;
    }
    zdisk_close();
    return -1;
}

static int held(uint32_t ino)                    /* an orphan the kernel still holds open: its last close frees it */
{
    int i = open_slot(ino);
    if (i >= 0) opens[i].orphan = 1;
    return i >= 0;
}

/* After the port is ours, before the first request (DESIGN D11 5.4): the inodes the kernel holds open on us, then
 * the orphans nobody holds are freed, and inode numbers jump past any the dead server may have handed out after its
 * last commit (the kernel may hold those: they must never name another file) */
static void restart_opens(const char *who)
{
    static u32 ino[160];
    int n = sys_svc_opens(ino, sizeof ino);
    u32 top = 0;
    for (int i = 0; i < n; i++) {
        open_ref(ino[i]);
        if (ino[i] > top) top = ino[i];
    }
    int kept = zenixfs_orphans_keep(fs, held);
    if (n < 0 || kept < 0 || zenixfs_ino_floor(fs, top + 1))
        dprintf(1, "%s: %s: restart: open files %d, orphans %d: some space may stay used until fsck.zenixfs\n", who, devname, n, kept);
}

int main(int argc, char **argv)
{
    if (argc < 2) { dprintf(1, "usage: zfsd DEVICE [PORT] [-root] [-id ID]\n"); return 2; }
    /* PORT left out (mount since DESIGN D19 step 1: "/sbin/zfsd DEVICE -id ID" fits the 80 bytes a service's program
     * may have, SVC_TEXT, where the port word would not): fs.DEVICE, the name mount gives the mount's service */
    char pbuf[16];
    const char *pname = argv[2], *dev = argv[1];
    if (argc == 2 || argv[2][0] == '-') {
        strcpy(pbuf, "fs.");
        strncpy(pbuf + 3, dev, 12);
        pbuf[15] = 0;
        pname = pbuf;
    }
    const char *who = pname[2] ? pname : "zfsd";
    char name[16];
    int restart = (sys_svc_self() & 0xFFFF) > 1, share = 0, isroot = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-fsck")) share = isroot = 1;
        if (!strcmp(argv[i], "-root")) isroot = 1;
    }
    if (argc >= 4 && !strcmp(argv[argc - 2], "-id")) {     /* the volume by its place and uuid (volid.h) */
        if (vol_find(sys_port_find("disk"), dev, argv[argc - 1], name, who, restart)) return 1;
        dev = name;
    }
    if (try_mount(dev, restart, share)) {
        dprintf(1, "%s: %s is not a ZenixFS volume, or is in use\n", who, dev);
        if (isroot)
            dprintf(1, "%s: the root cannot be mounted. Start Zenix from a floppy and run fsck.zenixfs -y %s, or repair it on a PC (man rescue)\n", who, dev);
        return 1;
    }
    struct zenixfs_stats st;
    zenixfs_get_stats(fs, &st);
    if (st.mount_damage)
        dprintf(1, "%s: %s: %u commit record(s) or roots were unreadable; mounted from the last good commit%s\n", who, devname, st.mount_damage,
                st.mount_old ? " (not to be built on: read-only, fsck.zenixfs -y)" : "");
    if (!zdisk_can_flush())
        dprintf(1, "%s: %s: the disk cannot flush its write cache: a power cut may lose the last commits, or damage the volume if the disk reorders writes\n", who, devname);
    int port = sys_port_create(pname);
    if (port < 0) { dprintf(1, "%s: cannot create the port %s\n", who, pname); return 1; }
    if (restart && !zenixfs_is_readonly(fs))
        restart_opens(who);
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
        u32 age = sys_ticks() - dirty_since;
        m = 0;
        if (age < COMMIT_TICKS)                         /* changes are waiting: the next request, or the commit time */
            m = (struct umsg *)sys3(32, (u32)port, 1 + COMMIT_TICKS - age, 0);     /* SYS_recv, waiting at most that long */
        if (!m) {                                       /* on time, also under a steady stream of requests */
            zenixfs_sync(fs);
            dirty = 0;
            m = sys_recv(port, 1);
        }
    }
    zenixfs_unmount(fs);
    return 1;
}
