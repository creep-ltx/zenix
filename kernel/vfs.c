/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "fsproto.h"

static struct file files[NFILE];
static int strcmp_(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int memcmp_(const void *a, const void *b, u32 n)
{
    const u8 *p = a, *q = b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}
static u32 strlen_(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void strcpy_(char *d, const char *s) { while ((*d++ = *s++)) ; }
static void strncpy_(char *d, const char *s, u32 n) { u32 i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; d[i] = 0; }

struct file *file_new(int type)
{
    for (int i = 0; i < NFILE; i++)
        if (files[i].type == FT_NONE) {
            memset(&files[i], 0, sizeof files[i]);
            files[i].type = (u8)type;
            files[i].refs = 1;
            return &files[i];
        }
    return 0;
}

struct file *file_dup(struct file *f)
{
    f->refs++;
    return f;
}

long fs_call_on(int mi, u32 type, u32 a0, u32 a1, u32 a2, u32 a3);

__attribute__((noinline)) void file_close(struct file *f)
{
    if (!f || --f->refs)
        return;
    if (f->type == FT_INODE)                /* the server counts opens: an unlinked inode is freed at the last close */
        fs_call_on(f->fs, FS_CLOSE, f->ino, 0, 0, 0);
    if (f->type == FT_PIPE_R && f->pipe)
        f->pipe->readers--;
    if (f->type == FT_PIPE_W && f->pipe)
        f->pipe->writers--;
    if ((f->type == FT_PIPE_R || f->type == FT_PIPE_W) && f->pipe &&
        !f->pipe->readers && !f->pipe->writers)
        kfree(f->pipe);
    f->type = FT_NONE;
    wake_io();                              /* blocked pipe peers re-check */
}

int fd_install(struct proc *p, struct file *f)
{
    for (int i = 0; i < NFD; i++)
        if (!p->fd[i]) {
            p->fd[i] = f;
            return i;
        }
    return -EMFILE;
}

static long pipe_read(struct file *f, void *buf, u32 n)
{
    struct pipe *p = f->pipe;
    u8 *d = buf;
    for (;;) {
        if (current->killed)
            return -EINTR;
        if (p->count) {
            u32 got = 0;
            while (got < n && p->count) {
                d[got++] = p->buf[p->rd];
                p->rd = (u16)((p->rd + 1) % sizeof p->buf);
                p->count--;
            }
            wake_io();
            return (long)got;
        }
        if (!p->writers)
            return 0;                       /* end of file */
        block_io();
    }
}

static long pipe_write(struct file *f, const void *buf, u32 n)
{
    struct pipe *p = f->pipe;
    const u8 *s = buf;
    u32 put = 0;
    while (put < n) {
        if (current->killed)
            return -EINTR;
        if (!p->readers)
            return -EPIPE;                  /* broken pipe */
        if (p->count < sizeof p->buf) {
            p->buf[p->wr] = s[put++];
            p->wr = (u16)((p->wr + 1) % sizeof p->buf);
            p->count++;
            continue;
        }
        wake_io();
        block_io();
    }
    wake_io();
    return (long)put;
}

long vfs_pipe(int fds[2])
{
    struct pipe *p = kmalloc(sizeof *p);
    struct file *r = file_new(FT_PIPE_R), *w = file_new(FT_PIPE_W);
    if (!p || !r || !w) {
        if (r) r->type = FT_NONE;
        if (w) w->type = FT_NONE;
        kfree(p);
        return p ? -ENFILE : -ENOMEM;
    }
    memset(p, 0, sizeof *p);
    p->readers = p->writers = 1;
    r->pipe = w->pipe = p;
    fds[0] = fd_install(current, r);
    fds[1] = fd_install(current, w);
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0) current->fd[fds[0]] = 0;
        if (fds[1] >= 0) current->fd[fds[1]] = 0;
        file_close(r);
        file_close(w);
        return -EMFILE;
    }
    return 0;
}

/* ---- filesystems live in server processes; see fsproto.h ----
 * Mount table: entry 0 is the root filesystem (the server on port "fs"), more are put on directories by
 * SYS_mount. With only the root mounted a path goes to its server untouched (the server resolves it, symbolic
 * links included). Once something else is mounted, every path is first made absolute and normalised as text
 * (cwd text + path, "." and ".." removed) and handed to the server of the longest mount point that prefixes it,
 * as a path inside that filesystem. Limits of this: ".." is resolved on the text, so a working directory reached
 * through a symbolic link behaves as its text says; a symbolic link on the root filesystem cannot point into a
 * mount by relative ".." going out of it; link and rename across mounts fail. */

#define NMOUNT 8
struct mount {
    u8 used;
    int port;                           /* -1: look it up by name again */
    u32 root;                           /* inode number of the root directory of that filesystem */
    char path[48];                      /* where it is mounted ("/" for the root) */
    char pname[16];                     /* port name of the server */
    char dev[16], type[8];
};
static struct mount mnt[NMOUNT] = { { 1, -1, 0, "/", "fs", "", "" } };
static int nmnt = 1;                    /* mounts in use, the root included */

__attribute__((noinline)) long fs_call_on(int mi, u32 type, u32 a0, u32 a1, u32 a2, u32 a3)
{
    struct umsg m;                      /* on this process's kernel stack: the server reads it in place */
    struct mount *mt = &mnt[mi];
    if (current->pid == 0 || !mt->used)
        return -EINVAL;
    if (mt->port < 0 && (mt->port = port_find(mt->pname)) < 0)
        return -EIO;                    /* the filesystem server is gone */
    m.type = type;
    m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3;
    /* FS_CLOSE never starts a stopped server (D11): a new server has no opens to release, and proc_exit's closes
     * must never load a program */
    long r = ipc_kcall(mt->port, &m, type == FS_CLOSE ? SVC_NS_CLOSE : 0);
    if (r == -ECONNRESET)
        r = -EIO;                       /* the server is gone (D11): programs see an I/O error, as before */
    if (r == -5 || r == -1)
        mt->port = -1;                  /* the server is gone (-1: its port is closed or its slot reused, audit G1) */
    return r;
}

/* the root inode of mount mi (learned from the server the first time it is needed) */
static __attribute__((noinline)) u32 mount_root(int mi)
{
    if (!mnt[mi].root) {
        struct fsinfo fi;
        memset(&fi, 0, sizeof fi);
        long r = fs_call_on(mi, FS_ROOT, (u32)&fi, 0, 0, 0);
        if (r > 0) {
            mnt[mi].root = (u32)r;
            if (!mnt[mi].dev[0]) {
                memcpy(mnt[mi].dev, fi.dev, sizeof mnt[mi].dev);
                memcpy(mnt[mi].type, fi.type, sizeof mnt[mi].type);
            }
        }
    }
    return mnt[mi].root ? mnt[mi].root : 2;
}

#define PATHMAX FS_PATHMAX
/* out = PATH as an absolute, normalised path (from the text working directory CWD): the one normaliser, for routing,
 * mount points and the working directory text. 0, or -ENAMETOOLONG if it would be longer than PATHMAX - 1 */
static __attribute__((noinline)) int path_abs(char *out, const char *cwd, const char *path)
{
    int n = 0;
    if (path[0] != '/') {
        while (cwd[n] && n < PATHMAX - 1) { out[n] = cwd[n]; n++; }
        if (n == 1) n = 0;
    }
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        int len = (int)(e - p);
        if (len == 1 && p[0] == '.') {
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 0 && out[n - 1] != '/') n--;
            if (n > 0) n--;
        } else {
            if (n + 1 + len >= PATHMAX) return -ENAMETOOLONG;
            out[n++] = '/';
            for (int i = 0; i < len; i++) out[n++] = p[i];
        }
        p = e;
    }
    if (n == 0) out[n++] = '/';
    out[n] = 0;
    return 0;
}

/* which mount takes PATH; *rel = the path to give its server (in buf, PATHMAX bytes, or PATH itself), *cwd = the
 * working directory inode to give it. -ENAMETOOLONG when the path is too long. */
static __attribute__((noinline)) int route(const char *path, const char *cwdtext, u32 cwdino, char *buf, const char **rel, u32 *cwd)
{
    if (nmnt <= 1) { *rel = path; *cwd = cwdino; return 0; }
    char tmp[PATHMAX];
    int e = path_abs(tmp, cwdtext, path);
    if (e) return e;
    int best = 0, bl = 1;
    for (int i = 1; i < NMOUNT; i++) {
        if (!mnt[i].used) continue;
        int l = 0;
        while (mnt[i].path[l]) l++;
        if (l > bl && !memcmp_(tmp, mnt[i].path, (u32)l) && (tmp[l] == 0 || tmp[l] == '/')) { best = i; bl = l; }
    }
    const char *r = best ? tmp + bl : tmp;
    if (!*r) r = "/";
    int n = 0;
    while (r[n]) { buf[n] = r[n]; n++; }
    buf[n] = 0;
    *rel = buf;
    *cwd = mount_root(best);
    return best;
}
#define ROUTE(path_, rb_, rel_, cwdino_) route(path_, current->cwd, current->cwd_ino, rb_, &rel_, &cwdino_)

long fs_call(u32 type, u32 a0, u32 a1, u32 a2, u32 a3)
{
    return fs_call_on(0, type, a0, a1, a2, a3);
}

/* Open a program for loading without reading it into a buffer first: the loader reads the header, allocates the
 * process's memory and then reads the image straight into it (fs_exec_read). */
long fs_exec_open(const char *path, const struct proc *parent, struct exec_src *x)
{
    char rb[PATHMAX]; const char *rel; u32 cwd;
    int m = route(path, parent ? parent->cwd : "/", parent ? parent->cwd_ino : 2, rb, &rel, &cwd);
    if (m < 0)
        return m;
    long ino = fs_call_on(m, FS_OPEN, (u32)rel, cwd, O_RDONLY | O_EXEC, (u32)&x->st);
    if (ino < 0)
        return ino;
    x->m = m;
    x->ino = ino;
    if (!S_ISREG(x->st.mode) || x->st.size > 256u * 1024) {
        fs_exec_close(x);
        return S_ISDIR(x->st.mode) ? -EISDIR : S_ISREG(x->st.mode) ? -EFBIG : -EACCES;
    }
    return 0;
}

/* every successful fs_exec_open ends with this (the server counts opens, see FS_CLOSE) */
void fs_exec_close(const struct exec_src *x)
{
    fs_call_on(x->m, FS_CLOSE, (u32)x->ino, 0, 0, 0);
}

long fs_exec_read(const struct exec_src *x, u32 off, void *dst, u32 n)
{
    u32 got = 0;
    while (got < n) {
        long r = fs_call_on(x->m, FS_READ, (u32)x->ino, off + got, (u32)((u8 *)dst + got), n - got);
        if (r <= 0)
            return r < 0 ? r : -EIO;        /* shorter than its size said */
        got += (u32)r;
    }
    return 0;
}

/* the few device files: /dev/tty (this process's controlling tty), /dev/console (console 1), /dev/tty1 .. /dev/tty10
 * (the virtual consoles: root, or a process whose controlling tty it is - nobody reads another user's keys),
 * /dev/null, /dev/zero */
static long open_device(const char *path, int flags)
{
    int type = 0, con = 0;
    if (!strcmp_(path, "/dev/tty")) { type = FT_TTY; con = tty_of(current); }
    else if (!strcmp_(path, "/dev/null")) type = FT_NULL;
    else if (!strcmp_(path, "/dev/zero")) type = FT_ZERO;
    else {
        if (!strcmp_(path, "/dev/console"))
            con = 1;
        else if (path[5] == 't' && path[6] == 't' && path[7] == 'y' && path[8] >= '1' && path[8] <= '9' &&
                 (!path[9] || (path[8] == '1' && path[9] == '0' && !path[10])))
            con = path[9] ? 10 : path[8] - '0';
        else
            return -ENODEV;                 /* not a device of ours: the filesystem's /dev */
        type = FT_TTY;
        if (current->uid != 0 && con != tty_of(current))
            return -EACCES;                  /* another console: root only */
    }
    struct file *f = file_new(type);
    if (!f)
        return -ENFILE;
    f->off = (u32)con;
    f->flags = (u16)flags;
    f->mode = 020666;                       /* a character device */
    int fd = fd_install(current, f);
    if (fd < 0)
        file_close(f);
    return fd;
}

long vfs_open(const char *path, int flags)
{
    struct ustat st;
    char rb[PATHMAX]; const char *rel; u32 cwd;
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/') {
        long d = open_device(path, flags);
        if (d != -ENODEV)
            return d;
    }
    int m = ROUTE(path, rb, rel, cwd);
    if (m < 0)
        return m;
    long ino = fs_call_on(m, FS_OPEN, (u32)rel, cwd, (u32)flags, (u32)&st);
    if (ino < 0)
        return ino;                         /* the server's errno: -ENOENT, -EACCES, -EEXIST (O_CREAT|O_EXCL) ... */
    struct file *f = file_new(FT_INODE);
    if (!f) {
        fs_call_on(m, FS_CLOSE, (u32)ino, 0, 0, 0);
        return -ENFILE;
    }
    f->ino = (u32)ino;
    f->fs = (u8)m;
    f->mode = st.mode;
    f->flags = (u16)flags;
    int fd = fd_install(current, f);
    if (fd < 0)
        file_close(f);
    return fd;
}

long vfs_stat(const char *path, struct ustat *st)
{
    char rb[PATHMAX]; const char *rel; u32 cwd;
    int m = ROUTE(path, rb, rel, cwd);
    return m < 0 ? m : fs_call_on(m, FS_STAT, (u32)rel, cwd, (u32)st, 0);
}

long vfs_fstat(struct file *f, struct ustat *st)
{
    if (f->type == FT_TTY || f->type == FT_NULL || f->type == FT_ZERO) {
        memset(st, 0, sizeof *st);
        st->mode = 020666;
        st->links = 1;
        if (f->type == FT_TTY)
            st->ino = f->off;               /* a console's number (1..NCONS): the ustat has no device field */
        return 0;
    }
    if (f->type != FT_INODE)
        return -EINVAL;                     /* a pipe: no stat */
    return fs_call_on(f->fs, FS_FSTAT, f->ino, (u32)st, 0, 0);
}

long vfs_chdir(const char *path)
{
    char rb[PATHMAX], text[PATHMAX]; const char *rel; u32 cwd;
    int e = path_abs(text, current->cwd, path);         /* the new working directory's text must fit: else refused */
    if (e)
        return e;
    int m = ROUTE(path, rb, rel, cwd);
    if (m < 0)
        return m;
    long ino = fs_call_on(m, FS_CHDIR, (u32)rel, cwd, 0, 0);
    if (ino < 0)
        return ino;
    current->cwd_ino = (u32)ino;
    strcpy_(current->cwd, text);
    return 0;
}

long vfs_lseek(struct file *f, s32 off, u32 whence)
{
    s32 np;
    if (whence == 0)
        np = off;
    else if (whence == 1)
        np = (s32)f->off + off;
    else if (whence == 2) {
        struct ustat st;
        long r = fs_call_on(f->fs, FS_FSTAT, f->ino, (u32)&st, 0, 0);
        if (r < 0)
            return r;
        np = (s32)st.size + off;
    } else
        return -EINVAL;                     /* SEEK_SET 0, SEEK_CUR 1, SEEK_END 2: nothing else */
    if (np < 0)
        return -EINVAL;
    f->off = (u32)np;
    return np;
}

/* mount is the n-th mount in use, as for vfs_mountinfo (0 = the root), so the two can be paired after an unmount
 * left a gap in the table */
long vfs_statfs(void *out, u32 mount)
{
    u32 seen = 0;
    for (int i = 0; i < NMOUNT; i++)
        if (mnt[i].used && seen++ == mount)
            return fs_call_on(i, FS_STATFS, (u32)out, 0, 0, 0);
    return -EINVAL;
}

long vfs_path1(u32 op, const char *p, u32 a2, u32 a3)
{
    char rb[PATHMAX]; const char *rel; u32 cwd;
    int m = ROUTE(p, rb, rel, cwd);
    if (m < 0)
        return m;
    return fs_call_on(m, op, (u32)rel, cwd, a2, a3);
}

long vfs_link(u32 op, const char *a, const char *b)
{
    char ra[PATHMAX], rb[PATHMAX]; const char *rela, *relb; u32 ca, cb;
    if (op == FS_SYMLINK) {                 /* a is the link text, only b is a path */
        int m = ROUTE(b, rb, relb, cb);
        return m < 0 ? m : fs_call_on(m, op, (u32)a, (u32)relb, cb, 0);
    }
    int m1 = ROUTE(a, ra, rela, ca), m2 = ROUTE(b, rb, relb, cb);
    if (m1 < 0 || m2 < 0)
        return m1 < 0 ? m1 : m2;
    if (m1 != m2)
        return -EXDEV;                      /* a hard link cannot cross filesystems */
    return fs_call_on(m1, op, (u32)rela, (u32)relb, ca, 0);
}

long vfs_sync(void)
{
    long r = 0;
    for (int i = 0; i < NMOUNT; i++)
        if (mnt[i].used && fs_call_on(i, FS_SYNC, 0, 0, 0, 0) < 0 && i == 0)
            r = -EIO;
    return r;
}

long vfs_mkdir(const char *path)
{
    char rb[PATHMAX]; const char *rel; u32 cwd;
    int m = ROUTE(path, rb, rel, cwd);
    return m < 0 ? m : fs_call_on(m, FS_MKDIR, (u32)rel, cwd, 0, 0);
}

long vfs_unlink(const char *path)
{
    char rb[PATHMAX]; const char *rel; u32 cwd;
    int m = ROUTE(path, rb, rel, cwd);
    return m < 0 ? m : fs_call_on(m, FS_UNLINK, (u32)rel, cwd, 0, 0);
}

long vfs_rmdir(const char *path)
{
    char rb[PATHMAX]; const char *rel; u32 cwd;
    int m = ROUTE(path, rb, rel, cwd);
    return m < 0 ? m : fs_call_on(m, FS_RMDIR, (u32)rel, cwd, 0, 0);
}

long vfs_rename(const char *from, const char *to)
{
    char ra[PATHMAX], rb[PATHMAX]; const char *rela, *relb; u32 ca, cb;
    int m1 = ROUTE(from, ra, rela, ca), m2 = ROUTE(to, rb, relb, cb);
    if (m1 < 0 || m2 < 0)
        return m1 < 0 ? m1 : m2;
    if (m1 != m2)
        return -EXDEV;                      /* not across filesystems: mv copies */
    return fs_call_on(m1, FS_RENAME, (u32)rela, (u32)relb, ca, 0);
}

long vfs_read(struct file *f, void *buf, u32 n)
{
    switch (f->type) {
    case FT_TTY:
        return tty_read((int)f->off, buf, n);
    case FT_PIPE_R:
        return pipe_read(f, buf, n);
    case FT_NULL:
        return 0;
    case FT_ZERO:
        memset(buf, 0, n);
        return (long)n;
    case FT_PIPE_W:
        return -EBADF;
    case FT_INODE: {
        if (S_ISDIR(f->mode))
            return -EISDIR;
        if (f->flags & O_WRONLY)
            return -EBADF;                  /* opened for writing only */
        long r = fs_call_on(f->fs, FS_READ, f->ino, f->off, (u32)buf, n);
        if (r > 0)
            f->off += (u32)r;
        return r;
    }
    }
    return -EBADF;
}

long vfs_write(struct file *f, const void *buf, u32 n)
{
    switch (f->type) {
    case FT_TTY:
        return tty_write((int)f->off, buf, n);
    case FT_PIPE_W:
        return pipe_write(f, buf, n);
    case FT_NULL: case FT_ZERO:
        return (long)n;                     /* the bit bucket */
    case FT_INODE: {
        if (!(f->flags & (O_WRONLY | O_RDWR)))
            return -EBADF;                  /* opened for reading only */
        if (f->flags & O_APPEND) {
            struct ustat st;
            long e = fs_call_on(f->fs, FS_FSTAT, f->ino, (u32)&st, 0, 0);
            if (e < 0)
                return e;
            f->off = st.size;
        }
        long r = fs_call_on(f->fs, FS_WRITE, f->ino, f->off, (u32)buf, n);
        if (r > 0)
            f->off += (u32)r;
        return r;
    }
    }
    return -EBADF;
}

long vfs_readdir(struct file *f, struct udirent *out)
{
    u32 cookie = f->off;
    if (f->type != FT_INODE || !S_ISDIR(f->mode))
        return -ENOTDIR;
    long r = fs_call_on(f->fs, FS_READDIR, f->ino, f->off, (u32)out, (u32)&cookie);
    if (r >= 0)
        f->off = cookie;
    return r;
}

/* ---- mount, umount ---- */

static int user_str_eq(const char *a, const char *b) { return !strcmp_(a, b); }

long vfs_mount(const struct umountreq *r)
{
    char abs[PATHMAX], rb[PATHMAX]; const char *rel; u32 cwd;
    struct ustat st;
    int e = path_abs(abs, current->cwd, r->path);
    if (e)
        return e;
    if (abs[1] == 0)
        return -EBUSY;                          /* not on the root */
    if (strlen_(abs) >= sizeof mnt[0].path)
        return -ENAMETOOLONG;                   /* a mount point is at most 47 bytes */
    int free = -1;
    for (int i = 1; i < NMOUNT; i++) {
        if (mnt[i].used && user_str_eq(mnt[i].path, abs))
            return -EBUSY;                      /* already mounted there */
        if (!mnt[i].used && free < 0)
            free = i;
    }
    if (free < 0)
        return -EBUSY;                          /* all NMOUNT entries in use */
    int pm = ROUTE(abs, rb, rel, cwd);
    if (pm < 0)
        return pm;
    if ((e = (int)fs_call_on(pm, FS_STAT, (u32)rel, cwd, (u32)&st, 0)) < 0)
        return e;
    if (!S_ISDIR(st.mode))
        return -ENOTDIR;                        /* the mount point must be a directory */
    int port = port_find(r->port);
    if (port < 0)
        return -ENODEV;                         /* no server on that port */
    struct mount *mt = &mnt[free];
    memset(mt, 0, sizeof *mt);
    strcpy_(mt->path, abs);
    strncpy_(mt->pname, r->port, sizeof mt->pname - 1);
    strncpy_(mt->dev, r->dev, sizeof mt->dev - 1);
    strncpy_(mt->type, r->type, sizeof mt->type - 1);
    mt->port = port;
    mt->used = 1;
    struct fsinfo fi;
    long root = fs_call_on(free, FS_ROOT, (u32)&fi, 0, 0, 0);
    if (root <= 0) {
        mt->used = 0;
        return root < 0 ? root : -EIO;
    }
    mt->root = (u32)root;
    nmnt++;
    return 0;
}

static int prefix_of(const char *dir, const char *path)         /* is PATH DIR or below it */
{
    int l = (int)strlen_(dir);
    return !memcmp_(path, dir, (u32)l) && (path[l] == 0 || path[l] == '/');
}

long vfs_umount(const char *path)
{
    char abs[PATHMAX];
    int e = path_abs(abs, current->cwd, path);
    if (e)
        return e;
    int mi = -1;
    for (int i = 1; i < NMOUNT; i++)
        if (mnt[i].used && user_str_eq(mnt[i].path, abs))
            mi = i;
    if (mi < 0)
        return -EINVAL;                         /* nothing is mounted there */
    for (int i = 1; i < NMOUNT; i++)                    /* busy: another filesystem is mounted below it */
        if (i != mi && mnt[i].used && prefix_of(mnt[mi].path, mnt[i].path))
            return -EBUSY;
    for (int i = 1; i < NPROC; i++) {                   /* busy: a working directory or an open file inside */
        struct proc *p = &procs[i];
        if (p->state == P_FREE)
            continue;
        if (prefix_of(mnt[mi].path, p->cwd))
            return -EBUSY;
        for (int k = 0; k < NFD; k++)
            if (p->fd[k] && p->fd[k]->type == FT_INODE && p->fd[k]->fs == mi)
                return -EBUSY;
    }
    fs_call_on(mi, FS_QUIT, 0, 0, 0, 0);               /* the server syncs, lets go of its device and exits */
    mnt[mi].used = 0;
    nmnt--;
    return 0;
}

long vfs_mountinfo(u32 index, struct umountinfo *out)
{
    u32 seen = 0;
    for (int i = 0; i < NMOUNT; i++) {
        if (!mnt[i].used)
            continue;
        if (seen++ != index)
            continue;
        if (i == 0)
            mount_root(0);
        memset(out, 0, sizeof *out);
        strncpy_(out->path, mnt[i].path, sizeof out->path - 1);
        strncpy_(out->port, mnt[i].pname, sizeof out->port - 1);
        strncpy_(out->dev, mnt[i].dev, sizeof out->dev - 1);
        strncpy_(out->type, mnt[i].type, sizeof out->type - 1);
        return 0;
    }
    return -1;                                  /* past the end (mount -l counts on it: not an error) */
}
