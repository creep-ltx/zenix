#include "kernel.h"
#include "fsproto.h"

static struct file files[NFILE];
static int strcmp_(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

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

void file_close(struct file *f)
{
    if (!f || --f->refs)
        return;
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
    return -1;
}

static long pipe_read(struct file *f, void *buf, u32 n)
{
    struct pipe *p = f->pipe;
    u8 *d = buf;
    for (;;) {
        if (current->killed)
            return -1;
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
            return -1;
        if (!p->readers)
            return -1;                      /* broken pipe */
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
        return -1;
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
        return -1;
    }
    return 0;
}

/* ---- the filesystem lives in a server process; see fsproto.h ---- */

static int fs_port = -1;

long fs_call(u32 type, u32 a0, u32 a1, u32 a2, u32 a3)
{
    struct umsg m;                      /* on this process's kernel stack: the server reads it in place */
    if (current->pid == 0)
        return -1;
    if (fs_port < 0 && (fs_port = port_find("fs")) < 0)
        return -1;
    m.type = type;
    m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3;
    long r = ipc_call(fs_port, &m);
    if (r == -5)
        fs_port = -1;                   /* the server is gone */
    return r;
}

long fs_read_file(const char *path, u32 cwd, u8 **buf, u32 *size)
{
    struct ustat st;
    long ino = fs_call(FS_OPEN, (u32)path, cwd, O_RDONLY, (u32)&st);
    if (ino < 0)
        return -1;
    if (!S_ISREG(st.mode) || st.size > 256u * 1024)
        return -2;
    u8 *b = kmalloc(st.size ? st.size : 1);
    if (!b)
        return -3;
    u32 got = 0;
    while (got < st.size) {
        long r = fs_call(FS_READ, (u32)ino, got, (u32)(b + got), st.size - got);
        if (r <= 0) {
            kfree(b);
            return -4;
        }
        got += (u32)r;
    }
    *buf = b;
    *size = st.size;
    return 0;
}

/* the few device files: /dev/tty (this process's terminal), /dev/null, /dev/zero */
static long open_device(const char *path, int flags)
{
    int type = 0;
    if (!strcmp_(path, "/dev/tty") || !strcmp_(path, "/dev/console")) type = FT_TTY;
    else if (!strcmp_(path, "/dev/null")) type = FT_NULL;
    else if (!strcmp_(path, "/dev/zero")) type = FT_ZERO;
    else return -2;
    struct file *f = file_new(type);
    if (!f)
        return -1;
    f->off = type == FT_TTY ? (u32)tty_of(current) : 0;
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
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/') {
        long d = open_device(path, flags);
        if (d != -2)
            return d;
    }
    long ino = fs_call(FS_OPEN, (u32)path, current->cwd_ino, (u32)flags, (u32)&st);
    if (ino < 0)
        return -1;
    struct file *f = file_new(FT_INODE);
    if (!f)
        return -1;
    f->ino = (u32)ino;
    f->mode = st.mode;
    f->flags = (u16)flags;
    int fd = fd_install(current, f);
    if (fd < 0)
        file_close(f);
    return fd;
}

long vfs_stat(const char *path, struct ustat *st)
{
    return fs_call(FS_STAT, (u32)path, current->cwd_ino, (u32)st, 0);
}

long vfs_fstat(struct file *f, struct ustat *st)
{
    if (f->type == FT_TTY || f->type == FT_NULL || f->type == FT_ZERO) {
        memset(st, 0, sizeof *st);
        st->mode = 020666;
        st->links = 1;
        return 0;
    }
    if (f->type != FT_INODE)
        return -1;
    return fs_call(FS_FSTAT, f->ino, (u32)st, 0, 0);
}

long vfs_chdir(const char *path)
{
    long ino = fs_call(FS_CHDIR, (u32)path, current->cwd_ino, 0, 0);
    if (ino < 0)
        return -1;
    current->cwd_ino = (u32)ino;
    vfs_chdir_text(current->cwd, path);
    return 0;
}

long vfs_lseek(struct file *f, s32 off, u32 whence)
{
    s32 np;
    if (whence == 0)
        np = off;
    else if (whence == 1)
        np = (s32)f->off + off;
    else {
        struct ustat st;
        if (fs_call(FS_FSTAT, f->ino, (u32)&st, 0, 0) < 0)
            return -1;
        np = (s32)st.size + off;
    }
    if (np < 0)
        return -1;
    f->off = (u32)np;
    return np;
}

long vfs_statfs(void *out)
{
    return fs_call(FS_STATFS, (u32)out, 0, 0, 0);
}

long vfs_path1(u32 op, const char *p, u32 a2, u32 a3)
{
    if (op == FS_READLINK)
        return fs_call(op, (u32)p, current->cwd_ino, a2, a3);
    return fs_call(op, (u32)p, current->cwd_ino, a2, 0);
}

long vfs_link(u32 op, const char *a, const char *b)
{
    return fs_call(op, (u32)a, (u32)b, current->cwd_ino, 0);
}

long vfs_sync(void)
{
    return fs_call(FS_SYNC, 0, 0, 0, 0);
}

long vfs_mkdir(const char *path)
{
    return fs_call(FS_MKDIR, (u32)path, current->cwd_ino, 0, 0);
}

long vfs_unlink(const char *path)
{
    return fs_call(FS_UNLINK, (u32)path, current->cwd_ino, 0, 0);
}

long vfs_rmdir(const char *path)
{
    return fs_call(FS_RMDIR, (u32)path, current->cwd_ino, 0, 0);
}

long vfs_rename(const char *from, const char *to)
{
    return fs_call(FS_RENAME, (u32)from, (u32)to, current->cwd_ino, 0);
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
    case FT_INODE: {
        if (S_ISDIR(f->mode) || (f->flags & O_WRONLY))
            return -1;
        long r = fs_call(FS_READ, f->ino, f->off, (u32)buf, n);
        if (r > 0)
            f->off += (u32)r;
        return r;
    }
    }
    return -1;
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
            return -1;
        if (f->flags & O_APPEND) {
            struct ustat st;
            if (fs_call(FS_FSTAT, f->ino, (u32)&st, 0, 0) < 0)
                return -1;
            f->off = st.size;
        }
        long r = fs_call(FS_WRITE, f->ino, f->off, (u32)buf, n);
        if (r > 0)
            f->off += (u32)r;
        return r;
    }
    }
    return -1;
}

long vfs_readdir(struct file *f, struct udirent *out)
{
    u32 cookie = f->off;
    if (f->type != FT_INODE || !S_ISDIR(f->mode))
        return -1;
    long r = fs_call(FS_READDIR, f->ino, f->off, (u32)out, (u32)&cookie);
    if (r >= 0)
        f->off = cookie;
    return r;
}

/* join and normalise a path textually: handles "." and ".." */
void vfs_chdir_text(char *cwd, const char *path)
{
    char out[96];
    int n = 0;
    if (path[0] != '/') {
        while (cwd[n] && n < 95) {
            out[n] = cwd[n];
            n++;
        }
        if (n == 1)
            n = 0;                          /* "/" + "/x" must not give "//x" */
    }
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *e = p;
        while (*e && *e != '/')
            e++;
        u32 len = (u32)(e - p);
        if (len == 1 && p[0] == '.') {
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;                        /* drop the slash before it */
        } else if (n + 1 + (int)len < 95) {
            out[n++] = '/';
            for (u32 i = 0; i < len; i++)
                out[n++] = p[i];
        }
        p = e;
    }
    if (n == 0)
        out[n++] = '/';
    out[n] = 0;
    for (int i = 0; i <= n; i++)
        cwd[i] = out[i];
}
