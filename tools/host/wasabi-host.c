/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid's core (net/wasabi/, DESIGN D24 step 0) on Linux: an OS layer over Linux's calls, for
 * tools/host/wasabi-test.sh and its conformance client tools/host/wasabi-check.py.
 *   wasabi-host --root DIR --offset N [--peer A.B.C.D] [--noconops] [--] [wasabid's arguments]
 * Zenix paths are DIR + path. TCP and UDP ports are the daemon's + N, on 127.0.0.1 only. --peer: every caller looks
 * like that address (the address filter's test). The machine is played from files in DIR:
 *   .klog .klog-prev .klog-oldest   the kernel log (this boot's, the previous boot's, the oldest offset the ring holds)
 *   .procs                          extra processes, "PID PPID NAME" lines (the guard's and KILL's tests)
 *   .kills                          kill()s of pretend processes are written here, "PID SIG"
 *   .conwrite                       its text is written to console 1 at the next look (then removed)
 * The pretend processes are boot (1), init (2), netd (3) and fsd (4); then wasabid itself and its children. Ten
 * consoles, 1 and 2 in use, 1 shown, 80 x 32; a typed line "echo WORDS" is answered with WORDS on the next line. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../net/wasabi/wasabi.h"

static char root[4096];
static unsigned offset;
static uint32_t peer_override;
static int conops = 1;
static struct timespec t0;

static const char *hp(const char *p)
{
    static char b[4][4200];
    static int k;
    if (!strcmp(p, "/dev/null"))
        return p;                               /* the real one */
    k = (k + 1) & 3;
    snprintf(b[k], sizeof b[k], "%s%s", root, p);
    return b[k];
}

uint32_t wsb_os_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
uint32_t wsb_os_uptime(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec - t0.tv_sec);
}
void wsb_os_log(const char *line) { fprintf(stderr, "%s\n", line); }
void *wsb_os_alloc(uint32_t n) { return malloc(n ? n : 1); }
void wsb_os_free(void *p) { free(p); }
const char *wsb_os_strerror(int e) { return strerror(e < 0 ? -e : e); }

int wsb_os_poll(struct wsb_pollfd *f, int n, int ms)
{
    int r = poll((struct pollfd *)f, (nfds_t)n, ms);
    return r < 0 ? -errno : r;
}

static int sock(int type, unsigned port)
{
    int s = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), on = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)(port + offset)) };
    if (s < 0)
        return -errno;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || (type == SOCK_STREAM && listen(s, 4))) {
        int e = -errno;
        close(s);
        return e;
    }
    return s;
}
int wsb_os_listen(unsigned port) { return sock(SOCK_STREAM, port); }
int wsb_os_udp(unsigned port) { return sock(SOCK_DGRAM, port); }
int wsb_os_accept(int lfd, uint32_t *peer)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int fd = accept4(lfd, (struct sockaddr *)&a, &l, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return errno == EWOULDBLOCK ? -EAGAIN : -errno;
    *peer = peer_override ? peer_override : ntohl(a.sin_addr.s_addr);
    return fd;
}
long wsb_os_recv(int fd, void *b, long n)
{
    long r = recv(fd, b, (size_t)n, 0);
    return r < 0 ? (errno == EWOULDBLOCK ? -EAGAIN : -errno) : r;
}
long wsb_os_send(int fd, const void *b, long n)
{
    long r = send(fd, b, (size_t)n, MSG_NOSIGNAL);
    return r < 0 ? (errno == EWOULDBLOCK ? -EAGAIN : -errno) : r;
}
long wsb_os_recvfrom(int fd, void *b, long n, uint32_t *addr, unsigned *port)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    long r = recvfrom(fd, b, (size_t)n, 0, (struct sockaddr *)&a, &l);
    if (r < 0)
        return errno == EWOULDBLOCK ? -EAGAIN : -errno;
    *addr = peer_override ? peer_override : ntohl(a.sin_addr.s_addr);
    *port = ntohs(a.sin_port);
    return r;
}
long wsb_os_sendto(int fd, const void *b, long n, uint32_t addr, unsigned port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(peer_override ? INADDR_LOOPBACK : addr);
    long r = sendto(fd, b, (size_t)n, MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof a);
    return r < 0 ? -errno : r;
}
int wsb_os_shutdown(int fd) { return shutdown(fd, SHUT_WR) ? -errno : 0; }

static DIR *dirs[1024];
void wsb_os_close(int fd)
{
    if (fd >= 0 && fd < 1024 && dirs[fd]) {
        closedir(dirs[fd]);
        dirs[fd] = 0;
        return;
    }
    close(fd);
}

int wsb_os_open(const char *path, int flags, uint32_t mode)
{
    int f = flags == WSB_O_READ ? O_RDONLY : flags == WSB_O_WRITE ? O_WRONLY | O_CREAT | O_EXCL : O_WRONLY | O_CREAT | O_APPEND;
    int fd = open(hp(path), f | O_NONBLOCK | O_CLOEXEC, mode);
    return fd < 0 ? -errno : fd;
}
long wsb_os_read(int fd, void *b, long n)
{
    long r = read(fd, b, (size_t)n);
    return r < 0 ? (errno == EWOULDBLOCK ? -EAGAIN : -errno) : r;
}
long wsb_os_write(int fd, const void *b, long n)
{
    long r = write(fd, b, (size_t)n);
    return r < 0 ? -errno : r;
}
static void tost(const struct stat *s, struct wsb_stat *st)
{
    st->mode = s->st_mode;
    st->size = (uint32_t)s->st_size;
    st->mtime = (uint32_t)s->st_mtime;
    st->uid = 0;                                /* every file of the pretend root is root's */
}
int wsb_os_fstat(int fd, struct wsb_stat *st)
{
    struct stat s;
    if (fstat(fd, &s))
        return -errno;
    tost(&s, st);
    return 0;
}
int wsb_os_stat(const char *p, struct wsb_stat *st)
{
    struct stat s;
    if (stat(hp(p), &s))
        return -errno;
    tost(&s, st);
    return 0;
}
int wsb_os_lstat(const char *p, struct wsb_stat *st)
{
    struct stat s;
    if (lstat(hp(p), &s))
        return -errno;
    tost(&s, st);
    return 0;
}
int wsb_os_rename(const char *a, const char *b) { return rename(hp(a), hp(b)) ? -errno : 0; }
int wsb_os_unlink(const char *p) { return unlink(hp(p)) ? -errno : 0; }
int wsb_os_rmdir(const char *p) { return rmdir(hp(p)) ? -errno : 0; }
int wsb_os_mkdir(const char *p) { return mkdir(hp(p), 0755) ? -errno : 0; }
int wsb_os_chmod(const char *p, uint32_t m) { return chmod(hp(p), m) ? -errno : 0; }
int wsb_os_opendir(const char *p)
{
    DIR *d = opendir(hp(p));
    if (!d)
        return -errno;
    int fd = dirfd(d);
    if (fd >= 1024) { closedir(d); return -EMFILE; }
    dirs[fd] = d;
    return fd;
}
int wsb_os_readdir(int fd, char name[64])
{
    struct dirent *e;
    if (fd < 0 || fd >= 1024 || !dirs[fd])
        return -EBADF;
    while ((e = readdir(dirs[fd])))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            snprintf(name, 64, "%.63s", e->d_name);
            return 1;
        }
    return 0;
}
int wsb_os_realpath(const char *p, char *out, int cap)
{
    char r[PATH_MAX];
    size_t l = strlen(root);
    if (!realpath(hp(p), r))
        return -errno;
    if (strncmp(r, root, l) || (r[l] && r[l] != '/'))
        return -EACCES;                         /* outside the pretend root */
    snprintf(out, (size_t)cap, "%s", r[l] ? r + l : "/");
    return 0;
}
int wsb_os_mount(unsigned i, struct wsb_mount *m)
{
    struct stat s;
    memset(m, 0, sizeof *m);
    if (i == 0) { strcpy(m->path, "/"); strcpy(m->type, "ext2"); return 0; }
    if (i == 1 && !stat(hp("/nas"), &s)) { strcpy(m->path, "/nas"); strcpy(m->type, "nfs"); return 0; }
    return -ENOENT;
}
int wsb_os_statfs(unsigned mount, struct wsb_statfs *f)
{
    struct statvfs v;
    if (mount > 1 || statvfs(root, &v))
        return -EINVAL;
    f->bsize = 1024;
    f->blocks = (uint32_t)(v.f_blocks * (v.f_frsize / 1024));
    uint64_t fr = v.f_bavail * (v.f_frsize / 1024);
    f->bfree = fr > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)fr;
    if (getenv("WASABI_HOST_FULL"))
        f->bfree = 100;                         /* a nearly full disk, for SPEED's room check */
    return 0;
}

/* ---- processes ---- */
#define NKIDS 64
static struct { int pid; char name[16]; } kids[NKIDS];

int wsb_os_spawn(const char *const *argv, int in, int out, int err)
{
    const char *env[] = WSB_ENV;
    char prog[4200];
    struct stat s;
    snprintf(prog, sizeof prog, "%s", stat(hp(argv[0]), &s) ? argv[0] : hp(argv[0]));
    int pid = fork();
    if (pid < 0)
        return -errno;
    if (!pid) {
        setpgid(0, 0);
        dup2(in, 0);
        dup2(out, 1);
        dup2(err, 2);
        for (int fd = 3; fd < 1024; fd++)
            close(fd);
        if (chdir(root))
            _exit(127);
        signal(SIGPIPE, SIG_DFL);
        execve(prog, (char *const *)argv, (char *const *)env);
        _exit(127);
    }
    setpgid(pid, pid);
    for (int i = 0; i < NKIDS; i++)
        if (!kids[i].pid) {
            const char *b = strrchr(argv[0], '/');
            kids[i].pid = pid;
            snprintf(kids[i].name, sizeof kids[i].name, "%s", b ? b + 1 : argv[0]);
            break;
        }
    return pid;
}
int wsb_os_pipe(int fds[2]) { return pipe2(fds, O_CLOEXEC) ? -errno : 0; }
int wsb_os_wait(int pid)
{
    int st, r = waitpid(pid, &st, WNOHANG);
    if (r == 0)
        return -EAGAIN;
    if (r < 0)
        return -errno;
    for (int i = 0; i < NKIDS; i++)
        if (kids[i].pid == pid)
            kids[i].pid = 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 255;
}

static int pretend(int pid, char *name, int *ppid)
{
    static const char *const fixed[] = { "boot", "init", "netd", "fsd" };
    if (pid >= 1 && pid <= 4) {
        if (name) strcpy(name, fixed[pid - 1]);
        if (ppid) *ppid = pid == 1 ? 0 : pid == 2 ? 1 : 2;
        return 1;
    }
    FILE *f = fopen(hp("/.procs"), "r");
    int p, pp, hit = 0;
    char n[32];
    while (f && fscanf(f, "%d %d %31s", &p, &pp, n) == 3)
        if (p == pid) {
            if (name) snprintf(name, 16, "%.15s", n);
            if (ppid) *ppid = pp;
            hit = 1;
            break;
        }
    if (f)
        fclose(f);
    return hit;
}
int wsb_os_kill(int pid, int sig)
{
    int t = pid < 0 ? -pid : pid;
    if (pretend(t, 0, 0)) {
        FILE *f = fopen(hp("/.kills"), "a");
        if (f) { fprintf(f, "%d %d\n", pid, sig); fclose(f); }
        return 0;
    }
    return kill(pid, sig) ? -errno : 0;
}
int wsb_os_proc(unsigned i, struct wsb_proc *p)
{
    static int extra[32], nextra;
    memset(p, 0, sizeof *p);
    if (i < 4) {
        int pp;
        p->pid = i + 1;
        pretend((int)i + 1, p->name, &pp);
        p->ppid = (uint32_t)pp;
        p->state = 3;
        return 0;
    }
    if (i == 4) {                               /* the extra ones of .procs, read once per walk */
        FILE *f = fopen(hp("/.procs"), "r");
        int a, b;
        char n[32];
        nextra = 0;
        while (f && nextra < 32 && fscanf(f, "%d %d %31s", &a, &b, n) == 3)
            extra[nextra++] = a;
        if (f)
            fclose(f);
        p->pid = (uint32_t)getpid();
        p->ppid = 2;
        p->state = 1;
        strcpy(p->name, "wasabid");
        return 0;
    }
    if (i < 5u + (unsigned)nextra) {
        int pp;
        p->pid = (uint32_t)extra[i - 5];
        pretend(extra[i - 5], p->name, &pp);
        p->ppid = (uint32_t)pp;
        p->state = 3;
        return 0;
    }
    unsigned k = i - 5 - (unsigned)nextra;
    if (k >= NKIDS)
        return -EINVAL;
    if (!kids[k].pid)
        return 1;
    p->pid = (uint32_t)kids[k].pid;
    p->ppid = (uint32_t)getpid();
    p->pgid = p->pid;
    p->state = 3;
    memcpy(p->name, kids[k].name, 16);
    return 0;
}
int wsb_os_self(void) { return getpid(); }

int wsb_os_meminfo(uint32_t *chip, uint32_t *fast) { *chip = 1187840; *fast = 0; return 0; }
int wsb_os_uname(char *out, int cap) { snprintf(out, (size_t)cap, "Zenix 0.3.1 (12) m68k (68020+, no MMU)"); return 0; }
int wsb_os_ifaces(struct wsb_iface *t, int max)
{
    if (max < 1)
        return 0;
    memset(t, 0, sizeof *t);
    strcpy(t->name, "sl0");
    t->addr = 0x0A00020F;
    t->rx = 100; t->tx = 200; t->bad = 1;
    return 1;
}
int wsb_os_klog(uint32_t off, void *buf, uint32_t n)
{
    uint32_t oldest = 0;
    FILE *f = fopen(hp("/.klog-oldest"), "r");
    if (f) { if (fscanf(f, "%u", &oldest) != 1) oldest = 0; fclose(f); }
    if (off & WSB_KLOG_PREV)
        oldest = 0;
    if (off & WSB_KLOG_OLDEST)
        return (int)oldest;
    int fd = open(hp(off & WSB_KLOG_PREV ? "/.klog-prev" : "/.klog"), O_RDONLY);
    off &= 0x3FFFFFFFu;
    if (off < oldest)
        off = oldest;                           /* as the kernel: below the oldest reads from it */
    if (fd < 0)
        return 0;
    long r = pread(fd, buf, n, off);
    close(fd);
    return r < 0 ? 0 : (int)r;
}

/* ---- the pretend consoles ---- */
#define ROWS 32
#define COLS 80
static struct { int used, crow, ccol; uint32_t changes; uint8_t cell[ROWS * COLS * 3]; } con[WSB_NCONS + 1];
static int front = 1, amiga_down;

static void con_clear(int n)
{
    for (int i = 0; i < ROWS * COLS; i++) {
        con[n].cell[3 * i] = ' ';
        con[n].cell[3 * i + 1] = 0;
        con[n].cell[3 * i + 2] = 7;
    }
    con[n].crow = con[n].ccol = 0;
    con[n].used = 1;
}
static void con_putc(int n, int ch)
{
    if (ch == '\r' || ch == '\n') {
        char line[COLS + 1];
        int r = con[n].crow, k = 0;
        for (int x = 0; x < COLS; x++)
            line[x] = (char)con[n].cell[3 * (r * COLS + x)];
        line[COLS] = 0;
        while (k < COLS && line[k] != ' ')
            k++;
        con[n].ccol = 0;
        if (++con[n].crow == ROWS) {
            memmove(con[n].cell, con[n].cell + 3 * COLS, 3 * COLS * (ROWS - 1));
            for (int x = 0; x < COLS; x++) { uint8_t *c = con[n].cell + 3 * ((ROWS - 1) * COLS + x); c[0] = ' '; c[1] = 0; c[2] = 7; }
            con[n].crow = ROWS - 1;
        }
        char *ec = strstr(line, "echo ");
        if (ec) {                               /* the pretend shell: "... echo WORDS" answers WORDS */
            int e = COLS;
            while (e > 0 && line[e - 1] == ' ')
                e--;
            for (int x = (int)(ec - line) + 5; x < e; x++)
                con_putc(n, line[x]);
            con_putc(n, '\n');
        }
        con[n].changes++;
        return;
    }
    if (con[n].ccol < COLS) {
        uint8_t *c = con[n].cell + 3 * (con[n].crow * COLS + con[n].ccol++);
        c[0] = (uint8_t)ch;
        c[1] = 0;
        c[2] = (uint8_t)(1 + (ch & 3)) | 0x00;
    }
    con[n].changes++;
}
static void con_puts(int n, const char *s) { while (*s) con_putc(n, (unsigned char)*s++); }
static void con_setup(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    con_clear(1);
    con_puts(1, "Zenix 0.3.1 tty1\n\nlogin: ");
    con_clear(2);
    con_puts(2, "Zenix 0.3.1 tty2\n\nlogin: ");
}
int wsb_os_con_ops(void) { return conops; }
int wsb_os_con_read(int n, struct wsb_con_head *h, uint8_t *cells, uint32_t cap)
{
    con_setup();
    if (n < 1 || n > WSB_NCONS)
        return -EINVAL;
    if (n == 1) {
        FILE *f = fopen(hp("/.conwrite"), "r");
        if (f) {
            int ch;
            while ((ch = fgetc(f)) != EOF)
                con_putc(1, ch);
            fclose(f);
            unlink(hp("/.conwrite"));
        }
    }
    if (!con[n].used)
        return -ENXIO;
    h->rows = ROWS; h->cols = COLS;
    h->crow = (uint16_t)con[n].crow; h->ccol = (uint16_t)con[n].ccol;
    h->changes = con[n].changes;
    h->shown = n == front;
    h->pcfont = 0;
    if (cells && cap)
        memcpy(cells, con[n].cell, cap < sizeof con[n].cell ? cap : sizeof con[n].cell);
    return 0;
}
/* a pixel's pen: the glyph's bits as a pretend font (glyph + line, shifted by the column), in the cell's colour */
static uint8_t pen(int n, int x, int line)
{
    const uint8_t *c = con[n].cell + 3 * ((line / 8) * COLS + x / 8);
    return ((c[0] + line) >> (x & 7)) & 1 ? (c[2] & 7) : 0;
}
int wsb_os_con_line(int n, int line, uint8_t *pens, uint32_t cap, struct wsb_con_disp *d)
{
    con_setup();
    if (n != front)
        return -ENXIO;
    if (line < 0 || line >= ROWS * 8)
        return -EINVAL;
    d->width = COLS * 8;
    d->height = ROWS * 8;
    d->depth = 3;
    d->generation = 7;
    for (int i = 0; i < 16; i++) {
        d->rgb[i][0] = (uint8_t)(i * 32);
        d->rgb[i][1] = (uint8_t)(255 - i * 32);
        d->rgb[i][2] = (uint8_t)(i * 85);
    }
    for (uint32_t x = 0; x < (uint32_t)COLS * 8 && x < cap; x++)
        pens[x] = pen(n, (int)x, line);
    return 0;
}
int wsb_os_con_input(int n, int kind, const uint8_t *b, int cnt)
{
    con_setup();
    if (n < 1 || n > WSB_NCONS || cnt < 0 || cnt > 256)
        return -EINVAL;
    if (kind == WSB_IN_RAW) {
        FILE *f = fopen(hp("/.keys"), "a");
        for (int i = 0; i < cnt; i++) {
            if (b[i] == 0x78 || b[i] >= 0xF8)
                return -EINVAL;
            if (f) fprintf(f, "%02x\n", b[i]);
            if (b[i] == 0x66 || b[i] == 0x67) amiga_down = 1;
            else if (b[i] == 0xE6 || b[i] == 0xE7) amiga_down = 0;
            else if (amiga_down && b[i] >= 0x50 && b[i] <= 0x59) {
                front = b[i] - 0x50 + 1;
                if (!con[front].used) { con_clear(front); con_puts(front, "login: "); }
            }
        }
        if (f) fclose(f);
        return cnt;
    }
    if (!con[n].used)
        return -ENXIO;
    for (int i = 0; i < cnt; i++)
        con_putc(n, b[i]);
    return cnt;
}

int main(int argc, char **argv)
{
    int i = 1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) {
            if (!realpath(argv[++i], root)) { perror("--root"); return 2; }
        } else if (!strcmp(argv[i], "--offset") && i + 1 < argc)
            offset = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--peer") && i + 1 < argc) {
            struct in_addr a;
            if (!inet_aton(argv[++i], &a)) return 2;
            peer_override = ntohl(a.s_addr);
        } else if (!strcmp(argv[i], "--noconops"))
            conops = 0;
        else if (!strcmp(argv[i], "--")) {
            i++;
            break;
        } else
            break;
    }
    if (!root[0]) {
        fprintf(stderr, "usage: wasabi-host --root DIR --offset N [--peer A.B.C.D] [--noconops] [--] [ARGS]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    argv[i - 1] = "wasabid";
    int st = wsb_init(argc - i + 1, argv + i - 1);
    while (st == WSB_GOON)
        st = wsb_step();
    wsb_end();
    return st;
}
