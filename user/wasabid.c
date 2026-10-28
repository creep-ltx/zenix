/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid (DESIGN D24, docs/d24-remote.md; docs/man/wasabid.txt): the remote control daemon, /sbin/wasabid. Its core
 * is net/wasabi/ (no system calls, shared with the PC tests, tools/host/wasabi-test.sh); this file is the core's OS
 * layer on Zenix (net/wasabi/wasabi.h "what the OS layer gives the core"), through libz and the system calls, as
 * tools/host/wasabi-host.c is on Linux. Built at -Os: all of it is cold code (the main session, D24 step 0).
 *
 * The descriptors: the core counts its own against 16 (D24 5.1) with the log as one, so standard input and output
 * are closed at the start and standard error is the log (the console when started by hand, /dev/kmsg under init).
 * The console operations of D24 6.1-6.3 come with step 3: until then wsb_os_con_ops() is 0 and caps leave the screen
 * commands out. */
#include "socket.h"
#include "../net/wasabi/wasabi.h"

static int lfd = -1;                    /* the listener: SOCK_IFLIST is asked through it (no descriptor of its own) */

uint32_t wsb_os_now(void) { return sys_ticks() * 10u; }
uint32_t wsb_os_uptime(void) { return sys_uptime(); }
void wsb_os_log(const char *line)
{
    char b[200];
    u32 n = strlen(line);
    if (n > sizeof b - 1) n = sizeof b - 1;
    memcpy(b, line, n);
    b[n++] = '\n';
    sys_write(2, b, n);                 /* one write: a whole line in the kernel log */
}
/* The core's heap: libz's region heap (rheap_alloc), not malloc: malloc rounds a block up to a power of two and grows its
 * chunks four times over, which held 112 KB in `ps` for one connection (measured in FS-UAE, D24 step 1) */
void *wsb_os_alloc(uint32_t n) { return rheap_alloc(n); }
void wsb_os_free(void *p) { rheap_free(p); }
const char *wsb_os_strerror(int e) { return zstrerror(e); }

int wsb_os_poll(struct wsb_pollfd *f, int n, int ms)
{
    int r = poll((struct pollfd *)f, (u32)n, ms);
    return r < 0 ? -zerrno : r;
}

static int sock(int type, unsigned port)
{
    struct sockaddr_in a;
    int on = 1, s = socket(AF_INET, type | SOCK_NONBLOCK, 0);
    if (s < 0)
        return -zerrno;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || (type == SOCK_STREAM && listen(s, 4) < 0)) {
        int e = -zerrno;
        sys_close(s);
        return e;
    }
    return s;
}
int wsb_os_listen(unsigned port)
{
    int s = sock(SOCK_STREAM, port);
    if (s >= 0)
        lfd = s;
    return s;
}
int wsb_os_udp(unsigned port) { return sock(SOCK_DGRAM, port); }
int wsb_os_accept(int l, uint32_t *peer)
{
    struct sockaddr_in a;
    socklen_t n = sizeof a;
    int fd = accept4(l, (struct sockaddr *)&a, &n, SOCK_NONBLOCK);
    if (fd < 0)
        return -zerrno;
    *peer = ntohl(a.sin_addr.s_addr);
    return fd;
}
long wsb_os_recv(int fd, void *b, long n) { int r = recv(fd, b, (u32)n, 0); return r < 0 ? -zerrno : r; }
long wsb_os_send(int fd, const void *b, long n) { int r = send(fd, b, (u32)n, MSG_NOSIGNAL); return r < 0 ? -zerrno : r; }
long wsb_os_recvfrom(int fd, void *b, long n, uint32_t *addr, unsigned *port)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int r = recvfrom(fd, b, (u32)n, 0, (struct sockaddr *)&a, &l);
    if (r < 0)
        return -zerrno;
    *addr = ntohl(a.sin_addr.s_addr);
    *port = ntohs(a.sin_port);
    return r;
}
long wsb_os_sendto(int fd, const void *b, long n, uint32_t addr, unsigned port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(addr);
    int r = sendto(fd, b, (u32)n, MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof a);
    return r < 0 ? -zerrno : r;
}
int wsb_os_shutdown(int fd) { return shutdown(fd, SHUT_WR) < 0 ? -zerrno : 0; }
void wsb_os_close(int fd)
{
    if (fd == lfd)
        lfd = -1;
    sys_close(fd);
}

/* ---- files ---- */
int wsb_os_open(const char *path, int flags, uint32_t mode)
{
    char r[160];
    /* the serial port is taken by opening it (D12: one owner; SLIP's line): never for a file command, under any name */
    if (!zrealpath(path, r, sizeof r) && !strcmp(r, "/dev/ttyS0"))
        return -EBUSY;
    int fl = flags == WSB_O_READ ? O_RDONLY : flags == WSB_O_WRITE ? O_WRONLY | O_CREAT | O_EXCL : O_WRONLY | O_CREAT | O_APPEND;
    int fd = sys_open(path, fl | O_NONBLOCK);
    if (fd >= 0 && flags == WSB_O_WRITE)
        sys_chmod(path, mode);          /* a new file (O_EXCL): the mode asked for */
    return fd;
}
long wsb_os_read(int fd, void *b, long n) { return sys_read(fd, b, (u32)n); }
long wsb_os_write(int fd, const void *b, long n) { return sys_write(fd, b, (u32)n); }
static void tost(const struct ustat *s, struct wsb_stat *st)
{
    st->mode = s->mode;
    st->size = s->size;
    st->mtime = s->mtime;
    st->uid = s->uid;
}
int wsb_os_fstat(int fd, struct wsb_stat *st)
{
    struct ustat s;
    int e = sys_fstat(fd, &s);
    if (!e) tost(&s, st);
    return e < 0 ? e : 0;
}
int wsb_os_stat(const char *p, struct wsb_stat *st)
{
    struct ustat s;
    int e = sys_stat(p, &s);
    if (!e) tost(&s, st);
    return e < 0 ? e : 0;
}
int wsb_os_lstat(const char *p, struct wsb_stat *st)
{
    struct ustat s;
    int e = sys_lstat(p, &s);
    if (!e) tost(&s, st);
    return e < 0 ? e : 0;
}
int wsb_os_rename(const char *a, const char *b) { return sys_rename(a, b); }
int wsb_os_unlink(const char *p) { return sys_unlink(p); }
int wsb_os_rmdir(const char *p) { return sys_rmdir(p); }
int wsb_os_mkdir(const char *p) { return sys_mkdir(p); }
int wsb_os_chmod(const char *p, uint32_t m) { return sys_chmod(p, m); }
int wsb_os_opendir(const char *p) { return sys_open(p, O_RDONLY); }
int wsb_os_readdir(int fd, char name[64])
{
    struct udirent d;
    int r;
    while ((r = sys_readdir(fd, &d)) > 0) {
        d.name[63] = 0;
        if (strcmp(d.name, ".") && strcmp(d.name, "..")) {
            strcpy(name, d.name);
            return 1;
        }
    }
    return r;
}
int wsb_os_realpath(const char *path, char *out, int cap)
{
    return zrealpath(path, out, (u32)cap) < 0 ? -ENAMETOOLONG : 0;
}
int wsb_os_mount(unsigned i, struct wsb_mount *m)
{
    struct umountinfo mi;
    if (sys_mountinfo(i, &mi) != 0)
        return -ENOENT;
    memcpy(m->path, mi.path, sizeof m->path);
    m->path[sizeof m->path - 1] = 0;
    memcpy(m->type, mi.type, sizeof m->type);
    m->type[sizeof m->type - 1] = 0;
    return 0;
}
int wsb_os_statfs(unsigned mount, struct wsb_statfs *f)
{
    struct ustatfs s;
    int e = sys_statfs_mount(&s, mount);
    if (e < 0)
        return e;
    f->bsize = s.bsize;
    f->blocks = s.blocks;
    f->bfree = s.bfree;
    return 0;
}

/* ---- processes ---- */
int wsb_os_spawn(const char *const *argv, int in, int out, int err)
{
    int io[3] = { in, out, err };
    int pid = sys_spawn(argv[0], (char *const *)argv, io);
    if (pid > 0)
        sys_setpgid(pid, pid);          /* a group of its own (D16: FREE and the slot's end signal the group) */
    return pid;
}
int wsb_os_pipe(int fds[2]) { return sys_pipe(fds); }
int wsb_os_wait(int pid)
{
    int st = sys_waitnb(pid);
    return st == -2 ? -EAGAIN : st == -1 ? -ECHILD : st;
}
int wsb_os_kill(int pid, int sig) { return sys_killsig(pid, sig); }
int wsb_os_proc(unsigned i, struct wsb_proc *p)
{
    struct uproc u;
    memset(&u, 0, sizeof u);
    int e = sys_procinfo(i, &u);
    if (e < 0)
        return -EINVAL;
    if (!u.state)
        return 1;
    p->pid = u.pid;
    p->ppid = u.ppid;
    p->pgid = u.pgid;
    p->state = u.state;
    p->nice = u.nice;
    memcpy(p->name, u.name, sizeof p->name);
    p->name[15] = 0;
    return 0;
}
int wsb_os_self(void) { return sys_getpid(); }

/* ---- the machine ---- */
int wsb_os_meminfo(uint32_t *chip, uint32_t *fast)
{
    u32 m[4];
    int e = sys_meminfo(m);
    if (e < 0)
        return e;
    *chip = m[0];
    *fast = m[1];
    return 0;
}
int wsb_os_uname(char *out, int cap)
{
    struct uupdate u;
    char num[12], *q = num + sizeof num;
    if (sys_update(UPD_INFO, (u32)&u, sizeof u) < 0)
        return -ENOSYS;
    u.kver[15] = 0;
    *--q = 0;
    do *--q = (char)('0' + u.kvnum % 10); while ((u.kvnum /= 10) > 0);
    if (cap < 80)
        return -ENAMETOOLONG;
    strcpy(out, "Zenix ");              /* uname -a's line (no printf: the core has its own formatter) */
    strcat(out, u.kver);
    strcat(out, " (");
    strcat(out, q);
    strcat(out, ") m68k (68020+, no MMU)");
    return 0;
}
int wsb_os_ifaces(struct wsb_iface *t, int max)
{
    struct unetif rows[4];
    struct usockreq r;
    int s = lfd, own = 0, n;
    if (s < 0) {
        if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
            return 0;
        own = 1;
    }
    memset(&r, 0, sizeof r);
    r.buf = (u32)rows;
    r.len = sizeof rows;
    r.flags = sizeof rows[0];
    n = sys_sock(SOCK_IFLIST | (u32)sizeof r << 16, s, &r);
    if (own)
        sys_close(s);
    if (n < 0)
        return 0;
    if (n > 4) n = 4;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memcpy(t[i].name, rows[i].name, sizeof t[i].name);
        t[i].name[sizeof t[i].name - 1] = 0;
        t[i].addr = ntohl(rows[i].addr);
        t[i].rx = rows[i].rx_frames;
        t[i].tx = rows[i].tx_frames;
        t[i].bad = rows[i].rx_bad;
    }
    return n;
}
int wsb_os_klog(uint32_t off, void *buf, uint32_t n) { int r = sys_klog(off, buf, n); return r < 0 ? 0 : r; }

/* ---- the consoles: D24 step 3 ---- */
int wsb_os_con_ops(void) { return 0; }
int wsb_os_con_read(int con, struct wsb_con_head *h, uint8_t *cells, uint32_t cap)
{ (void)con; (void)h; (void)cells; (void)cap; return -ENOSYS; }
int wsb_os_con_line(int con, int line, uint8_t *pens, uint32_t cap, struct wsb_con_disp *d)
{ (void)con; (void)line; (void)pens; (void)cap; (void)d; return -ENOSYS; }
int wsb_os_con_input(int con, int kind, const uint8_t *b, int n)
{ (void)con; (void)kind; (void)b; (void)n; return -ENOSYS; }

int main(int argc, char **argv)
{
    static const char *const env[] = WSB_ENV;
    char blk[96];
    u32 n = 0;
    for (int i = 0; env[i]; i++) {      /* the commands' environment, set once and inherited by every spawn (5.1) */
        u32 l = strlen(env[i]);
        memcpy(blk + n, env[i], l + 1);
        n += l + 1;
    }
    blk[n++] = 0;
    sys_envset(blk, n);
    signal(SIGPIPE, SIG_IGN);
    sys_close(0);                       /* the core's budget: the log (standard error) is its only inherited one */
    sys_close(1);
    int st = wsb_init(argc, argv);
    while (st == WSB_GOON)
        st = wsb_step();
    return st;
}
