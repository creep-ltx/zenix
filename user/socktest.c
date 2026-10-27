/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/netproto.h"
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef int s32;
/* socktest CHECK...: the kernel's socket layer (DESIGN D19 step 3) against socktestd(8) on port "net". Each check prints
 * "socktest CHECK: PASS (...)" or "... FAIL (...)"; the exit status is the number of failures. tools/sock-test.sh
 * registers socktestd and runs them in the emulator (docs/man/socktest.txt lists them). The other ends are this
 * program again, started with an argument that begins with '_'. */
#define ZZ 0x7A7A
static char self[64];
static int fails;

static void result(const char *name, int ok, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("socktest %s: %s (%s)\n", name, ok ? "PASS" : "FAIL", buf);
    if (!ok)
        fails++;
}

/* ---- the raw calls (libz's BSD functions come with netd, step 4) ---- */
static struct usockreq R(void) { struct usockreq r; memset(&r, 0, sizeof r); return r; }
static int sk(int flags)
{
    struct usockreq r = R();
    r.level = AF_INET;
    r.flags = (u32)(SOCK_STREAM | flags);
    return sys_sock(SOCK_SOCKET, 0, &r);
}
static u8 *inaddr(int port)
{
    static u8 a[16];
    memset(a, 0, sizeof a);
    a[1] = AF_INET;
    a[2] = (u8)(port >> 8);
    a[3] = (u8)port;
    a[4] = 127;
    a[7] = 1;
    return a;
}
static int addr_op(u32 op, int fd, int port)
{
    struct usockreq r = R();
    u32 l = 16;
    r.addr = (u32)inaddr(port);
    r.alen = (u32)&l;
    return sys_sock(op, fd, &r);
}
static int listener(int port)
{
    int fd = sk(0);
    if (fd < 0)
        return fd;
    int e = addr_op(SOCK_BIND, fd, port);
    struct usockreq r = R();
    r.len = 4;
    if (!e)
        e = sys_sock(SOCK_LISTEN, fd, &r);
    if (e) {
        sys_close(fd);
        return e;
    }
    return fd;
}
static int acc(int lfd, int flags)
{
    struct usockreq r = R();
    r.flags = (u32)flags;
    return sys_sock(SOCK_ACCEPT, lfd, &r);
}
static int snd(int fd, const void *b, u32 n, u32 flags)
{
    struct usockreq r = R();
    r.buf = (u32)b;
    r.len = n;
    r.flags = flags;
    return sys_sock(SOCK_SENDTO, fd, &r);
}
static int rcv(int fd, void *b, u32 n, u32 flags)
{
    struct usockreq r = R();
    r.buf = (u32)b;
    r.len = n;
    r.flags = flags;
    return sys_sock(SOCK_RECVFROM, fd, &r);
}
static int zz(int fd, int cmd)
{
    struct usockreq r = R();
    s32 v = 0;
    u32 l = 4;
    r.level = ZZ;
    r.option = (u16)cmd;
    r.val = (u32)&v;
    r.vlen = (u32)&l;
    return sys_sock(SOCK_SETSOCKOPT, fd, &r);
}
static int served(int fd)               /* how many requests socktestd has answered */
{
    struct usockreq r = R();
    u32 v = 0, l = 4;
    r.level = ZZ;
    r.option = 4;
    r.val = (u32)&v;
    r.vlen = (u32)&l;
    int e = sys_sock(SOCK_GETSOCKOPT, fd, &r);
    return e < 0 ? e : (int)v;
}
static int timeo(int fd, int opt, u32 ticks)
{
    struct usockreq r = R();
    u32 tv[2] = { ticks / 100, ticks % 100 * 10000 }, l = 8;
    r.level = SOL_SOCKET;
    r.option = (u16)opt;
    r.val = (u32)tv;
    r.vlen = (u32)&l;
    return sys_sock(SOCK_SETSOCKOPT, fd, &r);
}
static int sockerr(int fd)
{
    struct usockreq r = R();
    s32 v = -1;
    u32 l = 4;
    r.level = SOL_SOCKET;
    r.option = SO_ERROR;
    r.val = (u32)&v;
    r.vlen = (u32)&l;
    int e = sys_sock(SOCK_GETSOCKOPT, fd, &r);
    return e < 0 ? e : v;
}
static int ioc(int fd, int req, s32 *v)
{
    struct usockreq r = R();
    r.option = (u16)req;
    r.val = (u32)v;
    return sys_sock(SOCK_IOCTL, fd, &r);
}
static int pollone(int fd, int ev, int ticks)
{
    struct pollfd p;
    p.fd = fd;
    p.events = (short)ev;
    p.revents = 0;
    int n = sys_evwait(&p, 1, ticks);
    return n < 0 ? n : n ? p.revents : 0;
}

/* a connected pair through socktestd: *a the connecting end, *b the accepted one */
static int pair(int port, int *a, int *b)
{
    int l = listener(port);
    if (l < 0)
        return l;
    *a = sk(0);
    int e = *a < 0 ? *a : addr_op(SOCK_CONNECT, *a, port);
    *b = e ? e : acc(l, 0);
    sys_close(l);
    return e ? e : *b < 0 ? *b : 0;
}

static int child(const char *a1, const char *a2, const int *stdio)
{
    char *argv[4] = { self, (char *)a1, (char *)a2, 0 };
    return sys_spawn(self, argv, stdio);
}

static volatile int hits;
static void count(int sig) { (void)sig; hits++; }
static void catch(int sig, int flags)
{
    struct sigaction a;
    a.sa_handler = count;
    a.sa_mask = 0;
    a.sa_flags = flags;
    sigaction(sig, &a, 0);
}
static void uncatch(int sig)
{
    struct sigaction a;
    a.sa_handler = SIG_DFL;
    a.sa_mask = 0;
    a.sa_flags = 0;
    sigaction(sig, &a, 0);
}

/* ---- the other ends ---- */
static int peer_main(const char *how, const char *arg)
{
    int port = atoi(arg);
    int fd = sk(0);
    if (fd < 0 || addr_op(SOCK_CONNECT, fd, port) < 0)
        return 90;
    if (!strcmp(how, "_late")) {        /* connect, wait a second, send "late", wait for the close */
        sys_sleep(100);
        snd(fd, "late", 4, 0);
        char c;
        rcv(fd, &c, 1, 0);
        return 0;
    }
    if (!strcmp(how, "_close"))         /* connect and close at once */
        return 0;
    if (!strcmp(how, "_reader")) {      /* read 2000 bytes after two seconds; they must be 0, 1, 2, ... in order */
        sys_sleep(200);
        u8 b[600];
        int got = 0, bad = 0;
        while (got < 2000) {
            int n = rcv(fd, b, sizeof b, 0);
            if (n <= 0)
                return 91;
            for (int k = 0; k < n; k++)
                if (b[k] != (u8)((got + k) * 7))
                    bad++;
            got += n;
        }
        return bad ? 92 : 0;
    }
    return 99;
}

/* fd 0 is a socket: block in recv on it; the exit status says what it returned (an error's number, 1 for data) */
static int blockrecv_main(void)
{
    char c;
    int r = rcv(0, &c, 1, 0);
    return r < 0 ? -r : 1;
}

static int sendpipe_main(void)          /* fd 0 is a socket whose peer closed: a send without MSG_NOSIGNAL */
{
    sys_sleep(20);
    snd(0, "x", 1, 0);
    return 3;                           /* not reached: SIGPIPE ends it (141) */
}

static int hang_main(void)              /* blocks in recv until ^C (sock-test.sh types it) */
{
    int a, b;
    if (pair(7100, &a, &b))
        return 90;
    printf("hang: waiting\n");
    oflush();
    char c;
    int r = rcv(b, &c, 1, 0);
    printf("hang: recv returned %d\n", r);
    return 0;
}

/* ---- the checks ---- */
static void c_basic(void)
{
    int a, b;
    int e = pair(7001, &a, &b);
    result("basic", !e, "a connected pair: %d", e);
    if (e)
        return;
    struct ustat st;
    e = sys_fstat(a, &st);
    result("fstat", !e && (st.mode & S_IFMT) == S_IFSOCK, "mode %o", st.mode);
    int n = snd(a, "hello", 5, 0);
    char buf[16];
    memset(buf, 0, sizeof buf);
    int m = rcv(b, buf, sizeof buf, 0);
    result("data", n == 5 && m == 5 && !memcmp(buf, "hello", 5), "sent %d, got %d '%s'", n, m, buf);
    n = sys_write(b, "back", 4);
    memset(buf, 0, sizeof buf);
    m = sys_read(a, buf, sizeof buf);
    result("readwrite", n == 4 && m == 4 && !memcmp(buf, "back", 4), "write %d, read %d", n, m);
    s32 v = -1;
    snd(a, "abc", 3, 0);
    e = ioc(b, FIONREAD, &v);
    result("fionread", !e && v == 3, "%d, %d bytes waiting", e, v);
    rcv(b, buf, 3, 0);
    /* the clamps: an address length of 1000 into a 28-byte region (checked as 28), the true length 16 comes back */
    u8 *small = sys_memalloc(28);
    u32 l = 1000;
    struct usockreq r = R();
    r.addr = (u32)small;
    r.alen = (u32)&l;
    e = sys_sock(SOCK_GETSOCKNAME, a, &r);
    result("addrclamp", !e && l == 16 && small && small[1] == AF_INET, "%d, length %u", e, l);
    /* a buffer length far over 64 KB, in a 64 KB region: checked as 64 KB */
    u8 *big = sys_memalloc(65536);
    snd(a, "xyz", 3, 0);
    r = R();
    r.buf = (u32)big;
    r.len = 0x7FFFFFF0u;
    n = sys_sock(SOCK_RECVFROM, b, &r);
    result("bufclamp", n == 3 && big && !memcmp(big, "xyz", 3), "%d", n);
    /* a server that answers a count far too big: the kernel gives at most what it checked */
    zz(b, 2);
    snd(a, "0123456789", 10, 0);
    n = rcv(b, buf, 4, 0);
    result("countclamp", n == 4, "the server said 1000000, the program got %d", n);
    rcv(b, buf, 16, 0);
    sys_close(a);
    sys_close(b);
}

static void c_ptr(void)
{
    int a, b;
    if (pair(7002, &a, &b)) {
        result("ptr", 0, "no pair");
        return;
    }
    int e1 = rcv(b, (void *)0x10, 4, 0);                /* the kernel's low memory */
    int e2 = snd(a, (void *)0x400, 4, 0);
    int e3 = sys_sock(SOCK_RECVFROM, b, (void *)0x20);  /* the request itself */
    struct usockreq r = R();
    r.addr = (u32)inaddr(1);
    r.alen = 0x30;                                      /* the length's pointer */
    int e4 = sys_sock(SOCK_GETSOCKNAME, a, &r);
    int e5 = sys_sock(SOCK_RECVFROM, 0, &r);            /* a terminal is no socket */
    int e6 = sys_sock(SOCK_RECVFROM, 13, &r);           /* no such fd */
    int e7 = sys_sock(99, a, &r);
    result("ptr", e1 == -EFAULT && e2 == -EFAULT && e3 == -EFAULT && e4 == -EFAULT,
           "recv into 0x10 %d, send from 0x400 %d, request at 0x20 %d, alen at 0x30 %d", e1, e2, e3, e4);
    result("notsock", e5 == -ENOTSOCK && e6 == -EBADF && e7 == -EINVAL, "%d %d %d", e5, e6, e7);
    sys_close(a);
    sys_close(b);
}

static void c_block(void)
{
    int l = listener(7003);
    int pid = child("_late", "7003", 0);
    if (pid < 0) {
        result("block", 0, "cannot start the other end: %d", pid);
        return;
    }
    u32 t0 = sys_ticks();
    int b = acc(l, 0);                  /* waits for the child's connect */
    int s0 = served(b);
    char buf[8];
    memset(buf, 0, sizeof buf);
    int n = rcv(b, buf, sizeof buf, 0); /* waits a second for "late" */
    u32 t = sys_ticks() - t0;
    int s1 = served(b);
    result("block", b >= 0 && n == 4 && !memcmp(buf, "late", 4) && t >= 80, "accept %d, recv %d '%s' after %u ticks", b, n,
           buf, t);
    result("nospin", s1 - s0 <= 4, "%d requests while it waited", s1 - s0 - 1);
    sys_close(b);
    sys_close(l);
    sys_wait(pid);
}

static void c_poll(void)
{
    int l = listener(7004);
    int pid = child("_late", "7004", 0);
    int b = acc(l, 0);
    int s0 = served(b);
    u32 t0 = sys_ticks();
    int rv = pollone(b, POLLIN, 300);
    u32 t = sys_ticks() - t0;
    int s1 = served(b);
    result("poll", rv == POLLIN && t >= 50, "revents %x after %u ticks", rv, t);
    result("pollnomsg", s1 - s0 == 2, "%d requests during the poll: the peer's send and this question", s1 - s0);
    rv = pollone(b, POLLOUT, 0);
    result("pollout", rv == POLLOUT, "revents %x", rv);
    sys_close(b);
    sys_close(l);
    sys_wait(pid);
}

static void c_restart(void)
{
    int l = listener(7005), b, pid;
    char buf[8];
    /* SA_RESTART: the alarm's handler runs, the recv goes on and gets the data */
    catch(SIGALRM, SA_RESTART);
    hits = 0;
    pid = child("_late", "7005", 0);
    b = acc(l, 0);
    sys_alarm(30);
    int n = rcv(b, buf, sizeof buf, 0);
    result("restart", n == 4 && hits == 1, "recv %d, handler ran %d times", n, hits);
    sys_close(b);
    sys_wait(pid);
    /* without it: -EINTR */
    catch(SIGALRM, 0);
    hits = 0;
    pid = child("_late", "7005", 0);
    b = acc(l, 0);
    sys_alarm(30);
    n = rcv(b, buf, sizeof buf, 0);
    result("eintr", n == -EINTR && hits == 1, "recv %d, handler %d", n, hits);
    sys_close(b);
    sys_wait(pid);
    /* SA_RESTART but a receive timeout: never restarted (Linux) */
    catch(SIGALRM, SA_RESTART);
    hits = 0;
    pid = child("_late", "7005", 0);
    b = acc(l, 0);
    timeo(b, SO_RCVTIMEO, 300);
    sys_alarm(30);
    n = rcv(b, buf, sizeof buf, 0);
    result("timeonorestart", n == -EINTR && hits == 1, "recv %d, handler %d", n, hits);
    sys_close(b);
    sys_wait(pid);
    /* the timeout itself: -EAGAIN at the deadline */
    pid = child("_late", "7005", 0);
    b = acc(l, 0);
    timeo(b, SO_RCVTIMEO, 40);
    u32 t0 = sys_ticks();
    n = rcv(b, buf, sizeof buf, 0);
    u32 t = sys_ticks() - t0;
    struct usockreq r = R();
    u32 tv[2] = { 9, 9 }, tl = 8;
    r.level = SOL_SOCKET;
    r.option = SO_RCVTIMEO;
    r.val = (u32)tv;
    r.vlen = (u32)&tl;
    int g = sys_sock(SOCK_GETSOCKOPT, b, &r);
    result("rcvtimeo", n == -EAGAIN && t >= 38 && t < 90 && !g && tv[0] == 0 && tv[1] == 400000,
           "recv %d after %u ticks; getsockopt %d: %u s %u us", n, t, g, tv[0], tv[1]);
    sys_close(b);
    sys_wait(pid);
    uncatch(SIGALRM);
    sys_close(l);
}

static void c_partial(void)
{
    static u8 data[2000];
    for (int k = 0; k < 2000; k++)
        data[k] = (u8)(k * 7);
    int l = listener(7006);
    int pid = child("_reader", "7006", 0);
    int b = acc(l, 0);
    catch(SIGALRM, SA_RESTART);
    hits = 0;
    sys_alarm(50);
    int n = snd(b, data, 2000, 0);      /* the reader takes nothing for two seconds: 512 fit, then the alarm */
    result("partial", n == 512 && hits == 1, "send gave %d after the alarm (handler %d)", n, hits);
    int m = n > 0 ? snd(b, data + n, (u32)(2000 - n), 0) : -1;   /* the rest, once the reader reads */
    int st = sys_wait(pid);
    result("partialorder", m == 2000 - n && st == 0, "the rest %d, the reader saw every byte once, in order: %d", m, st);
    uncatch(SIGALRM);
    sys_close(b);
    /* non-blocking: what fits at once, then -EAGAIN */
    int a2;
    if (pair(7007, &a2, &b) == 0) {
        s32 one = 1;
        ioc(a2, FIONBIO, &one);
        int n1 = snd(a2, data, 2000, 0), n2 = snd(a2, data, 10, 0);
        int n3 = rcv(b, data + 1000, 10, MSG_DONTWAIT);
        int n4 = rcv(a2, data + 1000, 10, 0);
        result("nonblock", n1 == 512 && n2 == -EAGAIN && n3 == 10 && n4 == -EAGAIN, "send %d, then %d; recv %d, %d", n1,
               n2, n3, n4);
        int nb = sk(SOCK_NONBLOCK), l2 = listener(7008);
        ioc(l2, FIONBIO, &one);
        int e = acc(l2, 0) == -EAGAIN ? 0 : 1;
        sys_close(l2);
        int e2 = addr_op(SOCK_CONNECT, nb, 8001);        /* nobody listens there */
        result("nonblockmisc", !e && e2 == -ECONNREFUSED, "accept %s, connect %d", e ? "waited" : "-EAGAIN", e2);
        sys_close(nb);
        sys_close(a2);
        sys_close(b);
    }
    sys_close(l);
}

static void c_connect(void)
{
    int l = listener(8100);             /* 8000 and up: socktestd completes the connect a second later */
    int a = sk(0);
    u32 t0 = sys_ticks();
    int e = addr_op(SOCK_CONNECT, a, 8100);
    u32 t = sys_ticks() - t0;
    result("connect", !e && t >= 80, "blocking connect %d after %u ticks", e, t);
    sys_close(a);
    a = sk(SOCK_NONBLOCK);
    e = addr_op(SOCK_CONNECT, a, 8100);
    int rv = pollone(a, POLLOUT, 300), se = sockerr(a);
    result("connectnb", e == -EINPROGRESS && rv == POLLOUT && se == 0, "%d, then poll %x, SO_ERROR %d", e, rv, se);
    sys_close(a);
    /* interrupted: -EINTR, the connection goes on; restarted (SA_RESTART): it only waits (a second CONNECT would be
     * -EALREADY) */
    catch(SIGALRM, 0);
    a = sk(0);
    sys_alarm(30);
    e = addr_op(SOCK_CONNECT, a, 8100);
    rv = pollone(a, POLLOUT, 300);
    result("connectintr", e == -EINTR && rv == POLLOUT, "%d, then poll %x", e, rv);
    sys_close(a);
    catch(SIGALRM, SA_RESTART);
    hits = 0;
    a = sk(0);
    sys_alarm(30);
    e = addr_op(SOCK_CONNECT, a, 8100);
    result("connectrestart", !e && hits == 1, "%d, handler %d", e, hits);
    uncatch(SIGALRM);
    sys_close(a);
    s32 one = 1;
    ioc(l, FIONBIO, &one);
    int nacc = 0;
    for (int k = 0; k < 6; k++) {       /* the four connections, accepted now; then -EAGAIN */
        int b = acc(l, 0);
        if (b >= 0) {
            nacc++;
            sys_close(b);
        }
    }
    result("backlog", nacc == 4, "%d connections were waiting to be accepted", nacc);
    sys_close(l);
}

static void c_sigpipe(void)
{
    int l = listener(7009);
    int pid = child("_close", "7009", 0);
    int b = acc(l, 0);
    sys_wait(pid);                      /* its end closed its socket */
    char c;
    int r = rcv(b, &c, 1, 0);
    int e = snd(b, "x", 1, MSG_NOSIGNAL);
    int fds[3] = { b, 1, 2 };
    char *argv[3] = { self, "_sendpipe", 0 };
    int p2 = sys_spawn(self, argv, fds);
    int st = p2 < 0 ? p2 : sys_wait(p2);
    result("eof", r == 0, "recv after the peer closed: %d", r);
    result("sigpipe", e == -EPIPE && st == 128 + SIGPIPE, "MSG_NOSIGNAL: %d; without it the sender ended with %d", e, st);
    sys_close(b);
    sys_close(l);
}

static void c_down(void)
{
    int a, b;
    if (pair(7010, &a, &b)) {
        result("down", 0, "no pair");
        return;
    }
    int fds[3] = { b, 1, 2 };           /* the accepted socket handed to a child, which blocks in recv on it */
    char *argv[3] = { self, "_blockrecv", 0 };
    int pid = sys_spawn(self, argv, fds);
    sys_sleep(50);
    u32 t0 = sys_ticks();
    int e = zz(a, 1);                   /* socktestd crashes in the middle of this request */
    int st = sys_wait(pid);
    u32 t = sys_ticks() - t0;
    result("down", e == -ENETDOWN && st == ENETDOWN && t < 50, "the request %d; the blocked child %d after %u ticks", e, st, t);
    char c;
    int r1 = rcv(a, &c, 1, 0), r2 = snd(a, "x", 1, MSG_NOSIGNAL), rv = pollone(a, POLLIN, 0);
    result("dead", r1 == -ENETDOWN && r2 == -ENETDOWN && rv == (POLLERR | POLLHUP), "recv %d, send %d, poll %x", r1, r2,
           rv);
    t0 = sys_ticks();
    int c1 = sys_close(a), c2 = sys_close(b);
    t = sys_ticks() - t0;
    struct uproc p;
    int alive = 0;
    for (u32 i = 0; i < 64; i++)
        if (sys_procinfo(i, &p) == 0 && p.state && p.state != 5 && !strcmp(p.name, "socktestd"))
            alive++;
    result("noclosestart", !c1 && !c2 && t < 20 && !alive, "close %d %d in %u ticks; socktestd running: %d", c1, c2, t,
           alive);
    int s = sk(0);
    result("again", s >= 0, "a new socket starts the server again: %d", s);
    if (s >= 0)
        sys_close(s);
}

int main(int argc, char **argv)
{
    if (!path_find(argv[0], self, sizeof self))
        strcpy(self, "/bin/socktest");
    if (argc > 1 && argv[1][0] == '_') {
        if (!strcmp(argv[1], "_blockrecv"))
            return blockrecv_main();
        if (!strcmp(argv[1], "_sendpipe"))
            return sendpipe_main();
        if (!strcmp(argv[1], "_hang"))
            return hang_main();
        return argc > 2 ? peer_main(argv[1], argv[2]) : 99;
    }
    if (argc < 2) {
        fprintf(2, "usage: socktest CHECK... (basic ptr block poll restart partial connect sigpipe down hang)\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        const char *c = argv[i];
        if (!strcmp(c, "basic")) c_basic();
        else if (!strcmp(c, "ptr")) c_ptr();
        else if (!strcmp(c, "block")) c_block();
        else if (!strcmp(c, "poll")) c_poll();
        else if (!strcmp(c, "restart")) c_restart();
        else if (!strcmp(c, "partial")) c_partial();
        else if (!strcmp(c, "connect")) c_connect();
        else if (!strcmp(c, "sigpipe")) c_sigpipe();
        else if (!strcmp(c, "down")) c_down();
        else if (!strcmp(c, "hang")) return hang_main();
        else result(c, 0, "no such check");
    }
    return fails;
}
