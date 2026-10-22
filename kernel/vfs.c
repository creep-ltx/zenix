#include "kernel.h"

static struct file files[NFILE];

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

long vfs_open(const char *path, int flags)
{
    struct inode in;
    int writing = (flags & (O_WRONLY | O_RDWR)) != 0;
    if (ext2_namei(path, current->cwd_ino, &in, 0)) {
        struct inode dir;
        char name[64];
        if (!(flags & O_CREAT) ||
            ext2_namei_parent(path, current->cwd_ino, &dir, name) ||
            ext2_create(&dir, name, &in))
            return -1;
    } else if (writing) {
        if (!S_ISREG(in.mode))
            return -1;
        if ((flags & O_TRUNC) && ext2_truncate(&in))
            return -1;
    }
    struct file *f = file_new(FT_INODE);
    if (!f)
        return -1;
    f->in = in;
    f->flags = (u16)flags;
    int fd = fd_install(current, f);
    if (fd < 0)
        file_close(f);
    return fd;
}

/* other opens may have changed the file: always work on a fresh inode */
static int refresh(struct file *f)
{
    return ext2_iget(f->in.ino, &f->in);
}

static int parent_of(const char *path, struct inode *dir, char *name)
{
    return ext2_namei_parent(path, current->cwd_ino, dir, name);
}

long vfs_mkdir(const char *path)
{
    struct inode dir;
    char name[64];
    if (parent_of(path, &dir, name))
        return -1;
    return ext2_mkdir(&dir, name);
}

long vfs_unlink(const char *path)
{
    struct inode dir;
    char name[64];
    if (parent_of(path, &dir, name))
        return -1;
    return ext2_unlink(&dir, name);
}

long vfs_rmdir(const char *path)
{
    struct inode dir;
    char name[64];
    if (parent_of(path, &dir, name))
        return -1;
    return ext2_rmdir(&dir, name);
}

long vfs_rename(const char *from, const char *to)
{
    struct inode od, nd;
    char on[64], nn[64];
    if (parent_of(from, &od, on) || parent_of(to, &nd, nn))
        return -1;
    return ext2_rename(&od, on, &nd, nn);
}

long vfs_read(struct file *f, void *buf, u32 n)
{
    switch (f->type) {
    case FT_TTY:
        return tty_read((int)f->off, buf, n);
    case FT_PIPE_R:
        return pipe_read(f, buf, n);
    case FT_INODE: {
        if (S_ISDIR(f->in.mode) || (f->flags & O_WRONLY) || refresh(f))
            return -1;
        int r = ext2_read(&f->in, f->off, buf, n);
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
    case FT_INODE: {
        if (!(f->flags & (O_WRONLY | O_RDWR)) || refresh(f))
            return -1;
        if (f->flags & O_APPEND)
            f->off = f->in.size;
        int r = ext2_write(&f->in, f->off, buf, n);
        if (r > 0)
            f->off += (u32)r;
        return r;
    }
    }
    return -1;
}

long vfs_readdir(struct file *f, struct udirent *out)
{
    if (f->type != FT_INODE || !S_ISDIR(f->in.mode))
        return -1;
    return ext2_readdir(&f->in, &f->off, out);
}

void vfs_stat_inode(const struct inode *in, struct ustat *st)
{
    st->ino = in->ino;
    st->mode = in->mode;
    st->size = in->size;
    st->mtime = in->mtime;
    st->uid = in->uid;
    st->gid = in->gid;
    st->links = in->links;
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
