/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid's core under libFuzzer (DESIGN D24 9, 12.1): the whole daemon (net/wasabi/) over an OS layer in memory, one
 * client sending the fuzzer's bytes in pieces, plus /etc/wasabid.conf's parser on the same bytes. Besides the
 * sanitizers (and LeakSanitizer: every block given back by wsb_end) it checks what the client relies on: every frame
 * the daemon sends is whole, at most 4 KB of payload (9), a known tag; the descriptor count never passes 16; nothing
 * waits for ever (the steps are bounded, time runs fast). Built by tools/host/wasabi-test.sh fuzz.
 * The first byte chooses: bit 0 a right HELLO first, bit 1 the console operations, bit 2 --init; the second the size
 * of the pieces (1-64); the rest is what the client sends. With -DSEEDS it writes a seed corpus instead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/wasabi/wasabi_int.h"
#include "../../user/regex.h"

#define KEY "fuzz-key-0123456789"
static const uint8_t *in;
static uint32_t inn, inpos, piece;
static uint32_t now_ms, steps;
static int accepted, cl_open, conops, open_fds;
static uint8_t outq[8];                         /* the client's view of the daemon's frames: the header being read */
static uint32_t outhdr, outleft;

#define FD_LISTEN 3
#define FD_UDP0 4
#define FD_UDP1 5
#define FD_NULL 6
#define FD_CONN 7
#define FD_FILE 20                              /* files: 20-29 */
#define FD_PIPE 30                              /* pipes: 30-59 */
static int fdopen_[64];
static uint32_t fdpos[64];
static int pipe_left[64];                       /* bytes a pipe's read end still gives */

static int newfd(int lo, int hi)
{
    for (int i = lo; i < hi; i++)
        if (!fdopen_[i]) {
            fdopen_[i] = 1;
            fdpos[i] = 0;
            open_fds++;
            return i;
        }
    return -EMFILE;
}

uint32_t wsb_os_now(void) { return now_ms; }
uint32_t wsb_os_uptime(void) { return now_ms / 1000; }
void wsb_os_log(const char *line) { (void)line; }
void *wsb_os_alloc(uint32_t n) { return malloc(n ? n : 1); }
void wsb_os_free(void *p) { free(p); }
const char *wsb_os_strerror(int e) { return e == -2 ? "no such file" : 0; }

int wsb_os_poll(struct wsb_pollfd *f, int n, int ms)
{
    int r = 0;
    if (++steps > 20000 && cl_open)
        abort();                                /* a connection that never settles */
    for (int i = 0; i < n; i++) {
        short ev = 0;
        if (f[i].fd == FD_LISTEN && !accepted)
            ev = WSB_POLLIN;
        else if (f[i].fd == FD_CONN && cl_open)
            ev = (f[i].events & WSB_POLLOUT) | ((f[i].events & WSB_POLLIN) ? WSB_POLLIN : 0);
        else if (f[i].fd >= FD_PIPE && f[i].fd < 64 && fdopen_[f[i].fd])
            ev = f[i].events & WSB_POLLIN;
        f[i].revents = ev;
        if (ev)
            r++;
    }
    if (!r)
        now_ms += ms > 0 ? (uint32_t)ms : 1;    /* time runs fast while nothing happens */
    else
        now_ms += 1;
    return r;
}
int wsb_os_listen(unsigned port) { (void)port; open_fds++; return FD_LISTEN; }
int wsb_os_udp(unsigned port) { open_fds++; return port == 1234 ? FD_UDP0 : FD_UDP1; }
int wsb_os_accept(int lfd, uint32_t *peer)
{
    (void)lfd;
    if (accepted)
        return -EAGAIN;
    accepted = cl_open = 1;
    open_fds++;
    *peer = 0x7F000001;
    return FD_CONN;
}
long wsb_os_recv(int fd, void *b, long n)
{
    if (fd != FD_CONN || !cl_open)
        return -EBADF;
    if (inpos >= inn)
        return 0;
    uint32_t k = inn - inpos < piece ? inn - inpos : piece;
    if ((long)k > n)
        k = (uint32_t)n;
    memcpy(b, in + inpos, k);
    inpos += k;
    return (long)k;
}
/* the client reads the daemon's frames: each whole, a known tag, at most 4 KB */
long wsb_os_send(int fd, const void *b, long n)
{
    const uint8_t *p = b;
    if (fd != FD_CONN || !cl_open)
        return -EPIPE;
    for (long i = 0; i < n; i++) {
        if (outleft) { outleft--; continue; }
        outq[outhdr++] = p[i];
        if (outhdr < 5)
            continue;
        uint32_t len = (uint32_t)outq[1] << 24 | (uint32_t)outq[2] << 16 | (uint32_t)outq[3] << 8 | outq[4];
        uint8_t t = outq[0];
        if (len > WSB_SMALL || !(t == WT_WELCOME || t == WT_ERR || t == WT_OK || t == WT_PONG || t == WT_DATA ||
                                 t == WT_END || t == WT_STDOUT || t == WT_STDERR || t == WT_EXIT || t == WT_LOG ||
                                 t == WT_SLOT))
            abort();
        outleft = len;
        outhdr = 0;
    }
    return n;
}
long wsb_os_recvfrom(int fd, void *b, long n, uint32_t *a, unsigned *port) { (void)fd; (void)b; (void)n; (void)a; (void)port; return -EAGAIN; }
long wsb_os_sendto(int fd, const void *b, long n, uint32_t a, unsigned port) { (void)fd; (void)b; (void)a; (void)port; return n; }
int wsb_os_shutdown(int fd) { (void)fd; return 0; }
void wsb_os_close(int fd)
{
    if (fd == FD_CONN)
        cl_open = 0;
    if (fd >= 0 && fd < 64 && fdopen_[fd])
        fdopen_[fd] = 0;
    open_fds--;
}

static const char conf_text[] = "key " KEY "\nname fuzz\n";
static const char file_text[] = "ZEXE a file of the pretend root\n";
int wsb_os_open(const char *path, int flags, uint32_t mode)
{
    (void)mode;
    if (strlen(path) > WSB_PATHMAX)
        abort();                                /* the core checks first */
    if (!strcmp(path, "/dev/null") || !strcmp(path, "/etc/wasabid.conf") || !strcmp(path, "/f") ||
        flags != WSB_O_READ) {
        int fd = newfd(FD_FILE, FD_PIPE);
        if (fd >= 0)
            fdpos[fd] = !strcmp(path, "/etc/wasabid.conf") ? 0 : !strcmp(path, "/f") ? 0x10000 : 0x20000;
        return fd;
    }
    return -ENOENT;
}
long wsb_os_read(int fd, void *b, long n)
{
    if (fd >= FD_PIPE && fd < 64) {
        if (pipe_left[fd] <= 0)
            return 0;
        long k = n < pipe_left[fd] ? n : pipe_left[fd];
        memset(b, 'o', (size_t)k);
        pipe_left[fd] -= (int)k;
        return k;
    }
    if (fd < FD_FILE || fd >= FD_PIPE || !fdopen_[fd])
        return -EBADF;
    const char *t = fdpos[fd] < 0x10000 ? conf_text : fdpos[fd] < 0x20000 ? file_text : "";
    uint32_t base = fdpos[fd] & 0xFFFF, len = (uint32_t)strlen(t);
    if (base >= len)
        return 0;
    long k = (long)(len - base) < n ? (long)(len - base) : n;
    memcpy(b, t + base, (size_t)k);
    fdpos[fd] += (uint32_t)k;
    return k;
}
long wsb_os_write(int fd, const void *b, long n) { (void)fd; (void)b; return n; }
static int st_of(const char *p, struct wsb_stat *st)
{
    memset(st, 0, sizeof *st);
    if (!strcmp(p, "/") || !strcmp(p, "/d") || !strcmp(p, "/nas")) { st->mode = 040755; return 0; }
    if (!strcmp(p, "/f") || !strcmp(p, "/d/x")) { st->mode = 0100644; st->size = 32; st->mtime = 1700000000; return 0; }
    return -ENOENT;
}
int wsb_os_fstat(int fd, struct wsb_stat *st)
{
    memset(st, 0, sizeof *st);
    st->mode = fd >= FD_FILE && fd < FD_PIPE && fdpos[fd] == 0 ? 0100600 : 0100644;
    return 0;
}
int wsb_os_stat(const char *p, struct wsb_stat *st) { return st_of(p, st); }
int wsb_os_lstat(const char *p, struct wsb_stat *st) { return st_of(p, st); }
int wsb_os_rename(const char *a, const char *b) { (void)a; (void)b; return 0; }
int wsb_os_unlink(const char *p) { (void)p; return 0; }
int wsb_os_rmdir(const char *p) { (void)p; return -ENOTEMPTY; }
int wsb_os_mkdir(const char *p) { (void)p; return -EEXIST; }
int wsb_os_chmod(const char *p, uint32_t m) { (void)p; (void)m; return 0; }
int wsb_os_opendir(const char *p)
{
    struct wsb_stat st;
    if (st_of(p, &st) || st.mode != 040755)
        return -ENOTDIR;
    return newfd(FD_FILE, FD_PIPE);
}
int wsb_os_readdir(int fd, char name[64])
{
    if (fdpos[fd] >= 3)
        return 0;
    strcpy(name, fdpos[fd] == 0 ? "x" : fdpos[fd] == 1 ? "f" : "a-name-that-is-long-enough-to-pass-the-path-limit-xx");
    fdpos[fd]++;
    return 1;
}
int wsb_os_realpath(const char *p, char *out, int cap)
{
    if ((int)strlen(p) >= cap)
        abort();
    strcpy(out, p);
    return 0;
}
int wsb_os_mount(unsigned i, struct wsb_mount *m)
{
    if (i > 1)
        return -ENOENT;
    strcpy(m->path, i ? "/nas" : "/");
    strcpy(m->type, i ? "nfs" : "ext2");
    return 0;
}
int wsb_os_statfs(unsigned mount, struct wsb_statfs *f) { (void)mount; f->bsize = 1024; f->blocks = 1000; f->bfree = 500; return 0; }

static int kidpid[8], kidage[8];
int wsb_os_spawn(const char *const *argv, int i, int o, int e)
{
    (void)argv;
    (void)i;
    if (o >= FD_PIPE && o < 64) pipe_left[o - 1] = 20;  /* the pipe's read end is the one below (wsb_os_pipe) */
    if (e >= FD_PIPE && e < 64) pipe_left[e - 1] = 7;
    for (int k = 0; k < 8; k++)
        if (!kidpid[k]) {
            kidpid[k] = 100 + k;
            kidage[k] = 0;
            return kidpid[k];
        }
    return -EAGAIN;
}
int wsb_os_pipe(int fds[2])
{
    int a = newfd(FD_PIPE, 64), b;
    if (a < 0)
        return a;
    if ((b = newfd(a + 1, 64)) < 0 || b != a + 1) {
        if (b >= 0) wsb_os_close(b);
        wsb_os_close(a);
        return -EMFILE;
    }
    open_fds -= 0;
    fds[0] = a;
    fds[1] = b;
    pipe_left[a] = 0;
    return 0;
}
int wsb_os_wait(int pid)
{
    for (int k = 0; k < 8; k++)
        if (kidpid[k] == pid) {
            if (++kidage[k] < 3 || pid == 101)  /* the second child ignores every signal: never ends */
                return -EAGAIN;
            kidpid[k] = 0;
            return 3;
        }
    return -ECHILD;
}
int wsb_os_kill(int pid, int sig) { (void)pid; (void)sig; return 0; }
int wsb_os_proc(unsigned i, struct wsb_proc *p)
{
    static const char *const n[] = { "boot", "init", "netd", "wasabid", "zup", "sh" };
    if (i >= 6)
        return -EINVAL;
    memset(p, 0, sizeof *p);
    p->pid = i + 1;
    p->ppid = i ? 1 : 0;
    p->state = 1;
    strcpy(p->name, n[i]);
    return 0;
}
int wsb_os_self(void) { return 4; }
int wsb_os_meminfo(uint32_t *c, uint32_t *f) { *c = 1000000; *f = 0; return 0; }
int wsb_os_uname(char *out, int cap) { snprintf(out, (size_t)cap, "Zenix 0.3.1 (12) m68k"); return 0; }
int wsb_os_ifaces(struct wsb_iface *t, int max) { (void)t; (void)max; return 0; }
static const char klog_text[] = "cpu: 68020\npi: Emu68\n=== previous boot (2) ended here with a kernel panic; x ===\n"
                                "> a\n=== end\nline\n";
int wsb_os_klog(uint32_t off, void *buf, uint32_t n)
{
    if (off & WSB_KLOG_OLDEST)
        return (int)(now_ms / 500 % 40);        /* the ring "wraps" as time goes on */
    off &= 0x3FFFFFFFu;
    uint32_t len = sizeof klog_text - 1;
    if (off >= len)
        return 0;
    uint32_t k = len - off < n ? len - off : n;
    memcpy(buf, klog_text + off, k);
    return (int)k;
}
int wsb_os_con_ops(void) { return conops; }
static uint32_t con_changes;
int wsb_os_con_read(int con, struct wsb_con_head *h, uint8_t *cells, uint32_t cap)
{
    if (con < 1 || con > 2)
        return -ENXIO;
    h->rows = 2; h->cols = 10; h->crow = 0; h->ccol = 0;
    h->changes = ++con_changes / 3;
    h->shown = con == 1;
    h->pcfont = 0;
    for (uint32_t i = 0; i < cap && i < 60; i++)
        cells[i] = i % 3 ? 7 : (uint8_t)('a' + i % 26);
    return 0;
}
int wsb_os_con_line(int con, int line, uint8_t *pens, uint32_t cap, struct wsb_con_disp *d)
{
    if (con != 1 || line < 0 || line >= 16)
        return -ENXIO;
    memset(d, 0, sizeof *d);
    d->width = 16; d->height = 16; d->depth = 3;
    for (uint32_t i = 0; i < cap && i < 16; i++)
        pens[i] = (uint8_t)(i * 7 + line);
    return 0;
}
int wsb_os_con_input(int con, int kind, const uint8_t *b, int n) { (void)con; (void)kind; (void)b; return n > 3 ? n - 1 : n; }

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static uint8_t buf[70000];
    if (n < 2 || n > 65536)
        return 0;
    /* the configuration's parser on the bytes as they are */
    struct wsb_conf cf;
    int line;
    const char *why;
    if (!wsb_conf_parse(&cf, (const char *)d, (uint32_t)n, &line, &why) &&
        (strlen(cf.key) < WSB_KEYMIN || strlen(cf.key) > WSB_KEYMAX || !cf.port || cf.nallow > WSB_ALLOWMAX))
        abort();
    /* the daemon, fed them by a client */
    uint32_t k = 0;
    if (d[0] & 1) {
        buf[k++] = WT_HELLO;
        put32(buf + k, 2 + 2 + sizeof KEY - 1);
        k += 4;
        put16(buf + k, 1);
        put16(buf + k + 2, sizeof KEY - 1);
        memcpy(buf + k + 4, KEY, sizeof KEY - 1);
        k += 4 + sizeof KEY - 1;
    }
    memcpy(buf + k, d + 2, n - 2);
    in = buf;
    inn = k + (uint32_t)n - 2;
    inpos = 0;
    piece = (d[1] & 63) + 1;
    now_ms = 1000;
    steps = 0;
    accepted = cl_open = 0;
    conops = (d[0] >> 1) & 1;
    outhdr = outleft = 0;
    memset(fdopen_, 0, sizeof fdopen_);
    memset(kidpid, 0, sizeof kidpid);
    memset(pipe_left, 0, sizeof pipe_left);
    open_fds = 0;
    char *argv[] = { "wasabid", (d[0] & 4) ? "--init" : 0, 0 };
    int st = wsb_init((d[0] & 4) ? 2 : 1, argv), idle = 0;
    while (st == WSB_GOON) {
        if (g.nfd > WSB_NFD)
            abort();
        st = wsb_step();
        if (!cl_open && ++idle > 400)
            break;                              /* the client is gone and the timers had their turn: stop */
    }
    wsb_end();
    return 0;
}

#ifdef SEEDS
static void seed(const char *dir, int i, const uint8_t *b, size_t n)
{
    char p[512];
    snprintf(p, sizeof p, "%s/seed-%02d", dir, i);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(b, 1, n, f); fclose(f); }
}
static size_t fr(uint8_t *p, uint8_t tag, const void *pl, uint32_t n)
{
    p[0] = tag;
    put32(p + 1, n);
    memcpy(p + 5, pl, n);
    return 5 + n;
}
int main(int argc, char **argv)
{
    static uint8_t b[20000];
    if (argc < 2)
        return 2;
    const char *dir = argv[1];
    int i = 0;
    struct { uint8_t tag; const char *pl; uint32_t n; } cmds[] = {
        { WT_PING, "", 0 }, { WT_INFO, "", 0 }, { WT_HEALTH, "", 0 }, { WT_PS, "", 0 }, { WT_SLOTS, "", 0 },
        { WT_LS, "\0\2/d", 4 }, { WT_GET, "\0\2/f", 4 }, { WT_DEL, "\0\2/d", 4 }, { WT_MKDIR, "\0\2/d", 4 },
        { WT_RUN, "\0\0\0\4\0\2ls", 8 }, { WT_RUN, "\0\0\0\2\0\2ls", 8 }, { WT_RUN, "\0\0\0\1\0\2ls", 8 },
        { WT_KILL, "\0\0\0\0\0\4netd", 10 }, { WT_DEBUG, "\0\0\0\0", 4 }, { WT_REBOOT, "\0\0\0\2", 4 },
        { WT_QUIT, "\0\0\0\1", 4 }, { WT_RESTART, "", 0 }, { WT_SPEED, "\0\0\0\1\0\0\x10\0", 8 },
        { WT_TEXT, "\0\0\0\0\0\0\0\0\0\0\0\2\0\1a", 15 }, { WT_GRAB, "\0\0", 2 }, { WT_SUM, "\0\0\0\0\0\0\0\0", 8 },
        { WT_SCREEN, "\0\0\0\0\0\4tty2", 10 }, { WT_WINDOWS, "", 0 }, { WT_KEY, "\0\1ls\n", 5 },
        { WT_INPUTS, "\1\0\0\5\0\x61\0\0\0\0\0\0", 12 }, { WT_FREE, "\0\0\0\1", 4 }, { 0x31, "", 0 },
    };
    for (unsigned c = 0; c < sizeof cmds / sizeof cmds[0]; c++) {
        size_t n = 0;
        b[n++] = 3;
        b[n++] = 20;
        n += fr(b + n, cmds[c].tag, cmds[c].pl, cmds[c].n);
        if (cmds[c].tag == WT_RUN || cmds[c].tag == WT_DEBUG)
            n += fr(b + n, WT_FREE, "\0\0\0\1\0\0\0\0", 8);
        seed(dir, i++, b, n);
    }
    /* a PUT with its body, and a SPEED to the daemon */
    size_t n = 0;
    b[n++] = 1;
    b[n++] = 63;
    n += fr(b + n, WT_PUT, "\0\0\0\x08\xff\xff\xff\xff\0\3/f2", 13);
    n += fr(b + n, WT_DATA, "ZEXE1234", 8);
    n += fr(b + n, WT_END, "", 0);
    seed(dir, i++, b, n);
    n = 2;
    n += fr(b + n, WT_SPEED, "\0\0\0\0\0\0\0\x04\0\4/tmp", 14);
    n += fr(b + n, WT_DATA, "abcd", 4);
    n += fr(b + n, WT_END, "", 0);
    seed(dir, i++, b, n);
    seed(dir, i++, (const uint8_t *)"\0\0key 0123456789abcdef\nallow 10.0.0.0/8\nport 99\n", 48);
    return 0;
}
#endif
