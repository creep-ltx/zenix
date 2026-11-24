/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "mmu.h"
#include "fsproto.h"
#include "random.h"

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
    if (f->type == FT_SERIAL)
        serdev_close();
    if (f->type == FT_SOCK)
        sock_release(f->ino);               /* NET_CLOSE while its epoch lives (D19 rule 11) */
    if (f->type == FT_GCON)
        gcon_close(f);                      /* D38: the console is the kernel's again */
    if (f->type == FT_TTY || f->type == FT_PTM) {
        int t = f->type;
        f->type = FT_NONE;                  /* first: a hang-up's signals see the file gone (D20 7.2) */
        tty_closed(t, (int)f->off);
        return;
    }
    struct pipe *pp = (f->type == FT_PIPE_R || f->type == FT_PIPE_W) ? f->pipe : 0;
    if (pp) {
        if (f->type == FT_PIPE_R)
            pp->readers--;
        else
            pp->writers--;
        ev_wake(&pp->rwait);                /* blocked peers check again: end of file, a broken pipe (D16) */
        ev_wake(&pp->wwait);
        if (!pp->readers && !pp->writers)
            kfree(pp);
    }
    f->type = FT_NONE;
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

/* OS-13: K bytes between the ring and LIN (TO: into the ring), at most two pieces; moves the index and the count.
 * The copies are memcpy's (main.c, -O2); the caller has checked that K fits (the count, or the room) */
static void pipe_copy(struct pipe *p, u8 *lin, u32 k, int to)
{
    u32 at = to ? p->wr : p->rd, a = PIPE_SIZE - at, end = (at + k) & (PIPE_SIZE - 1);
    u8 *r = p->buf;
    if (k < 16) {                           /* a few bytes (a key, a round trip): no calls */
        for (u32 i = 0; i < k; i++, at = (at + 1) & (PIPE_SIZE - 1))
            if (to)
                r[at] = lin[i];
            else
                lin[i] = r[at];
    } else {
        if (a > k)
            a = k;
        if (to) {
            memcpy(r + at, lin, a);
            memcpy(r, lin + a, k - a);
        } else {
            memcpy(lin, r + at, a);
            memcpy(lin + a, r, k - a);
        }
    }
    if (to) {
        p->wr = (u16)end;
        p->count = (u16)(p->count + k);
    } else {
        p->rd = (u16)end;
        p->count = (u16)(p->count - k);
    }
}

/* D16: a blocked reader or writer waits on the pipe's slot mask (event.c's order); a handler interrupts the wait
 * (-EINTR, restarted with SA_RESTART), a write that already moved bytes returns their count */
static long pipe_read(struct file *f, void *buf, u32 n)
{
    struct pipe *p = f->pipe;
    for (;;) {
        if (current->killed)
            return -EINTR;
        ev_arm(&p->rwait);
        if (p->count) {
            u32 got = n < p->count ? n : p->count;
            pipe_copy(p, buf, got, 0);
            ev_wake(&p->wwait);
            return (long)got;
        }
        if (!p->writers)
            return 0;                       /* end of file */
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;                 /* SYS_pipe's O_NONBLOCK: never wait */
        long e = ev_block(EV_IO, 0, 0, 1);
        if (e)
            return e;
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
        if (!p->readers) {
            sig_send(current, SIGPIPE);     /* broken pipe: by default the writer ends (141); ignored: -EPIPE */
            return -EPIPE;
        }
        if (p->count < PIPE_SIZE) {
            u32 k = PIPE_SIZE - p->count;
            if (k > n - put)
                k = n - put;
            pipe_copy(p, (u8 *)s + put, k, 1);
            put += k;
            continue;
        }
        ev_wake(&p->rwait);
        if (f->flags & O_NONBLOCK)
            return put ? (long)put : -EAGAIN;   /* what fitted, or nothing (SYS_pipe's O_NONBLOCK) */
        ev_arm(&p->wwait);
        if (p->count < PIPE_SIZE || !p->readers)
            continue;
        long e = ev_block(EV_IO, 0, 0, !put);
        if (e)
            return put ? (long)put : e;
    }
    ev_wake(&p->rwait);
    return (long)put;
}

/* SYS_evwait's check of one open file (D16): what a read or write would do without waiting; registers the caller on
 * the object. Files, /dev/null and the like never wait */
u16 vfs_poll(struct file *f)
{
    struct pipe *p = f->pipe;
    switch (f->type) {
    case FT_TTY:
        return tty_poll((int)f->off);
    case FT_PTM:
        return ptm_poll((int)f->off);
    case FT_GCON:
        return gcon_poll(f);
    case FT_SERIAL:
        return serdev_poll();
    case FT_PIPE_R:
        ev_arm(&p->rwait);
        return p->count ? POLLIN : !p->writers ? POLLIN | POLLHUP : 0;
    case FT_PIPE_W:
        ev_arm(&p->wwait);
        return !p->readers ? POLLERR : p->count < PIPE_SIZE ? POLLOUT : 0;
    case FT_RANDOM:
        return rnd_poll((int)f->off);
    case FT_SOCK:
        return sock_poll(f);            /* the server's bits, no message (D19 rule 8) */
    }
    return POLLIN | POLLOUT;
}

long vfs_pipe(int fds[2], u32 flags)
{
    if (flags & ~(u32)O_NONBLOCK)
        return -EINVAL;                 /* pipe2's flags: only O_NONBLOCK (both ends, as Linux) */
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
    r->flags = w->flags = (u16)flags;
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
 * as a path inside that filesystem. Its symbolic links are the kernel's then (DESIGN D26): the request carries
 * UMSG_LINKS, the server stops at the first link and answers where the path continues (FS_LINKED), and the kernel
 * normalises that, picks the mount again and sends the request again (struct pwalk, wstep). chdir and mount do so
 * with only the root mounted too, so the working directory and the mount points are recorded resolved. link and
 * rename across mounts fail. */

#define NMOUNT 8
struct mount {
    u8 used;
    int port;                           /* -1: look it up by name again */
    u32 root;                           /* inode number of the root directory of that filesystem */
    char path[48];                      /* where it is mounted ("/" for the root) */
    char pname[16];                     /* port name of the server */
    char dev[16], type[8];
    u32 gen;                            /* D13: new on every mount and every (re)start of its server (vfs_server_started) */
    u32 ok_epoch;                       /* F13: the ids below passed mount_search at this perm_epoch (0: none) */
    u16 ok_uid, ok_gid;
    u32 ok_grp;                         /* and the supplementary groups' list (struct proc grp_id) */
    u32 inst;                           /* D26: new on every mount only (gen also moves when its server restarts) */
    u8 net;                             /* D23 5.4: its server said type "nfs" at FS_ROOT: synced last, bounded */
};
static struct mount mnt[NMOUNT] = { { 1, -1, 0, "/", "fs", "", "", 1, 0, 0, 0, 0, 1, 0 } };

/* D23 5.4: a network filesystem, by the type its server gave in FS_ROOT's fsinfo (later network types join here) */
static int net_type(const struct fsinfo *fi)
{
    return fi->type[0] == 'n' && fi->type[1] == 'f' && fi->type[2] == 's' && !fi->type[3];
}
static u32 gen_last = 1, inst_last = 1;
/* F13: rises with every request that can change who may search a directory (a mode, an owner, a name moved or
 * removed), every mount, umount and server start: a cached mount_search answer older than it is asked again */
static u32 perm_epoch = 1;
static int nmnt = 1;                    /* mounts in use, the root included */
struct proc *vfs_hold;                  /* D29: while set, only it (and the servers) send filesystem requests */
/* SYS_groups' SET: a new list's number (struct proc grp_id). It is the next perm_epoch, so it is never one given before
 * (and never 0), and every cached mount_search answer, made for some older list, is asked again */
u32 vfs_new_grp_id(void) { return ++perm_epoch; }

/* D29 7.2 step 5: while zine syncs and hands the machine over, nobody else starts a filesystem request (so every
 * server is idle with its writes out at the hand-over); the servers' own calls (a server on an image file) go on */
static int held(void)
{
    if (!vfs_hold || vfs_hold == current || current->priv)
        return 0;
    for (int i = 0; i < NMOUNT; i++)
        if (mnt[i].used && mnt[i].port >= 0 && port_owner(mnt[i].port) == current)
            return 0;
    return 1;
}

/* one request to mount mi's server; kfl: UMSG_LINKS when the sender takes the FS_LINKED answer (wstep), else 0 */
static __attribute__((noinline)) long fs_call_k(int mi, u32 type, u32 a0, u32 a1, u32 a2, u32 a3, u16 kfl)
{
    struct { struct umsg m; struct umsg_groups g; } km;   /* on this process's kernel stack: the server reads it in
                                         * place; the caller's supplementary groups follow the message (UMSG_GROUPS) */
#define m km.m
    struct mount *mt = &mnt[mi];
    if (current->pid == 0 || !mt->used)
        return -EINVAL;
    while (held()) {
        if (current->killed && !current->exiting && type != FS_CLOSE)
            return -EINTR;              /* an exit's closes wait for the resume (ipc sends them even when killed) */
        sleep_until(ticks + 2);
        schedule();
    }
    if (mt->port < 0 && (mt->port = port_find(mt->pname)) < 0)
        return -EIO;                    /* the filesystem server is gone */
    m.type = type;
    m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3;
    if (type == FS_CHMOD || type == FS_CHOWN || type == FS_RENAME || type == FS_RMDIR || type == FS_UNLINK)
        perm_epoch++;                   /* before the call: a check that overlaps it is not cached (mount_search) */
    if (type == FS_CHMOD && mmu_on)
        seg_unlist(mi, -1);             /* D60: a segment's global pages follow its file's mode (the next spawn loads it) */
    /* FS_CLOSE never starts a stopped server (D11): a new server has no opens to release, and proc_exit's closes
     * must never load a program; FS_SYNC and FS_QUIT skip a server that is not running (it has nothing to write: an
     * ext2 one wrote through, a dead ZenixFS one's uncommitted changes are gone, a starting one wrote nothing yet) */
    int ns = type == FS_CLOSE ? SVC_NS_CLOSE : type == FS_SYNC || type == FS_QUIT ? SVC_NS_SKIP : 0;
    long r = ipc_kcall(mt->port, &m, ns, (u16)(kfl | UMSG_GROUPS), KP_FS);
#undef m
    if (r == -ECONNRESET) {             /* the server is gone (D11), or its port closed or its slot reused (G1, A11) */
        mt->port = -1;                  /* look it up again next time */
        r = ns == SVC_NS_SKIP ? 0 : -EIO;   /* programs see an I/O error, as before; a sync or quit skipped a server
                                             * that is not running: nothing to write */
    } else if (r == -EIO) {
        mt->port = -1;
    }
    if (!kfl && fsl_is(r))
        r = -EIO;                       /* D26: an FS_LINKED nobody asked for never reaches a program */
    return r;
}

long fs_call_on(int mi, u32 type, u32 a0, u32 a1, u32 a2, u32 a3)
{
    return fs_call_k(mi, type, a0, a1, a2, a3, 0);
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
            mnt[mi].net = (u8)net_type(&fi);
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

/* F13 (audit 2026-10-26): a path that crosses into mount mi passes the directories above its mount point, which live
 * on the parent volume: the caller needs search (x) permission on each, as on Linux. A mount's server sees only the
 * path below the mount point, so the kernel asks the parent's server for the mount point itself (FS_LSTAT with the
 * caller's ids: its walk checks x on every directory above it), and so on up through nested mounts to the root.
 * Root (uid 0, and a service's program read as root, D11) passes, as on Linux. Every error refuses (a parent server
 * that is gone, a mount point renamed away under its mount: Linux would not find it either). 0 or -errno. */
static __attribute__((noinline)) int mount_search(int mi)
{
    char buf[sizeof mnt[0].path];       /* a mount point's text, or the part of it below another mount */
    if (current->svc_depth || current->uid == 0)
        return 0;
    /* the answer is cached per mount for one set of ids (measured: an uncached check cost 1.7 ms a call in FS-UAE,
     * +38% on a stat through the mount); a success only, kept until perm_epoch rises */
    struct mount *top = &mnt[mi];
    u32 ep = perm_epoch;
    if (top->ok_epoch == ep && top->ok_uid == current->uid && top->ok_gid == current->gid &&
        top->ok_grp == current->grp_id)
        return 0;
    while (mi > 0) {
        const char *mp = mnt[mi].path;
        int ml = (int)strlen_(mp), pm = 0, bl = 1;
        for (int i = 1; i < NMOUNT; i++) {      /* the mount mi's mount point lies on: the longest one above it */
            if (i == mi || !mnt[i].used) continue;
            int l = (int)strlen_(mnt[i].path);
            if (l > bl && l < ml && !memcmp_(mp, mnt[i].path, (u32)l) && mp[l] == '/') { pm = i; bl = l; }
        }
        const char *r = pm ? mp + bl : mp;
        int n = 0;
        while (r[n]) { buf[n] = r[n]; n++; }
        buf[n] = 0;
        struct ustat st;
        long e = fs_call_on(pm, FS_LSTAT, (u32)buf, mount_root(pm), (u32)&st, 0);
        if (e < 0)
            return (int)e;
        mi = pm;
    }
    if (perm_epoch == ep) {             /* nothing changed while the servers were asked */
        top->ok_epoch = ep; top->ok_uid = current->uid; top->ok_gid = current->gid; top->ok_grp = current->grp_id;
    }
    return 0;
}

/* ---- D26: a path argument's walk across mounts (docs/d26-links-across-mounts.md 5) ----
 * One per path argument of a system call, on the caller's kernel stack: the mount, its instance, the inode to give as
 * the working directory, the server's path (buf, PATHMAX bytes, or the caller's own text with only the root mounted),
 * the links followed for it (at most 40, Linux's MAXSYMLINKS), and whether an FS_OPEN lost its O_CREAT */
#define MAXLINKS 40
struct pwalk {
    int m;
    u32 inst, cwd;
    const char *rel;
    char *buf;
    u8 links, kcopy, nocreat;           /* kcopy: rel is buf, absolute and normalised: the request may carry UMSG_LINKS */
    int err;                            /* following a link failed: buf holds the server's raw text, never to be sent */
};

/* the mount that takes the absolute, normalised text in W's buf: W's mount, and its server's path moved down in W's
 * buf. 0, or -EACCES when a directory above the mount point may not be searched (F13) */
static __attribute__((noinline)) int pick(struct pwalk *w)
{
    char *tmp = w->buf;
    int best = 0, bl = 1, e;
    for (int i = 1; i < NMOUNT; i++) {
        if (!mnt[i].used) continue;
        int l = 0;
        while (mnt[i].path[l]) l++;
        if (l > bl && !memcmp_(tmp, mnt[i].path, (u32)l) && (tmp[l] == 0 || tmp[l] == '/')) { best = i; bl = l; }
    }
    if (best && (e = mount_search(best)))
        return e;
    if (best) {
        const char *r = tmp[bl] ? tmp + bl : "/";
        int n = 0;
        while (r[n]) { tmp[n] = r[n]; n++; }
        tmp[n] = 0;
    }
    w->rel = w->buf;
    w->kcopy = 1;
    w->cwd = mount_root(best);
    w->m = best;
    w->inst = mnt[best].inst;
    return 0;
}

/* PATH (from CWD) made absolute and normalised into W's buf (which PATH may be): the scratch text is off the stack again
 * before the servers are asked. 0 or -ENAMETOOLONG */
static __attribute__((noinline)) int walk_norm(struct pwalk *w, const char *cwd, const char *path)
{
    char tmp[PATHMAX];
    int e = path_abs(tmp, cwd, path);
    if (!e)
        strcpy_(w->buf, tmp);
    return e;
}

/* start W for PATH from the working directory CWDTEXT / CWDINO (fs_exec_open passes the parent's), with BUF for the
 * walk's text: PATHMAX bytes of the kernel's, and PATH's own buffer may be it (what a system call copied in: no second
 * buffer on the stack). With only the root mounted the path goes to its server untouched, unless COPY (chdir, mount:
 * always normalised, D26 5.6). 0, or -ENAMETOOLONG, -EACCES */
static __attribute__((noinline)) int walk_start(struct pwalk *w, const char *path, const char *cwdtext, u32 cwdino,
                                                char *buf, int copy)
{
    w->buf = buf;
    w->links = w->nocreat = 0;
    w->err = 0;
    if (nmnt <= 1 && !copy) {
        w->m = 0; w->inst = mnt[0].inst; w->rel = path; w->cwd = cwdino; w->kcopy = 0;
        return 0;
    }
    int e = walk_norm(w, cwdtext, path);
    return e ? e : pick(w);
}
#define WSTART(w_, path_, buf_) walk_start(&(w_), path_, current->cwd, current->cwd_ino, buf_, 0)

/* W's server answered "this path continues at TEXT" (in W's buf, untrusted): W moves on there. 0 or -errno */
static __attribute__((noinline)) int walk_follow(struct pwalk *w)
{
    if (current->killed)
        return -EINTR;
    if (++w->links > MAXLINKS)
        return -ELOOP;
    w->buf[PATHMAX - 1] = 0;
    if (!mnt[w->m].used || mnt[w->m].inst != w->inst)
        return -EIO;                    /* unmounted while the client waited: TEXT means nothing now */
    int e = walk_norm(w, w->buf[0] == '/' ? "/" : mnt[w->m].path, w->buf);
    return e ? e : pick(w);
}

/* the absolute text W's server path stands for (its mount point and the path below it) into OUT (PATHMAX) */
static __attribute__((noinline)) int walk_text(const struct pwalk *w, char *out)
{
    int n = 0;
    const char *p = w->m ? mnt[w->m].path : "";
    while (*p) out[n++] = *p++;
    p = w->rel;
    if (w->m && p[0] == '/' && !p[1]) p++;
    while (*p) {
        if (n >= PATHMAX - 1) return -ENAMETOOLONG;
        out[n++] = *p++;
    }
    if (!n) out[n++] = '/';
    out[n] = 0;
    return 0;
}

#define WC_AGAIN (-FS_LINKED)           /* wstep: a state moved on, send again (never a server's answer: fs_call_k) */
/* one request on W's path (W2: the second path of FS_RENAME and FS_LINK); X2, X3 the other arguments (FS_SYMLINK: X2
 * the target text). On an FS_LINKED answer the state it names follows the link: WC_AGAIN, or -errno (-EXDEV when the
 * two paths now lie on two mounts) */
static __attribute__((noinline)) long wstep(struct pwalk *w, struct pwalk *w2, u32 type, u32 x2, u32 x3)
{
    u32 a0 = (u32)w->rel, a1 = w->cwd, a2 = x2, a3 = x3;
    int bit = w->kcopy && (!w2 || w2->kcopy);
    if (type == FS_SYMLINK) { a0 = x2; a1 = (u32)w->rel; a2 = w->cwd; a3 = 0; }
    else if (w2) { a1 = (u32)w2->rel; a2 = w->cwd; a3 = 0; }
    else if (type == FS_OPEN && w->nocreat) a2 &= ~(u32)O_CREAT;
    long r = fs_call_k(w->m, type, a0, a1, a2, a3, bit ? UMSG_LINKS : 0);
    if (!bit || !fsl_is(r))
        return r;
    u32 bits = (u32)(-r - FS_LINKED);
    if (!fsl_ok(type, bits))
        return -EIO;                    /* the table of D26 4: a server may not rewrite that argument */
    if (type == FS_OPEN && (bits & FSL_LAST))
        w->nocreat = 1;                 /* a link as the last name: open what it leads to, never create it (5.5) */
    struct pwalk *moved = (bits & FSL_A1) && w2 ? w2 : w;
    int e = walk_follow(moved);
    if (e)
        return moved->err = e;
    return w2 && w->m != w2->m ? -EXDEV : WC_AGAIN;
}
static long wcall(struct pwalk *w, struct pwalk *w2, u32 type, u32 x2, u32 x3)
{
    long r;
    while ((r = wstep(w, w2, type, x2, x3)) == WC_AGAIN)
        ;
    return r;
}

static struct exec_src *loading;        /* the program loads in progress, linked through the spawners' kernel stacks */

/* ---- D13: what tells the shared code segments (seg.c) that a file may have changed ---- */
u32 vfs_mount_gen(int m)
{
    return m >= 0 && m < NMOUNT && mnt[m].used ? mnt[m].gen : 0;
}

/* the server on port PNAME was started (svc_handover): a restarted ZenixFS server drops uncommitted changes and could
 * bring back older contents under the same inode, size and time, so its mounts get a new generation */
void vfs_server_started(const char *pname)
{
    for (int i = 0; i < NMOUNT; i++)
        if (mnt[i].used && !strcmp_(mnt[i].pname, pname))
            mnt[i].gen = ++gen_last;
    perm_epoch++;                       /* F13: a restarted ZenixFS server may bring back older modes */
}

/* would writing the file with status st change a program in memory or being read (-ETXTBSY)? */
static __attribute__((noinline)) int exec_busy(int m, const struct ustat *st)
{
    if (seg_busy(m, st))
        return 1;
    for (struct exec_src *x = loading; x; x = x->next)
        if (x->m == m && x->ino == (long)st->ino)
            return 1;
    return 0;
}

/* file INO (-1: any file) of mount m may have changed: its segments are no longer found, its loads not listed */
static __attribute__((noinline)) void changed(int m, long ino)
{
    seg_unlist(m, ino);
    for (struct exec_src *x = loading; x; x = x->next)
        if (x->m == m && (ino < 0 || x->ino == ino))
            x->stale = 1;
}

/* D13, audit3 B2: the inode a name leads to now (0: none, or nothing is loaded from any mount), asked before an
 * unlink, a rename over it, or a link or symlink made there; after the call, changed(w->m, that inode) unlists only
 * its segments (its number may be given to a new file), so an rm or mv elsewhere on the mount keeps the sharing.
 * The FS_LSTAT resolves W (D26 5.2, 5.3): the request that follows starts there, on the mount that holds the name */
static __attribute__((noinline)) long name_ino(struct pwalk *w)
{
    struct ustat st;
    if (!seg_any() && !loading)
        return 0;
    if (wcall(w, 0, FS_LSTAT, (u32)&st, 0) < 0)
        return 0;
    return (long)st.ino;
}
static void unlist_ino(int m, long ino)
{
    if (ino > 0)
        changed(m, ino);
}

/* Open a program for loading without reading it into a buffer first: the loader reads the header, allocates the
 * process's memory and then reads the image straight into it (fs_exec_read). */
long fs_exec_open(const char *path, const struct proc *parent, struct exec_src *x)
{
    char rb[PATHMAX];
    struct pwalk w;
    int e = walk_start(&w, path, parent ? parent->cwd : "/", parent ? parent->cwd_ino : 2, rb, 0);
    if (e)
        return e;
    long ino = wcall(&w, 0, FS_OPEN, O_RDONLY | O_EXEC, (u32)&x->st);
    if (ino < 0)
        return ino;
    int m = w.m;                        /* D26: the mount the program is on, after its links (D13's key) */
    x->m = m;
    x->ino = ino;
    x->gen = mnt[m].gen;
    x->stale = 0;
    x->wopen = 0;
    for (int i = 0; i < NFILE; i++)     /* open for writing now: a ZEX2 program gets a private copy (D13) */
        if (files[i].type == FT_INODE && files[i].fs == m && files[i].ino == (u32)ino && (files[i].flags & (O_WRONLY | O_RDWR)))
            x->wopen = 1;
    x->next = loading;                  /* until fs_exec_close: a restarted server must count this open (D11) */
    loading = x;
    if (!S_ISREG(x->st.mode)) {         /* the size is proc_spawn's to check (an AmigaOS program may be larger) */
        fs_exec_close(x);
        return S_ISDIR(x->st.mode) ? -EISDIR : -EACCES;
    }
    return 0;
}

/* every successful fs_exec_open ends with this (the server counts opens, see FS_CLOSE) */
void fs_exec_close(struct exec_src *x)
{
    fs_call_on(x->m, FS_CLOSE, (u32)x->ino, 0, 0, 0);
    for (struct exec_src **q = &loading; *q; q = &(*q)->next)   /* after the close: it counted until then */
        if (*q == x) {
            *q = x->next;
            break;
        }
}

/* the opens the kernel holds on mount mi, served on port PORT: its file entries (also one being closed, refs already
 * 0: its FS_CLOSE is queued and takes it off again), the kernel's FS_OPENs answered but not yet used (the client has
 * not run since: no file entry yet; a root program's own FS_OPEN makes none and is not counted), the program loads in
 * progress. With OUT 0 (umount's busy check) every kernel request still in flight on the port counts too. The kernel
 * is not preempted, so the list is exact. The count (inode numbers in OUT), or -ENOSPC if more than max. */
static __attribute__((noinline)) long mount_opens(int mi, int port, u32 *out, u32 max)
{
    u32 n = 0;
#define PUT(v) do { if (n == max) return -ENOSPC; if (out) out[n] = (u32)(v); n++; } while (0)
    for (int i = 0; i < NFILE; i++)
        if (files[i].type == FT_INODE && files[i].fs == mi)
            PUT(files[i].ino);
    for (int i = 1; i < NPROC; i++) {
        struct umsg *m = procs[i].wait_msg;
        if (procs[i].state == P_FREE || !m || procs[i].wait_port != port || !(m->kflags & UMSG_KERNEL))
            continue;
        if (m->state == MSG_DONE ? m->type == FS_OPEN && m->result >= 0 : !out)
            PUT(m->result);
    }
    for (struct exec_src *x = loading; x; x = x->next)
        if (x->m == mi)
            PUT(x->ino);
    return (long)n;
#undef PUT
}

/* SVC_OPENS (D11 5.4): one inode number for every open the kernel holds on the mount served on port PNAME (number
 * PORT), mount_opens. -ENOSPC if more than max. */
long vfs_opens(const char *pname, int port, u32 *out, u32 max)
{
    int mi = -1;
    for (int i = 0; i < NMOUNT; i++)
        if (mnt[i].used && !strcmp_(mnt[i].pname, pname))
            mi = i;
    return mi < 0 ? 0 : mount_opens(mi, port, out, max);
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
 * /dev/ptmx (a new pty's master) and /dev/pts/0 .. /dev/pts/7 (its slave: root and its owner, DESIGN D20 7.2),
 * /dev/ttyS0 (the serial port, DESIGN D12), /dev/kmsg (root, write only: the kernel log, D24 6.5), /dev/null,
 * /dev/zero, /dev/urandom and /dev/random (DESIGN D19 9.3, random.c: off 1 waits until the pool is seeded) */
static long open_device(const char *path, int flags)
{
    int type = 0, con = 0;
    if (!strcmp_(path, "/dev/tty")) { type = FT_TTY; con = tty_of(current); }
    else if (!strcmp_(path, "/dev/ptmx")) {
        long i = pty_alloc();
        if (i < 0)
            return i;
        type = FT_PTM;
        con = (int)i;
    }
    else if (path[5] == 'p' && path[6] == 't' && path[7] == 's' && path[8] == '/' && path[9] >= '0' && path[9] <= '9' &&
             !path[10]) {
        long i = pty_slave_ok(path[9] - '0');
        if (i < 0)
            return i;
        type = FT_TTY;
        con = (int)i;
    }
    else if (!strcmp_(path, "/dev/ttyS0")) {        /* the serial port (DESIGN D12): root, one owner at a time */
        if (current->uid != 0)
            return -EACCES;
        long e = serdev_open();
        if (e < 0)
            return e;
        type = FT_SERIAL;
    }
    else if (!strcmp_(path, "/dev/kmsg")) {         /* DESIGN D24 6.5: root writes lines into the kernel log, as a
                                                     * service's output goes there (D11 step 8): init's daemons */
        if (current->uid != 0)
            return -EACCES;
        if ((flags & (O_WRONLY | O_RDWR)) != O_WRONLY)
            return -EINVAL;                 /* write only: reading the log is SYS_klog's (dmesg) */
        type = FT_KLOG;
    }
    else if (!strcmp_(path, "/dev/null")) type = FT_NULL;
    else if (!strcmp_(path, "/dev/zero")) type = FT_ZERO;
    else if (!strcmp_(path, "/dev/urandom")) type = FT_RANDOM;
    else if (!strcmp_(path, "/dev/random")) { type = FT_RANDOM; con = 1; }
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
    if (!f) {
        if (type == FT_SERIAL)
            serdev_close();
        if (type == FT_PTM)
            tty_gc(con);                    /* the new pty goes again */
        return -ENFILE;
    }
    f->off = (u32)con;
    f->flags = (u16)flags;
    f->mode = 020666;                       /* a character device */
    if (type == FT_TTY || type == FT_PTM)
        tty_opened(type, con);              /* a pty's open ends are counted (D20 7.2) */
    int fd = fd_install(current, f);
    if (fd < 0)
        file_close(f);
    return fd;
}

static int is_dev(const char *p)
{
    return p[0] == '/' && p[1] == 'd' && p[2] == 'e' && p[3] == 'v' && p[4] == '/';
}

long vfs_open(char *path, int flags)
{
    struct ustat st;
    char other[PATHMAX];                    /* the walk's buffers are PATH's and this scratch one (they swap) */
    char *rb[2] = { path, other };
    struct pwalk w;
    if (is_dev(path)) {
        long d = open_device(path, flags);
        if (d != -ENODEV)
            return d;
    }
    int e = WSTART(w, path, rb[0]);
    if (e)
        return e;
    /* D13: an open for writing of a program whose shared code is in memory or being read is refused, "text file
     * busy" (the safe update is a new file renamed over the old name). The server truncates in its FS_OPEN, so with
     * O_TRUNC the kernel asks first (only while something is loaded); without, it checks the answer. D26 5.2: the
     * FS_STAT resolves the path on a copy of the walk, which the FS_OPEN then starts from; a path that does not stat
     * (nothing there to be busy) opens from where it was, so a link to nothing keeps its -ENOENT (5.5) */
    int wr = flags & (O_WRONLY | O_RDWR);
    long ino;
    for (;;) {
        char *spare = w.buf == rb[0] ? rb[1] : rb[0];
        if (wr && (flags & O_TRUNC) && (seg_any() || loading)) {
            struct pwalk c = w;
            if (w.kcopy) {
                strcpy_(spare, w.buf);
                c.buf = spare; c.rel = spare;
            }
            if (wcall(&c, 0, FS_STAT, (u32)&st, 0) >= 0) {
                if (exec_busy(c.m, &st))
                    return -ETXTBSY;
                w = c;
                spare = w.buf == rb[0] ? rb[1] : rb[0];
            }
        }
        ino = wstep(&w, 0, FS_OPEN, (u32)flags, (u32)&st);
        if (ino != WC_AGAIN)
            break;
        if (!walk_text(&w, spare) && is_dev(spare)) {   /* a link to /dev/...: the kernel's devices first */
            long d = open_device(spare, flags);
            if (d != -ENODEV)
                return d;
        }
    }
    int m = w.m;
    if (ino < 0)
        return ino;                         /* the server's errno: -ENOENT, -EACCES, -EEXIST (O_CREAT|O_EXCL) ... */
    if (wr) {
        /* with O_TRUNC the check came before the open, which has truncated the file already: a load that began
         * since is not refused afterwards (that answered -ETXTBSY for a file already emptied, audit3 B5); it is
         * marked stale and a segment listed since is unlisted, so nothing read while the file changed is shared */
        if (!(flags & O_TRUNC) && exec_busy(m, &st)) {
            fs_call_on(m, FS_CLOSE, (u32)ino, 0, 0, 0);
            return -ETXTBSY;
        }
        changed(m, ino);
    }
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

/* stat, lstat (OP FS_STAT, FS_LSTAT); a caller's buffer of N >= USTAT_SIZE bytes gets the device after the 28 bytes the
 * server wrote (D49 8): the mount's slot + 1, of the mount the walk ended on */
long vfs_stat(u32 op, char *path, struct ustat *st, u32 n)
{
    struct pwalk w;
    int e = WSTART(w, path, path);
    long r = e ? e : wcall(&w, 0, op, (u32)st, 0);
    if (r >= 0 && n >= USTAT_SIZE)
        ((u32 *)st)[7] = mnt[w.m].inst;    /* never reused while the system runs (D26's instance; the root's is 1) */
    return r;
}

/* N >= USTAT_SIZE: the device too (D49 8; 0 for the kernel's own devices and sockets) */
long vfs_fstat(struct file *f, struct ustat *st, u32 n)
{
    if (n >= USTAT_SIZE && f->type != FT_PIPE_R && f->type != FT_PIPE_W)   /* (a pipe: -EINVAL, nothing written) */
        ((u32 *)st)[7] = f->type == FT_INODE ? mnt[f->fs].inst : 0;
    if (f->type == FT_TTY || f->type == FT_NULL || f->type == FT_ZERO || f->type == FT_SERIAL || f->type == FT_KLOG ||
        f->type == FT_RANDOM || f->type == FT_PTM) {
        memset(st, 0, sizeof *st);
        st->mode = 020666;
        st->links = 1;
        if (f->type == FT_TTY)
            st->ino = f->off;               /* a console's number (1..NCONS), a pty slave's tty number (PTY_FIRST + N,
                                             * D20 7.2): the ustat has no device field */
        if (f->type == FT_PTM)
            st->ino = PTY_MINO + f->off - PTY_FIRST;    /* a master's: ptsname reads N from it */
        return 0;
    }
    if (f->type == FT_SOCK) {               /* DESIGN D19: S_IFSOCK, as Linux shows a socket */
        memset(st, 0, sizeof *st);
        st->mode = S_IFSOCK | 0777;
        st->links = 1;
        return 0;
    }
    if (f->type != FT_INODE)
        return -EINVAL;                     /* a pipe: no stat */
    return fs_call_on(f->fs, FS_FSTAT, f->ino, (u32)st, 0, 0);
}

/* D26 5.6: always normalised and resolved, with only the root mounted too: the working directory's text is the
 * link-free one (Linux's getcwd), so a later ".." is the physical one and umount's busy check sees it */
long vfs_chdir(char *path)
{
    char text[PATHMAX];
    struct pwalk w;
    int e = walk_start(&w, path, current->cwd, current->cwd_ino, path, 1);
    if (e)
        return e;
    long ino = wcall(&w, 0, FS_CHDIR, 0, 0);
    if (ino < 0)
        return ino;
    if ((e = walk_text(&w, text)))
        return e;                       /* the new working directory's text must fit: else refused */
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

long vfs_path1(u32 op, char *p, u32 a2, u32 a3)
{
    struct pwalk w;
    int e = WSTART(w, p, p);
    return e ? e : wcall(&w, 0, op, a2, a3);
}

/* rename, link and symlink: name_ino's FS_LSTAT on the new name, then the request, both redone from where the
 * request's FS_LINKED left the walks (D26 5.2); two paths on two mounts, textually or after their links: -EXDEV */
static __attribute__((noinline)) long two_paths(u32 op, struct pwalk *a, struct pwalk *b, const char *text)
{
    long ino, r;
    if (a && a->m != b->m)
        return -EXDEV;                      /* not across filesystems: mv copies */
    do {
        ino = name_ino(b);                  /* D13: the name replaced or made (its inode may be reused) */
        if (b->err)
            return b->err;                  /* a link loop, too long: the walk's answer, not "no inode" */
        if (a && a->m != b->m)
            return -EXDEV;
        r = op == FS_SYMLINK ? wstep(b, 0, op, (u32)text, 0) : wstep(a, b, op, 0, 0);
    } while (r == WC_AGAIN);
    unlist_ino(b->m, ino);
    return r;
}

long vfs_link(u32 op, char *a, char *b)
{
    struct pwalk wa, wb;
    int e = op == FS_SYMLINK ? 0 : WSTART(wa, a, a);    /* FS_SYMLINK: a is the link text, only b is a path */
    if (e || (e = WSTART(wb, b, b)))
        return e;
    return op == FS_SYMLINK ? two_paths(op, 0, &wb, a) : two_paths(op, &wa, &wb, 0);
}

/* every mount's server writes out what it holds: the local ones first, the network mounts last (D23 5.4: a dead NAS
 * must not leave a local mount after it unsynced). With VS_REBOOT (SYS_reboot) each has at most 5 s: the tick resets
 * the machine after that, naming it (kbd_tick, audit A7). With VS_BOUNDED the network mounts get FS_SYNC a0 1: their
 * servers answer within 2 s */
long vfs_sync(int how)
{
    long r = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < NMOUNT; i++) {
            if (!mnt[i].used || mnt[i].net != pass)
                continue;
            if (how & VS_REBOOT) {
                reboot_who = mnt[i].pname;
                reboot_deadline = (ticks + 500) | 1;
            }
            if (fs_call_on(i, FS_SYNC, pass && (how & VS_BOUNDED) ? 1 : 0, 0, 0, 0) < 0 &&
                (i == 0 || (!pass && (how & VS_LOCALS))))
                r = -EIO;
        }
    reboot_deadline = 0;
    return r;
}

long vfs_mkdir(char *path)
{
    return vfs_path1(FS_MKDIR, path, 0, 0);
}

long vfs_unlink(char *path)
{
    struct pwalk w;
    long ino, r;
    int e = WSTART(w, path, path);
    if (e)
        return e;
    do {
        ino = name_ino(&w);                 /* D13: its inode number may be given to a new file */
        if (w.err)
            return w.err;                   /* a link loop, too long: the walk's answer, not "no inode" */
        r = wstep(&w, 0, FS_UNLINK, 0, 0);
    } while (r == WC_AGAIN);
    unlist_ino(w.m, ino);
    return r;
}

long vfs_rmdir(char *path)
{
    return vfs_path1(FS_RMDIR, path, 0, 0);
}

/* the file renamed over may be reused (D13); the moved one keeps its inode and contents, so its segments stay right */
long vfs_rename(char *from, char *to)
{
    return vfs_link(FS_RENAME, from, to);
}

long vfs_read(struct file *f, void *buf, u32 n)
{
    switch (f->type) {
    case FT_TTY:
        return tty_read((int)f->off, buf, n);
    case FT_PTM:
        return ptm_read((int)f->off, buf, n, (f->flags & O_NONBLOCK) != 0);
    case FT_GCON:
        return gcon_read(f, buf, n);
    case FT_PIPE_R:
        return pipe_read(f, buf, n);
    case FT_SERIAL:                         /* waits for at least one byte, then returns what has arrived */
        for (;;) {
            if (current->killed)
                return -EINTR;
            ev_arm(&serdev_wait);
            u32 got = serdev_take(buf, n);
            if (got)
                return (long)got;
            if (f->flags & O_NONBLOCK)
                return -EAGAIN;             /* netd (D19 5.3) */
            long e = ev_block(EV_IO, 0, 0, 1);
            if (e)
                return e;
        }
    case FT_NULL: case FT_KLOG:
        return 0;
    case FT_ZERO:
        memset(buf, 0, n);
        return (long)n;
    case FT_RANDOM:
        return rnd_read((int)f->off, (f->flags & O_NONBLOCK) != 0, buf, n);
    case FT_SOCK:
        return sock_rw(f, buf, n, 0);
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
    case FT_PTM:
        return ptm_write((int)f->off, buf, n, (f->flags & O_NONBLOCK) != 0);
    case FT_PIPE_W:
        return pipe_write(f, buf, n);
    case FT_SERIAL:
        return serdev_write(buf, n, (f->flags & O_NONBLOCK) != 0);
    case FT_KLOG:
        klog_write(buf, n);                 /* a service's output: in dmesg, not on a console */
        return (long)n;
    case FT_NULL: case FT_ZERO:
        return (long)n;                     /* the bit bucket */
    case FT_RANDOM:
        return rnd_write((int)f->off, buf, n);   /* mixed; root's write to /dev/random credits it (random.c) */
    case FT_SOCK:
        return sock_rw(f, (void *)buf, n, 1);
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

/* D49: SYS_readdirs. The buffer is the caller's (checked here); the server writes the records into it in place. A
 * name that is a mount point keeps its server's stat (that of the directory underneath, as its ino always was): libz
 * takes it out (docs/d49-readdir.md 4.2) */
long vfs_readdirs(struct file *f, u32 buf, u32 size)
{
    struct fsrdx q;
    if (size < UDIRX_MAX || size > 0xFFFF || (buf & 3))
        return -EINVAL;                 /* bits 16-31 are for request flags later; records are 4-aligned */
    if (!user_ok_call_wr(current, buf, size))
        return -EFAULT;
    if (f->type != FT_INODE || !S_ISDIR(f->mode))
        return -ENOTDIR;
    q.size = size;
    q.next = f->off;
    long r = fs_call_on(f->fs, FS_READDIRS, f->ino, f->off, buf, (u32)&q);
    if (r > (long)size || (r > 0 && q.next == f->off))
        return -EIO;                    /* more than asked, or records that do not move on: a loop for ever */
    if (r >= 0)
        f->off = q.next;
    return r;
}

/* ---- mount, umount ---- */

static int user_str_eq(const char *a, const char *b) { return !strcmp_(a, b); }

long vfs_mount(const struct umountreq *r)
{
    char abs[PATHMAX], rb[PATHMAX];
    struct ustat st;
    struct pwalk w;
    /* D26 5.6: the mount point resolved (its links followed, also its last name: it must be a directory), and every
     * check on the resolved text: a link to / is -EBUSY as / is */
    int e = walk_start(&w, r->path, current->cwd, current->cwd_ino, rb, 1);
    if (e)
        return e;
    if ((e = (int)wcall(&w, 0, FS_STAT, (u32)&st, 0)) < 0)
        return e;
    if (!S_ISDIR(st.mode))
        return -ENOTDIR;                        /* the mount point must be a directory */
    if ((e = walk_text(&w, abs)))
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
    mt->gen = ++gen_last;                   /* D13: nothing loaded from the slot's last mount is reused */
    mt->inst = ++inst_last;                 /* D26: an FS_LINKED answered for the slot's last mount is refused */
    mt->used = 1;
    struct fsinfo fi;
    memset(&fi, 0, sizeof fi);
    long root = fs_call_on(free, FS_ROOT, (u32)&fi, 0, 0, 0);
    if (root <= 0) {
        fs_call_on(free, FS_QUIT, 0, 0, 0, 0);     /* a server that runs but gave no root lets its device go (A9) */
        mt->used = 0;
        return root < 0 ? root : -EIO;
    }
    mt->net = (u8)net_type(&fi);
    mt->root = (u32)root;
    nmnt++;
    if (mmu_d41 && port_owner(port))
        mmu_sysmark(port_owner(port));          /* MMU=D41 (D60 12): D41 7's system process, once mounted */
    perm_epoch++;                       /* F13: a new mount can sit between a mount and its parent */
    return 0;
}

static int prefix_of(const char *dir, const char *path)         /* is PATH DIR or below it */
{
    int l = (int)strlen_(dir);
    return !memcmp_(path, dir, (u32)l) && (path[l] == 0 || path[l] == '/');
}

static int mount_at(const char *abs)
{
    for (int i = 1; i < NMOUNT; i++)
        if (mnt[i].used && user_str_eq(mnt[i].path, abs))
            return i;
    return -1;
}

/* D26 5.7: the normalised text against the mount table first, asking no server (a hung NFS mount, a stopped mount
 * service); only when nothing matches, the directory part resolved (FS_STAT, links followed) and the last name
 * joined as it is (lstat's rule), so umount works by both names and never walks into the mount it removes */
static __attribute__((noinline)) int umount_find(const char *path)
{
    char abs[PATHMAX], res[PATHMAX], last[sizeof mnt[0].path];
    struct pwalk w;
    struct ustat st;
    int e = path_abs(abs, current->cwd, path);
    if (e)
        return e;
    int mi = mount_at(abs);
    if (mi >= 0 || !abs[1])
        return mi;
    int slash = (int)strlen_(abs);
    while (abs[slash] != '/') slash--;
    abs[slash] = 0;
    int l = (int)strlen_(abs + slash + 1);
    if (l >= (int)sizeof last)
        return -1;                              /* longer than any mount point */
    strcpy_(last, abs + slash + 1);
    if (!slash) { abs[0] = '/'; abs[1] = 0; }
    if ((e = walk_start(&w, abs, "/", 2, abs, 1)) || wcall(&w, 0, FS_STAT, (u32)&st, 0) < 0 ||
        !S_ISDIR(st.mode) || walk_text(&w, res))
        return -1;
    int n = (int)strlen_(res);
    if (n == 1) n = 0;
    if (n + 1 + l >= PATHMAX)
        return -1;
    res[n] = '/';
    strcpy_(res + n + 1, last);
    return mount_at(res);
}

long vfs_umount(const char *path)
{
    int mi = umount_find(path);
    if (mi == -ENAMETOOLONG)
        return mi;
    if (mi < 0)
        return -EINVAL;                         /* nothing is mounted there */
    for (int i = 1; i < NMOUNT; i++)                    /* busy: another filesystem is mounted below it */
        if (i != mi && mnt[i].used && prefix_of(mnt[mi].path, mnt[i].path))
            return -EBUSY;
    for (int i = 1; i < NPROC; i++)                     /* busy: a working directory inside */
        if (procs[i].state != P_FREE && prefix_of(mnt[mi].path, procs[i].cwd))
            return -EBUSY;
    /* ... an open file, an open answered but not yet a file, a program being loaded from it, a request in flight
     * (audit A3: a load that went on after umount read a new mount's inode with the same number) */
    if (mount_opens(mi, mnt[mi].port >= 0 ? mnt[mi].port : port_find(mnt[mi].pname), 0, 0))
        return -EBUSY;
    /* the mount's service goes first (D11 6.4): the server's end after FS_QUIT is final, no call starts it again */
    if (svc_sys(SVC_REMOVE, mnt[mi].pname, 0) == -EBUSY)
        return -EBUSY;                                  /* its server is being started right now */
    fs_call_on(mi, FS_QUIT, 0, 0, 0, 0);               /* the server syncs, lets go of its device and exits */
    mnt[mi].used = 0;
    nmnt--;
    perm_epoch++;                                       /* F13: the mount point's parent is another volume now */
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
