/* SPDX-License-Identifier: GPL-2.0-only */
/* nettest CHECK...: the network over the loopback (DESIGN D19 step 4, docs/man/nettest.txt): netd and libz's BSD
 * socket calls, as a program sees them. Each check prints "nettest CHECK: PASS (...)" or "... FAIL (...)"; the exit
 * status is the number of failures. tools/netd-test.sh runs them in the emulator. The servers are this program
 * again (nettest serve ...), started and ended by the checks. */
#include "socket.h"

static char self[64];
static int fails;

static void result(const char *name, int ok, const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("nettest %s: %s (%s)\n", name, ok ? "PASS" : "FAIL", buf);
    if (!ok) fails++;
}

static struct sockaddr_in sin(u32 a, int port)
{
    struct sockaddr_in s;
    memset(&s, 0, sizeof s);
    s.sin_family = AF_INET;
    s.sin_port = htons((unsigned short)port);
    s.sin_addr.s_addr = htonl(a);
    return s;
}
#define SA(x) ((struct sockaddr *)&(x))

static int spawn(char *const *argv, int in)
{
    int io[3] = { in, 1, 2 };
    return sys_spawn(self, argv, in >= 0 ? io : 0);
}
static int start_server(const char *proto, int port, const char *mode)
{
    char p[8];
    snprintf(p, sizeof p, "%d", port);
    char *argv[] = { "nettest", "serve", (char *)proto, p, (char *)mode, 0 };
    return spawn(argv, -1);
}
static void stop(int pid)
{
    if (pid > 0) { sys_killsig(pid, SIGKILL); sys_wait(pid); }
}
/* a TCP connection to 127.0.0.1:port, waiting up to 10 s for its server to listen */
static int dial(int port)
{
    for (int t = 0; t < 100; t++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return -1;
        struct sockaddr_in a = sin(INADDR_LOOPBACK, port);
        if (!connect(s, SA(a), sizeof a)) return s;
        int e = zerrno;
        sys_close(s);
        if (e != ECONNREFUSED) { zerrno = e; return -1; }
        sys_sleep(10);
    }
    zerrno = ECONNREFUSED;
    return -1;
}
static int recv_all(int s, void *b, u32 n)
{
    u32 got = 0;
    while (got < n) {
        int r = recv(s, (char *)b + got, n - got, 0);
        if (r <= 0) return r < 0 ? -1 : (int)got;
        got += (u32)r;
    }
    return (int)got;
}
static u32 pattern(u32 i, u32 seed) { return (i * 2654435761u + seed) >> 13 & 0xFF; }

/* ---- the servers: nettest serve tcp|udp PORT echo|sink ----
 * tcp echo: every byte back, up to 16 clients at once (poll); tcp sink: at the end of a client's data, its count and
 * the sum of its bytes (8 bytes); udp echo: every datagram back to its sender */
static int serve(const char *proto, int port, const char *mode)
{
    int udp = !strcmp(proto, "udp"), sink = !strcmp(mode, "sink");
    int l = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    int one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = sin(INADDR_ANY, port);
    if (l < 0 || bind(l, SA(a), sizeof a) || (!udp && listen(l, 16))) {
        printf("nettest serve: %s port %d: %s\n", proto, port, zwhy("?"));
        return 1;
    }
    static char buf[2048];
    if (udp)
        for (;;) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(l, buf, sizeof buf, 0, SA(from), &fl);
            if (n < 0) return zerrno == ENETDOWN ? 3 : 1;
            sendto(l, buf, (u32)n, 0, SA(from), fl);
        }
    struct pollfd p[17];
    u32 cnt[17], sum[17];
    int n = 1;
    p[0].fd = l;
    p[0].events = POLLIN;
    for (;;) {
        for (int i = 0; i < n; i++) p[i].revents = 0;
        if (poll(p, (u32)n, -1) < 0) continue;
        if (p[0].revents & (POLLERR | POLLHUP)) return 3;      /* netd ended */
        if ((p[0].revents & POLLIN) && n < 17) {
            int c = accept(l, 0, 0);
            if (c >= 0) { p[n].fd = c; p[n].events = POLLIN; cnt[n] = sum[n] = 0; n++; }
        }
        for (int i = 1; i < n; i++) {
            if (!p[i].revents) continue;
            int r = recv(p[i].fd, buf, sizeof buf, 0);
            if (r > 0 && !sink) { if (send(p[i].fd, buf, (u32)r, 0) != r) r = -1; }
            else if (r > 0) { cnt[i] += (u32)r; for (int k = 0; k < r; k++) sum[i] += (unsigned char)buf[k]; }
            if (r <= 0) {
                if (sink && !r) { u32 rep[2] = { cnt[i], sum[i] }; send(p[i].fd, rep, 8, 0); }
                sys_close(p[i].fd);
                p[i] = p[n - 1]; cnt[i] = cnt[n - 1]; sum[i] = sum[n - 1];
                n--; i--;
            }
        }
    }
}

/* ---- the checks ---- */
static void c_tcp(void)
{
    int sv = start_server("tcp", 7001, "echo"), s = dial(7001), ok = 1;
    if (s < 0) { result("tcp", 0, "connect: %s", zwhy("?")); stop(sv); return; }
    struct sockaddr_in pa, la;
    socklen_t pl = sizeof pa, ll = sizeof la;
    int names = !getpeername(s, SA(pa), &pl) && !getsockname(s, SA(la), &ll) && pl == 16 && ntohs(pa.sin_port) == 7001 &&
                pa.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(la.sin_port) >= 1024;
    static char out[1000], in[1000];
    u32 total = 0, t0 = sys_ticks();
    for (u32 k = 0; k < 100 && ok; k++) {          /* 100 KB in 1000-byte pieces, each read back */
        for (u32 i = 0; i < sizeof out; i++) out[i] = (char)pattern(total + i, 7);
        if (send(s, out, sizeof out, 0) != (int)sizeof out || recv_all(s, in, sizeof in) != (int)sizeof in ||
            memcmp(in, out, sizeof in)) ok = 0;
        else total += sizeof out;
    }
    u32 t = sys_ticks() - t0;
    sys_close(s);
    /* one big blocking send (the kernel sends it in pieces: netd takes what its buffers hold), to the sink */
    int sk = start_server("tcp", 7002, "sink"), d = dial(7002), big = 0;
    if (d >= 0) {
        static char b[65000];
        u32 sum = 0;
        for (u32 i = 0; i < sizeof b; i++) { b[i] = (char)pattern(i, 3); sum += (unsigned char)b[i]; }
        u32 rep[2] = { 0, 0 };
        int sent = send(d, b, sizeof b, 0);
        shutdown(d, SHUT_WR);
        big = sent == (int)sizeof b && recv_all(d, rep, 8) == 8 && rep[0] == sizeof b && rep[1] == sum;
        char x;
        big = big && recv(d, &x, 1, 0) == 0;       /* then the end of the stream */
        sys_close(d);
    }
    stop(sv);
    stop(sk);
    result("tcp", ok && names && big, "100 KB echoed in %u ticks, %s; addresses %s; 65000 bytes in one send %s",
           t, ok ? "same bytes" : "DIFFERENT", names ? "right" : "WRONG", big ? "counted and summed right, then EOF" : "WRONG");
}

static int dgram_wait(int s, char *in, u32 n, int ms)    /* the next datagram within ms from port 7003, or -1 */
{
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, ms) != 1) return -1;
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int r = recvfrom(s, in, n, 0, SA(from), &fl);
    return r >= 0 && fl == 16 && ntohs(from.sin_port) == 7003 && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) ? r : -2;
}
static void c_udp(void)
{
    int sv = start_server("udp", 7003, "echo"), s = socket(AF_INET, SOCK_DGRAM, 0), ok = s >= 0, got = 0, up = 0;
    static char out[1400], in[1500];
    struct sockaddr_in to = sin(INADDR_LOOPBACK, 7003);
    for (int t = 0; t < 50 && !up; t++) {               /* until the server answers; then its late answers drain */
        sendto(s, "?", 1, 0, SA(to), sizeof to);
        up = dgram_wait(s, in, sizeof in, 200) == 1;
    }
    while (dgram_wait(s, in, sizeof in, 300) >= 0)
        ;
    static const u32 sizes[] = { 1, 7, 100, 512, 1000, 1400 };
    for (u32 k = 0; ok && k < sizeof sizes / sizeof sizes[0]; k++) {
        for (u32 i = 0; i < sizes[k]; i++) out[i] = (char)pattern(i, k);
        sendto(s, out, sizes[k], 0, SA(to), sizeof to);
        int r = dgram_wait(s, in, sizeof in, 3000);
        if (r == (int)sizes[k] && !memcmp(in, out, sizes[k])) got++;
        else ok = 0;
    }
    /* a datagram longer than the buffer: the rest is lost, as on Linux */
    sendto(s, out, 100, 0, SA(to), sizeof to);
    int trunc = dgram_wait(s, in, 10, 3000) == 10 && !memcmp(in, out, 10) && dgram_wait(s, in, sizeof in, 300) == -1;
    sys_close(s);
    stop(sv);
    result("udp", up && ok && trunc, "%d of 6 datagrams (1 to 1400 bytes) echoed whole; a long one cut to the buffer %s", got,
           trunc ? "and the rest dropped" : "WRONG");
}

static void c_poll(void)
{
    int sv = start_server("tcp", 7004, "echo"), s = dial(7004);
    if (s < 0) { result("poll", 0, "connect: %s", zwhy("?")); stop(sv); return; }
    struct pollfd p = { s, POLLIN, 0 };
    u32 t0 = sys_ticks();
    int quiet = poll(&p, 1, 300);
    u32 waited = sys_ticks() - t0;
    p.events = POLLOUT; p.revents = 0;
    int out = poll(&p, 1, 0) == 1 && (p.revents & POLLOUT);
    send(s, "hey", 3, 0);
    p.events = POLLIN; p.revents = 0;
    int in = poll(&p, 1, 3000) == 1 && (p.revents & POLLIN);
    int n = 0;
    ioctl(s, FIONREAD, &n);
    char b[4];
    int r = recv_all(s, b, 3);
    /* select too (libz's, over poll) */
    send(s, "x", 1, 0);
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    struct timeval tv = { 3, 0 };
    int sel = select(s + 1, &rd, 0, 0, &tv) == 1 && FD_ISSET(s, &rd) && recv(s, b, 1, 0) == 1;
    sys_close(s);
    stop(sv);
    result("poll", !quiet && waited >= 25 && out && in && n == 3 && r == 3 && sel,
           "nothing to read: %d after %u ticks; writable %s; the echo wakes it %s (FIONREAD %d); select %s", quiet, waited,
           out ? "yes" : "NO", in ? "yes" : "NO", n, sel ? "wakes" : "WRONG");
}

static void c_nonblock(void)
{
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sin(INADDR_LOOPBACK, 7005);
    int ok = l >= 0 && !bind(l, SA(a), sizeof a) && !listen(l, 2);
    int f0 = fcntl(l, F_GETFL), set = fcntl(l, F_SETFL, f0 | O_NONBLOCK), f1 = fcntl(l, F_GETFL);
    int acc = accept(l, 0, 0), e1 = zerrno;
    int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int cr = connect(c, SA(a), sizeof a), e2 = zerrno;           /* the loopback answers in netd's next turn */
    struct pollfd p = { c, POLLOUT, 0 };
    int up = poll(&p, 1, 3000) == 1;
    int err = -1;
    socklen_t el = sizeof err;
    getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el);
    struct pollfd q = { l, POLLIN, 0 };
    int pending = poll(&q, 1, 3000) == 1;
    int s = accept4(l, 0, 0, SOCK_NONBLOCK), f2 = fcntl(s, F_GETFL);
    char b;
    int rr = recv(s, &b, 1, 0), e3 = zerrno;
    int ff = fcntl(1, F_GETFL), fs = fcntl(1, F_SETFL, O_NONBLOCK), e4 = zerrno;   /* not a socket: not yet */
    sys_close(s); sys_close(c); sys_close(l);
    ok = ok && f0 == O_RDWR && !set && f1 == (O_RDWR | O_NONBLOCK) && acc < 0 && e1 == EAGAIN && cr < 0 &&
         (e2 == EINPROGRESS) && up && !err && pending && s >= 0 && f2 == (O_RDWR | O_NONBLOCK) && rr < 0 && e3 == EAGAIN &&
         ff == O_RDWR && fs < 0 && e4 == EINVAL;
    result("nonblock", ok, "F_GETFL %x then %x; accept %s; connect %s, then writable %s, SO_ERROR %d; accept4's socket %x, "
           "recv %s; a terminal: F_GETFL %x, F_SETFL O_NONBLOCK %s", f0, f1, zstrerror(e1), zstrerror(e2), up ? "yes" : "NO", err,
           f2, zstrerror(e3), ff, zstrerror(e4));
}

static void c_names(void)
{
    struct in_addr a;
    char b[INET_ADDRSTRLEN];
    unsigned char raw[4];
    int ok = inet_pton(AF_INET, "10.0.2.15", raw) == 1 && raw[0] == 10 && raw[3] == 15 && inet_pton(AF_INET, "1.2.3", raw) == 0 &&
             inet_pton(AF_INET, "1.2.3.256", raw) == 0 && inet_pton(AF_INET, "01.2.3.4", raw) == 0 &&
             !strcmp(inet_ntop(AF_INET, raw, b, sizeof b), "10.0.2.15") && !inet_ntop(AF_INET, raw, b, 5) &&
             inet_aton("127.1", &a) && a.s_addr == htonl(0x7f000001) && inet_aton("0x0a000001", &a) && a.s_addr == htonl(0x0a000001) &&
             inet_addr("x") == INADDR_NONE && !strcmp(inet_ntoa(a), "10.0.0.1");
    struct addrinfo h, *r = 0, *r2 = 0, *r3 = 0;
    memset(&h, 0, sizeof h);
    h.ai_socktype = SOCK_STREAM;
    int g1 = getaddrinfo("127.0.0.1", "http", &h, &r);
    int ok1 = !g1 && r && !r->ai_next && r->ai_family == AF_INET && r->ai_protocol == IPPROTO_TCP &&
              ((struct sockaddr_in *)r->ai_addr)->sin_port == htons(80);
    int g2 = getaddrinfo("localhost", "7", 0, &r2);         /* /etc/hosts, through netd: a stream and a datagram entry */
    int ok2 = !g2 && r2 && r2->ai_next && !r2->ai_next->ai_next &&
              ((struct sockaddr_in *)r2->ai_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    int g3 = getaddrinfo("no-such-host.invalid", 0, 0, &r3);  /* no name server here: no answer */
    h.ai_flags = AI_NUMERICHOST;
    struct addrinfo *r4 = 0;
    int g4 = getaddrinfo("localhost", 0, &h, &r4);
    freeaddrinfo(r);
    freeaddrinfo(r2);
    result("names", ok && ok1 && ok2 && g3 != 0 && g4 == EAI_NONAME,
           "inet_* %s; 127.0.0.1:http %s; localhost %s; an unknown name: %s; numeric only: %s", ok ? "right" : "WRONG",
           ok1 ? "right" : gai_strerror(g1), ok2 ? "from /etc/hosts" : gai_strerror(g2), gai_strerror(g3), gai_strerror(g4));
}

/* several connections at once, until netd's limit (the small profile: 8 TCP ends, so 4 loopback connections): the
 * one over the limit is refused at once with ENOBUFS, and every connection made still works */
static void c_many(void)
{
    enum { MAX = 12 };
    int sv = start_server("tcp", 7006, "echo"), s[MAX], n = 0, ok = 1, e = 0;
    while (n < MAX) {
        if ((s[n] = dial(7006)) < 0) { e = zerrno; break; }
        n++;
    }
    for (int i = 0; ok && i < n; i++) { char m = (char)('a' + i); ok = send(s[i], &m, 1, 0) == 1; }
    for (int i = n - 1; ok && i >= 0; i--) { char m; ok = recv(s[i], &m, 1, 0) == 1 && m == 'a' + i; }
    for (int i = 0; i < n; i++) sys_close(s[i]);
    stop(sv);
    result("many", ok && n >= 3 && (n == MAX || e == ENOBUFS), "%d connections at once%s%s, each its own bytes back%s",
           n, n < MAX ? ", the next refused: " : "", n < MAX ? zstrerror(e) : "", ok ? "" : ": WRONG");
}

/* bind below 1024: root only; ICMP echo sockets for everyone */
static void c_perm(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sin(INADDR_ANY, 80);
    int root = bind(s, SA(a), sizeof a);
    sys_close(s);
    char *argv[] = { "nettest", "_bind80", 0 };
    int pid = sys_spawn(self, argv, 0);
    int st = pid > 0 ? sys_wait(pid) : -1;
    int raw = socket(AF_INET, SOCK_RAW, 1), e = zerrno;
    result("perm", !root && st == EACCES && raw < 0 && e == EPROTONOSUPPORT,
           "root binds port 80 %s; a user: %s, and may not change interfaces or routes%s; a raw socket: %s",
           root ? "NOT" : "yes", zstrerror(st), st == 99 ? " (WRONG: allowed)" : "", zstrerror(e));
}
static int bind80(void)
{
    sys_setuid(1000);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sin(INADDR_ANY, 80);
    int e = bind(s, SA(a), sizeof a) ? zerrno : 0;
    /* the interfaces and the routes are root's too (D19 9.1) */
    struct unetifreq q;
    struct uroute rt;
    struct usockreq r;
    memset(&q, 0, sizeof q);
    q.op = NETIFREQ_DOWN;
    memset(&r, 0, sizeof r);
    r.buf = (u32)&q;
    r.len = sizeof q;
    int e2 = sys_sock(SOCK_IFSET | (u32)sizeof r << 16, s, &r);
    memset(&rt, 0, sizeof rt);
    rt.op = ROUTE_DEL;
    r.buf = (u32)&rt;
    r.len = sizeof rt;
    int e3 = sys_sock(SOCK_ROUTE | (u32)sizeof r << 16, s, &r);
    return e2 != -EPERM || e3 != -EPERM ? 99 : e;
}

/* netd killed: a blocked recv returns ENETDOWN at once; the next socket starts a new netd (D19 4.3 rules 9, 10) */
static int netd_pid(void)
{
    struct uproc p;
    for (u32 i = 1; sys_procinfo(i, &p) >= 0; i++)
        if (p.state && p.state != 5 && !strcmp(p.name, "netd")) return (int)p.pid;
    return -1;
}
static void c_down(void)
{
    int sv = start_server("tcp", 7007, "echo"), s = dial(7007);
    char *argv[] = { "nettest", "_recv", 0 };
    int ch = s >= 0 ? spawn(argv, s) : -1;           /* the connection as its standard input: it waits in recv */
    if (s >= 0) sys_close(s);
    sys_sleep(100);
    int np = netd_pid();
    u32 t0 = sys_ticks();
    if (np > 0) sys_killsig(np, SIGKILL);
    int st = ch > 0 ? sys_wait(ch) : -1;
    u32 t = sys_ticks() - t0;
    stop(sv);                                         /* its sockets died too */
    int s2 = socket(AF_INET, SOCK_STREAM, 0), np2 = netd_pid();
    int sv2 = start_server("tcp", 7008, "echo"), d = dial(7008);
    char b = 0;
    int works = d >= 0 && send(d, "z", 1, 0) == 1 && recv(d, &b, 1, 0) == 1 && b == 'z';
    sys_close(d); sys_close(s2);
    stop(sv2);
    result("down", np > 0 && st == ENETDOWN && t < 100 && s2 >= 0 && np2 > 0 && np2 != np && works,
           "netd %d killed: the waiting recv got %s after %u ticks; the next socket started netd %d, which serves %s", np,
           zstrerror(st), t, np2, works ? "a new connection" : "NOTHING");
}

/* waits for ^C (the script sends it) */
static void c_hang(void)
{
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sin(INADDR_ANY, 7009);
    bind(l, SA(a), sizeof a);
    listen(l, 1);
    printf("hang: waiting\n");
    oflush();
    accept(l, 0, 0);
    printf("hang: accept returned: %s\n", zwhy("?"));
}

/* a listener for netstat to show (the script runs `netstat -l` meanwhile) */
static void c_listen(void)
{
    int l = socket(AF_INET, SOCK_STREAM, 0), u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = sin(INADDR_ANY, 7010), b = sin(INADDR_LOOPBACK, 7011);
    bind(l, SA(a), sizeof a);
    listen(l, 1);
    bind(u, SA(b), sizeof b);
    int c = dial(7010), s = accept(l, 0, 0);
    printf("listen: ready\n");
    oflush();
    sys_sleep(3000);
    sys_close(c); sys_close(s);
}

/* the checksum and the copy in assembler (net/port/m68k.S) against C, and their speed (an emulator's ticks are no
 * hardware speed: docs/d19-network.md labels them) */
unsigned short netd_chksum(const void *data, int len);
void *netd_copy(void *dst, const void *src, unsigned long n);
static __attribute__((optimize("Os"))) unsigned short c_sum(const unsigned char *p, int len)
{
    u32 acc = 0;
    for (; len > 1; len -= 2, p += 2) acc += (u32)p[0] << 8 | p[1];
    if (len) acc += (u32)p[0] << 8;
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (unsigned short)acc;
}
static void c_asm(void)
{
    enum { N = 1600 };
    static unsigned char a[N + 64], b[N + 64], c[N + 64];
    u32 x = 12345;
    for (int i = 0; i < N + 64; i++) { x = x * 1103515245u + 12345u; a[i] = (unsigned char)(x >> 16); }
    int bad = 0, n = 0;
    for (int al = 0; al < 4; al++)
        for (int len = 0; len < 300; len++, n++)
            if (netd_chksum(a + al, len) != c_sum(a + al, len)) bad++;
    for (int len = 1400; len < 1600; len += 37, n++) if (netd_chksum(a + 1, len) != c_sum(a + 1, len)) bad++;
    memset(a, 0xFF, 64);                                /* the carries: all ones */
    if (netd_chksum(a, 64) != c_sum(a, 64)) bad++;
    int cbad = 0, cn = 0;
    for (int sa = 0; sa < 4; sa++)
        for (int da = 0; da < 4; da++)
            for (int len = 0; len < 200; len++, cn++) {
                memset(b, 0xAA, sizeof b);
                memset(c, 0xAA, sizeof c);
                for (int i = 0; i < len; i++) c[8 + da + i] = a[sa + i];
                if (netd_copy(b + 8 + da, a + sa, (unsigned long)len) != b + 8 + da || memcmp(b, c, sizeof b)) cbad++;
            }
    /* speed: 64 KB a round, 1460-byte pieces as TCP segments are */
    static unsigned char big[1460 * 8 + 8];
    u32 t0 = sys_hrtime(), s1 = 0, s2 = 0;
    for (int r = 0; r < 40; r++) for (int k = 0; k < 8; k++) s1 += c_sum(big + k * 1460, 1460);
    u32 tc = sys_hrtime() - t0;
    t0 = sys_hrtime();
    for (int r = 0; r < 40; r++) for (int k = 0; k < 8; k++) s2 += netd_chksum(big + k * 1460, 1460);
    u32 ta = sys_hrtime() - t0;
    t0 = sys_hrtime();
    for (int r = 0; r < 40; r++) memcpy(b, big + 1, 1460);      /* libz's memcpy: misaligned source, byte by byte */
    u32 tm = sys_hrtime() - t0;
    t0 = sys_hrtime();
    for (int r = 0; r < 40; r++) memcpy(b, big, 1460);          /* aligned: longwords in C */
    u32 tma = sys_hrtime() - t0;
    t0 = sys_hrtime();
    for (int r = 0; r < 40; r++) netd_copy(b, big + 1, 1460);
    u32 tn = sys_hrtime() - t0;
    t0 = sys_hrtime();
    for (int r = 0; r < 40; r++) netd_copy(b, big, 1460);
    u32 tna = sys_hrtime() - t0;
    result("asm", !bad && !cbad && s1 == s2, "checksum: %d cases, %d wrong; copy: %d cases, %d wrong; E-clock counts in "
           "this machine: checksum C %u, asm %u; copy (1460 bytes x 40) libz %u misaligned %u aligned, asm %u %u",
           n, bad, cn, cbad, tc, ta, tm, tma, tn, tna);
}

/* ---- signals and the real netd (D19 4.3 rules 4, 5): SA_RESTART, partial sends, SIGPIPE ---- */
static volatile int hits;
static void on_alarm(int sig) { (void)sig; hits++; }
static void catch_alarm(int flags)
{
    struct sigaction a;
    memset(&a, 0, sizeof a);
    a.sa_handler = on_alarm;
    a.sa_flags = flags;
    sigaction(SIGALRM, &a, 0);
    hits = 0;
}
static void uncatch_alarm(void) { sys_alarm(0); signal(SIGALRM, SIG_DFL); }
static int listen_on(int port, int backlog)
{
    int l = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = sin(INADDR_ANY, port);
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (l < 0 || bind(l, SA(a), sizeof a) || listen(l, backlog)) { if (l >= 0) sys_close(l); return -1; }
    return l;
}
/* a child: nettest _late PORT D1 D2: after D1 ticks connect, after D2 more send "late", then wait for the close */
static int start_late(int port, int d1, int d2)
{
    char p[8], a[8], b[8];
    snprintf(p, sizeof p, "%d", port); snprintf(a, sizeof a, "%d", d1); snprintf(b, sizeof b, "%d", d2);
    char *argv[] = { "nettest", "_late", p, a, b, 0 };
    return spawn(argv, -1);
}
static int late_main(int port, int d1, int d2)
{
    sys_sleep((u32)d1);
    int s = dial(port);
    if (s < 0) return 90;
    sys_sleep((u32)d2);
    send(s, "late", 4, 0);
    char c;
    recv(s, &c, 1, 0);
    return 0;
}
static void c_restart(void)
{
    int l = listen_on(7012, 4), pid, b, n;
    char buf[8];
    if (l < 0) { result("restart", 0, "listen: %s", zwhy("?")); return; }
    /* recv: SA_RESTART lets it go on to the data; without it EINTR; with a receive timeout never restarted */
    catch_alarm(SA_RESTART);
    pid = start_late(7012, 0, 100);
    b = accept(l, 0, 0);
    sys_alarm(30);
    n = recv(b, buf, sizeof buf, 0);
    int r1 = n == 4 && hits == 1;
    sys_close(b); sys_wait(pid);
    catch_alarm(0);
    pid = start_late(7012, 0, 100);
    b = accept(l, 0, 0);
    sys_alarm(30);
    n = recv(b, buf, sizeof buf, 0);
    int e2 = n < 0 ? zerrno : 0, r2 = n < 0 && e2 == EINTR && hits == 1;
    sys_close(b); sys_wait(pid);
    catch_alarm(SA_RESTART);
    pid = start_late(7012, 0, 100);
    b = accept(l, 0, 0);
    struct timeval tv = { 3, 0 };
    setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sys_alarm(30);
    n = recv(b, buf, sizeof buf, 0);
    int e3 = n < 0 ? zerrno : 0, r3 = n < 0 && e3 == EINTR && hits == 1;
    sys_close(b); sys_wait(pid);
    /* accept: the alarm comes while it waits for a connection; restarted, it gets the one that comes later */
    catch_alarm(SA_RESTART);
    pid = start_late(7012, 100, 0);
    sys_alarm(30);
    b = accept(l, 0, 0);
    int r4 = b >= 0 && hits == 1;
    if (b >= 0) sys_close(b);
    sys_wait(pid);
    /* send: the send buffer full before the call, so the alarm comes before any progress: restarted, it sends all
     * once the reader reads (never the start twice: the reader checks the order) */
    int r5 = 0, m = -1, st = -1;
    char *argv[] = { "nettest", "_reader", "7012", "200", 0 };
    pid = spawn(argv, -1);
    b = accept(l, 0, 0);
    if (b >= 0) {
        static char d[256];
        u32 sent = 0;
        int k;
        fcntl(b, F_SETFL, O_NONBLOCK);
        for (;;) {
            for (k = 0; k < (int)sizeof d; k++) d[k] = (char)pattern(sent + (u32)k, 9);
            k = send(b, d, sizeof d, 0);
            if (k <= 0) break;
            sent += (u32)k;
        }
        fcntl(b, F_SETFL, 0);
        catch_alarm(SA_RESTART);
        sys_alarm(30);
        for (k = 0; k < (int)sizeof d; k++) d[k] = (char)pattern(sent + (u32)k, 9);
        m = send(b, d, sizeof d, 0);
        shutdown(b, SHUT_WR);
        st = sys_wait(pid);
        r5 = m == (int)sizeof d && hits == 1 && st == 0;
        sys_close(b);
    }
    uncatch_alarm();
    sys_close(l);
    result("restart", r1 && r2 && r3 && r4 && r5, "recv with SA_RESTART %s; without: %s; with SO_RCVTIMEO: %s; accept %s; "
           "a send that waited from its start %s (%d, reader %d)", r1 ? "went on to the data" : "WRONG", zstrerror(e2),
           zstrerror(e3), r4 ? "went on to the connection" : "WRONG", r5 ? "went on and sent all, in order" : "WRONG", m, st);
}
/* a child: nettest _reader PORT DELAY: connect, after DELAY ticks read to the end; exit 0 when every byte is the
 * pattern (seed 9) in order, else 91; prints nothing */
static int reader_main(int port, int delay)
{
    int s = dial(port);
    if (s < 0) return 90;
    sys_sleep((u32)delay);
    static char b[1024];
    u32 got = 0;
    int r, bad = 0;
    while ((r = recv(s, b, sizeof b, 0)) > 0) {
        for (int i = 0; i < r; i++) if ((unsigned char)b[i] != pattern(got + (u32)i, 9)) bad++;
        got += (u32)r;
    }
    char c[12];
    snprintf(c, sizeof c, "%u", got);
    send(s, c, strlen(c), 0);                       /* the count back to the sender (it shut down only its writing) */
    return bad ? 91 : 0;
}
static void c_partial(void)
{
    enum { N = 30000 };
    static char d[N];
    for (u32 i = 0; i < N; i++) d[i] = (char)pattern(i, 9);
    int l = listen_on(7013, 2);
    char *argv[] = { "nettest", "_reader", "7013", "200", 0 };
    int pid = spawn(argv, -1), b = l >= 0 ? accept(l, 0, 0) : -1, n = -1, m = -1, st = -1, h = 0;
    char cnt[12];
    memset(cnt, 0, sizeof cnt);
    if (b >= 0) {
        catch_alarm(SA_RESTART);
        sys_alarm(50);                              /* the reader reads nothing for 2 s: the send stops part way */
        n = send(b, d, N, 0);
        h = hits;
        if (n > 0) m = send(b, d + n, (u32)(N - n), 0);    /* the rest, once the reader reads */
        shutdown(b, SHUT_WR);
        recv_all(b, cnt, sizeof cnt - 1);
        st = sys_wait(pid);
        uncatch_alarm();
        sys_close(b);
    }
    if (l >= 0) sys_close(l);
    result("partial", n > 0 && n < N && h == 1 && m == N - n && st == 0 && atoi(cnt) == N,
           "a %d-byte send gave %d after the alarm (handler %d), the rest %d; the reader got %s bytes, %s", N, n, h, m,
           cnt, st == 0 ? "each once, in order" : "WRONG");
}
static int close_main(int port) { int s = dial(port); return s < 0 ? 90 : 0; }   /* connect and end: its socket closes */
static int sendpipe_main(int ign)       /* fd 0: a socket whose peer is gone; send without MSG_NOSIGNAL */
{
    if (ign) signal(SIGPIPE, SIG_IGN);
    for (int k = 0; k < 50; k++) {
        if (send(0, "x", 1, 0) < 0) return zerrno;
        sys_sleep(2);
    }
    return 0;
}
static void c_sigpipe(void)
{
    int l = listen_on(7014, 2);
    char *argv[] = { "nettest", "_close", "7014", 0 };
    int pid = spawn(argv, -1), b = l >= 0 ? accept(l, 0, 0) : -1;
    sys_wait(pid);
    char c;
    int eof = b >= 0 ? recv(b, &c, 1, 0) : -1, e = 0, k, first = 0;
    /* the peer's socket is closed: the first send is taken, the peer answers with a reset, then EPIPE (as Linux) */
    for (k = 0; k < 50 && b >= 0; k++) {
        if (send(b, "x", 1, MSG_NOSIGNAL) < 0) { e = zerrno; if (!first) first = e; if (e == EPIPE) break; }
        sys_sleep(2);
    }
    int io[3] = { b, 1, 2 };
    char *a1[] = { "nettest", "_sendpipe", "0", 0 }, *a2[] = { "nettest", "_sendpipe", "1", 0 };
    int p1 = sys_spawn(self, a1, io), s1 = p1 > 0 ? sys_wait(p1) : -1;
    int p2 = sys_spawn(self, a2, io), s2 = p2 > 0 ? sys_wait(p2) : -1;
    if (b >= 0) sys_close(b);
    if (l >= 0) sys_close(l);
    result("sigpipe", eof == 0 && e == EPIPE && s1 == 128 + SIGPIPE && s2 == EPIPE,
           "recv after the peer closed: %d; sends with MSG_NOSIGNAL: %s first, %s after %d tries; without it the sender "
           "ended with %d; with SIGPIPE ignored it got %s", eof, zstrerror(first), zstrerror(e), k, s1, zstrerror(s2));
}

/* five ends of netd in a minute (D19 4.3 rule 12): netd killed until the kernel gives the service up; then a socket
 * gets ENETDOWN at once (root's ifup brings it back: tools/netd-test.sh runs that next) */
static void c_giveup(void)
{
    int kills = 0, s = -1, e = 0;
    for (int k = 0; k < 7; k++) {
        s = socket(AF_INET, SOCK_STREAM, 0);       /* starts netd (after D11's back-off) */
        if (s < 0) { e = zerrno; break; }
        int np = netd_pid();
        sys_close(s);
        s = -1;
        if (np <= 0) break;
        sys_killsig(np, SIGKILL);
        kills++;
        for (int t = 0; t < 200 && netd_pid() == np; t++) sys_sleep(1);
    }
    u32 t0 = sys_ticks();
    int s2 = socket(AF_INET, SOCK_DGRAM, 0), e2 = s2 < 0 ? zerrno : 0;
    u32 t = sys_ticks() - t0;
    if (s2 >= 0) sys_close(s2);
    result("giveup", e == ENETDOWN && kills >= 1 && kills <= 5 && e2 == ENETDOWN && t < 20,
           "netd killed %d times, then a socket: %s; the next one too, after %u ticks: %s", kills, zstrerror(e), t,
           zstrerror(e2));
}

/* ---- through SLIP to the PC (tools/slip-test.sh; the PC's servers: tools/slip-host.py on 10.0.2.2) ---- */
static int dial_to(u32 addr, int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sin(addr, port);
    if (s >= 0 && connect(s, SA(a), sizeof a)) { int e = zerrno; sys_close(s); zerrno = e; return -1; }
    return s;
}
#define PC 0x0a000202u                          /* 10.0.2.2: zenix-slirp's gateway, the PC */
static void c_sliptcp(void)                     /* TCP echo: 10 KB in 1,000-byte pieces */
{
    int s = dial_to(PC, 7100), ok = s >= 0;
    static char out[1000], in[1000];
    u32 t0 = sys_ticks(), total = 0;
    for (u32 k = 0; ok && k < 10; k++) {
        for (u32 i = 0; i < sizeof out; i++) out[i] = (char)pattern(total + i, 5);
        ok = send(s, out, sizeof out, 0) == (int)sizeof out && recv_all(s, in, sizeof in) == (int)sizeof in && !memcmp(in, out, sizeof in);
        total += sizeof out;
    }
    if (s >= 0) sys_close(s);
    result("sliptcp", ok, "10 KB echoed by 10.0.2.2:7100 in %u ticks%s", sys_ticks() - t0, ok ? "" : ": WRONG");
}
static void c_slipget(void)                     /* 100 KB from the PC, checked byte by byte (its pattern) */
{
    int s = dial_to(PC, 7102);
    u32 want = 102400, got = 0, bad = 0, t0 = sys_ticks();
    if (s >= 0 && send(s, "102400\n", 7, 0) == 7) {
        static char b[2048];
        int r;
        while ((r = recv(s, b, sizeof b, 0)) > 0) {
            for (int i = 0; i < r; i++) if ((unsigned char)b[i] != pattern(got + (u32)i, 11)) bad++;
            got += (u32)r;
        }
    }
    u32 t = sys_ticks() - t0;
    if (s >= 0) sys_close(s);
    result("slipget", got == want && !bad, "%u of %u bytes from 10.0.2.2:7102, %u wrong, in %u ticks (%u bytes/s in this "
           "emulator)", got, want, bad, t, t ? got * 100 / t : 0);
}
static void c_slipudp(void)                     /* UDP echo */
{
    static char out[1000], in[1000];
    int u = socket(AF_INET, SOCK_DGRAM, 0), n = 0;
    struct sockaddr_in to = sin(PC, 7101);
    static const u32 sizes[] = { 1, 100, 500, 900 };
    for (u32 k = 0; u >= 0 && k < 4; k++) {
        for (u32 i = 0; i < sizes[k]; i++) out[i] = (char)pattern(i, k + 20);
        int r = -1;
        for (int tries = 0; tries < 3 && r != (int)sizes[k]; tries++) {
            sendto(u, out, sizes[k], 0, SA(to), sizeof to);
            struct pollfd p = { u, POLLIN, 0 };
            r = poll(&p, 1, 3000) == 1 ? recv(u, in, sizeof in, 0) : -1;
        }
        if (r == (int)sizes[k] && !memcmp(in, out, sizes[k])) n++;
    }
    if (u >= 0) sys_close(u);
    result("slipudp", n == 4, "%d of 4 datagrams (1 to 900 bytes) echoed by 10.0.2.2:7101", n);
}
static void c_slipdns(void)                     /* a name through 10.0.2.3 (slirp's DNS, which asks the PC's test server) */
{
    struct addrinfo h, *r = 0, *r2 = 0;
    memset(&h, 0, sizeof h);
    h.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo("amiga.zenix.test", 0, &h, &r), g2 = getaddrinfo("nothing.zenix.test", 0, &h, &r2);
    int dns = !g && r && ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr == htonl(0x0a010203);
    freeaddrinfo(r);
    freeaddrinfo(r2);
    result("slipdns", dns && g2 == EAI_NONAME, "amiga.zenix.test: %s; nothing.zenix.test: %s",
           dns ? "10.1.2.3" : gai_strerror(g), gai_strerror(g2));
}

/* a SYN flood from the PC (D19 8.2, 9.4): four listeners that never accept, the PC opens hundreds of connections to
 * them through zenix-slirp's forwards (ports 7200-7203), so SYNs come as fast as the line carries them. The pool limits
 * must hold (netd's memory stays in its profile, netd never ends) and the machine must stay responsive: sleeps of 20
 * ms measured on the E-clock meanwhile, and a program started and waited for. After it, a connection to the PC works */
static u32 netd_mem(void)
{
    struct uproc p;
    for (u32 i = 1; sys_procinfo(i, &p) >= 0; i++)
        if (p.state && p.state != 5 && !strcmp(p.name, "netd")) return p.mem;
    return 0;
}
static void c_slipflood(void)
{
    int l[4], np = netd_pid(), nl = 0;
    u32 m0 = netd_mem();
    for (int k = 0; k < 4; k++) if ((l[k] = listen_on(7200 + k, 255)) >= 0) nl++;
    int ctl = dial_to(PC, 7103);
    char rep[64];
    memset(rep, 0, sizeof rep);
    if (ctl < 0 || send(ctl, "flood 7200 4 400\n", 17, 0) != 17) {
        result("slipflood", 0, "the PC's flood control: %s", zwhy("?"));
        return;
    }
    sys_sleep(200);                             /* the flood under way */
    u32 worst = 0, sum = 0, t0;
    for (int k = 0; k < 100; k++) {
        t0 = sys_hrtime();
        sys_sleep(2);
        u32 d = (sys_hrtime() - t0) / 709;      /* milliseconds (PAL's E-clock; NTSC reads 1% long) */
        sum += d;
        if (d > worst) worst = d;
    }
    char *tv[] = { "true", 0 };
    t0 = sys_hrtime();
    int tp = sys_spawn("/bin/true", tv, 0), ts = tp > 0 ? sys_wait(tp) : -1;
    u32 tt = (sys_hrtime() - t0) / 709, mf = netd_mem();
    struct pollfd p = { ctl, POLLIN, 0 };
    if (poll(&p, 1, 90000) == 1) recv(ctl, rep, sizeof rep - 1, 0);
    sys_close(ctl);
    for (char *q = rep; *q; q++) if (*q == '\n') *q = 0;
    int acc = 0;
    for (int k = 0; k < 4; k++) {               /* the connections that got through the backlogs: taken and closed */
        if (l[k] < 0) continue;
        fcntl(l[k], F_SETFL, O_NONBLOCK);
        for (int c; (c = accept(l[k], 0, 0)) >= 0; acc++) sys_close(c);
        sys_close(l[k]);
    }
    int s = dial_to(PC, 7100), ok = s >= 0;
    char e[4];
    ok = ok && send(s, "ping", 4, 0) == 4 && recv_all(s, e, 4) == 4 && !memcmp(e, "ping", 4);
    if (s >= 0) sys_close(s);
    int np2 = netd_pid();
    result("slipflood", nl == 4 && !strncmp(rep, "done", 4) && np2 == np && worst < 500 && ts == 0 && tt < 3000 && ok,
           "4 listeners, the PC: %s; meanwhile 100 sleeps of 20 ms took %u ms on average, %u at most; /bin/true ran in %u ms; "
           "netd %d -> %d, %u KB -> %u KB; %d connections came through the backlogs; afterwards a connection to the PC %s",
           rep[0] ? rep : "no answer", sum / 100, worst, tt, np, np2, m0 / 1024, mf / 1024, acc, ok ? "works" : "FAILED");
}

int main(int argc, char **argv)
{
    strcpy(self, "/bin/nettest");
    if (argc >= 5 && !strcmp(argv[1], "serve")) return serve(argv[2], atoi(argv[3]), argv[4]);
    if (argc == 2 && !strcmp(argv[1], "_recv")) { char b; return recv(0, &b, 1, 0) < 0 ? zerrno : 0; }
    if (argc == 2 && !strcmp(argv[1], "_bind80")) return bind80();
    if (argc == 5 && !strcmp(argv[1], "_late")) return late_main(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (argc == 4 && !strcmp(argv[1], "_reader")) return reader_main(atoi(argv[2]), atoi(argv[3]));
    if (argc == 3 && !strcmp(argv[1], "_close")) return close_main(atoi(argv[2]));
    if (argc == 3 && !strcmp(argv[1], "_sendpipe")) return sendpipe_main(atoi(argv[2]));
    if (argc < 2) { fprintf(2, "usage: nettest CHECK... (tcp udp poll nonblock names many perm restart partial sigpipe down giveup asm hang listen slip sliptcp slipget slipudp slipdns slipflood)\n"); return 2; }
    for (int i = 1; i < argc; i++) {
        const char *c = argv[i];
        if (!strcmp(c, "tcp")) c_tcp();
        else if (!strcmp(c, "udp")) c_udp();
        else if (!strcmp(c, "poll")) c_poll();
        else if (!strcmp(c, "nonblock")) c_nonblock();
        else if (!strcmp(c, "names")) c_names();
        else if (!strcmp(c, "many")) c_many();
        else if (!strcmp(c, "perm")) c_perm();
        else if (!strcmp(c, "restart")) c_restart();
        else if (!strcmp(c, "partial")) c_partial();
        else if (!strcmp(c, "sigpipe")) c_sigpipe();
        else if (!strcmp(c, "down")) c_down();
        else if (!strcmp(c, "giveup")) c_giveup();
        else if (!strcmp(c, "asm")) c_asm();
        else if (!strcmp(c, "hang")) c_hang();
        else if (!strcmp(c, "listen")) c_listen();
        else if (!strcmp(c, "slip")) { c_sliptcp(); c_slipget(); c_slipudp(); c_slipdns(); }
        else if (!strcmp(c, "sliptcp")) c_sliptcp();
        else if (!strcmp(c, "slipget")) c_slipget();
        else if (!strcmp(c, "slipudp")) c_slipudp();
        else if (!strcmp(c, "slipdns")) c_slipdns();
        else if (!strcmp(c, "slipflood")) c_slipflood();
        else { printf("nettest: no check %s\n", c); fails++; }
        oflush();
    }
    return fails;
}
