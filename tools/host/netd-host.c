/* SPDX-License-Identifier: GPL-2.0-only */
/* netd's core on the PC (DESIGN D19 8.1): netd (net/netd + lwIP) runs in a child process, as on Zenix it runs in a
 * process of its own, with SLIP over a socketpair to zenix-slirp; this process plays the kernel's socket layer and
 * the programs. The messages between the two are this test's own (the kernel's real NET_* protocol is the main
 * line's, D19 step 3), but they keep 4.3's rules: every reply carries the socket's new bits (NETSRV_REPLY), netd
 * pushes the bits that changed once per turn (NETSRV_READY), and the fake kernel blocks only on those bits, the way
 * sock.c's loop of rule 3 will: it checks that a reply of -EAGAIN never comes with bits that say "ready" (a spin).
 * netd's end is a kill of the child: the epoch goes up, every blocked call returns -ENETDOWN at once (rule 10), and
 * the next netd serves new sockets.
 *   netd-host ZENIX-SLIRP LOGDIR       the whole test (run in a network namespace by tools/host/net-test.sh)
 *   netd-host --config                 the configuration parsers alone */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "../../net/netd/netd.h"
#include "../../net/port/netd_port.h"

static int fails;
static void ok(int c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf(c ? "ok " : "FAIL netd-host: ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
    if (!c) fails++;
}
static long long ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- the messages between the fake kernel and netd (this test's own) ---- */
enum { OP_SOCKET = 1, OP_BIND, OP_CONNECT, OP_LISTEN, OP_ACCEPT, OP_SEND, OP_RECV, OP_SHUTDOWN, OP_GETOPT, OP_SETOPT,
       OP_GETNAME, OP_IOCTL, OP_CLOSE, OP_RESOLVE, OP_NETSTAT, OP_STATS, OP_IFINFO, OP_PUSH = 100 };
struct req {
    int op;
    unsigned idx, gen, nidx, ngen, uid;
    int a, b, c;
    unsigned alen;
    unsigned char addr[16];
    unsigned len;
    unsigned char data[65536] __attribute__((aligned(8)));
};
struct rep {
    int op;
    long r;
    unsigned bits;
    unsigned alen;
    unsigned char addr[16];
    unsigned len;
    unsigned char data[65536] __attribute__((aligned(8)));
};
#define REQ_HDR offsetof(struct req, data)
#define REP_HDR offsetof(struct rep, data)

/* =================== the netd side (the child) =================== */
static int kfd;                     /* netd's end of the message socket */
static const char *cfg_if, *cfg_resolv, *cfg_hosts;
static int line_fd;

unsigned long netd_os_now_ms(void) { return (unsigned long)ms_now(); }
unsigned long netd_os_clock4us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)((unsigned long long)t.tv_sec * 250000 + (unsigned long long)t.tv_nsec / 4000);
}
int netd_os_serial_open(const char *dev, unsigned long baud)
{
    (void)baud;
    if (!strncmp(dev, "fd:", 3)) {
        int fd = atoi(dev + 3);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        return fd;
    }
    int fd = open(dev, O_RDWR | O_NONBLOCK | O_NOCTTY);
    return fd < 0 ? -errno : fd;
}
long netd_os_read(int h, void *buf, unsigned long n)
{
    ssize_t r = read(h, buf, n);
    if (r < 0) return errno == EAGAIN || errno == EINTR ? 0 : -errno;
    return r ? r : -EIO;
}
long netd_os_write(int h, const void *buf, unsigned long n)
{
    ssize_t r = write(h, buf, n);
    if (r < 0) return errno == EAGAIN || errno == EINTR ? 0 : -errno;
    return r;
}
void netd_os_close(int h) { close(h); }
void *netd_os_alloc(unsigned long n) { return malloc(n); }
void netd_os_free(void *p) { free(p); }
static int config_mode;
static char logbuf[4096];
void netd_os_log(const char *line)
{
    if (config_mode) {                  /* the parsers' reports, for config_test to check */
        if (strlen(logbuf) + strlen(line) + 2 < sizeof logbuf) { strcat(logbuf, line); strcat(logbuf, "\n"); }
        return;
    }
    fprintf(stderr, "%s\n", line);
}
void netd_os_fatal(void) { fprintf(stderr, "netd: fatal\n"); _exit(70); }

static void netd_serve(struct req *q, struct rep *p)
{
    memset(p, 0, REP_HDR);
    p->op = q->op;
    p->alen = q->alen;
    switch (q->op) {
    case OP_SOCKET: p->r = netd_socket(q->idx, q->gen, q->a, q->b, q->c, q->uid); break;
    case OP_BIND: p->r = netd_bind(q->idx, q->gen, q->addr, q->alen, q->uid); break;
    case OP_CONNECT: p->r = netd_connect(q->idx, q->gen, q->addr, q->alen); break;
    case OP_LISTEN: p->r = netd_listen(q->idx, q->gen, q->a); break;
    case OP_ACCEPT: p->r = netd_accept(q->idx, q->gen, q->nidx, q->ngen, p->addr, &p->alen); break;
    case OP_SEND: p->r = netd_send(q->idx, q->gen, q->data, q->len, q->a, q->alen ? q->addr : NULL, q->alen); break;
    case OP_RECV:
        p->r = netd_recv(q->idx, q->gen, p->data, q->len, q->a, p->addr, &p->alen);
        if (p->r > 0) p->len = (unsigned)p->r;
        break;
    case OP_SHUTDOWN: p->r = netd_shutdown(q->idx, q->gen, q->a); break;
    case OP_GETOPT: p->len = q->len; p->r = netd_getsockopt(q->idx, q->gen, q->a, q->b, p->data, &p->len); break;
    case OP_SETOPT: p->r = netd_setsockopt(q->idx, q->gen, q->a, q->b, q->data, q->len); break;
    case OP_GETNAME: p->r = netd_getname(q->idx, q->gen, q->a, p->addr, &p->alen); break;
    case OP_IOCTL: { long v = 0; p->r = netd_ioctl(q->idx, q->gen, (unsigned long)q->a, &v); memcpy(p->data, &v, sizeof v); p->len = sizeof v; break; }
    case OP_CLOSE: p->r = netd_close(q->idx, q->gen); break;
    case OP_RESOLVE: q->data[q->len < sizeof q->data ? q->len : sizeof q->data - 1] = 0;
        p->r = netd_resolve(q->idx, q->gen, (char *)q->data, q->uid); break;
    case OP_NETSTAT: p->r = netd_netstat((struct netd_connrow *)p->data, sizeof p->data / sizeof(struct netd_connrow));
        p->len = (unsigned)(p->r > 0 ? p->r : 0) * sizeof(struct netd_connrow); break;
    case OP_STATS: {
        struct netd_stats s;
        unsigned long m[5];
        netd_get_stats(&s);
        m[0] = netd_mem_used; m[1] = netd_mem_limit; m[2] = s.udp_dropped; m[3] = s.accept_dropped; m[4] = s.mem_refused;
        memcpy(p->data, m, sizeof m);
        p->len = sizeof m;
        break;
    }
    case OP_IFINFO: p->r = netd_if_info(q->a, (struct netd_ifinfo *)p->data); p->len = sizeof(struct netd_ifinfo); break;
    default: p->r = -ENOSYS;
    }
    p->bits = netd_bits(q->idx, q->gen);        /* the reply and the new bits together (rule 1) */
    if (q->op == OP_ACCEPT && p->r == 0) {
        /* the new socket's bits ride on the same reply in the real protocol; here a push right after */
    }
}

static int netd_main(void)
{
    static struct req q;
    static struct rep p;
    struct netd_limits lim;
    struct netd_config c;
    unsigned char seed[48];
    int i;
    memset(&c, 0, sizeof c);
    if (getrandom(seed, sizeof seed, 0) != sizeof seed) return 1;
    netd_limits_profile(&lim, NETD_PROFILE_SMALL);
    if (getenv("NETD_HEAP")) lim.heap = strtoul(getenv("NETD_HEAP"), 0, 0);
    if (netd_init(&lim, seed)) { fprintf(stderr, "netd: bad limits\n"); return 1; }
    netd_parse_interfaces(cfg_if, strlen(cfg_if), &c);
    netd_parse_resolv(cfg_resolv, strlen(cfg_resolv), &c);
    netd_hosts_load(cfg_hosts, strlen(cfg_hosts));
    netd_set_dns(c.dns, c.ndns);
    netd_set_search(c.search);
    for (i = 0; i < c.nifs; i++)
        if (c.ifs[i].is_auto && netd_if_up(&c.ifs[i])) fprintf(stderr, "netd: %s would not come up\n", c.ifs[i].name);
    for (;;) {
        struct pollfd pf[1 + NETD_IFMAX];
        int h[NETD_IFMAX], ww[NETD_IFMAX], nh, np = 1;
        unsigned long t = netd_turn();
        struct netd_ready rd[NETD_NSOCK];
        int n = netd_changed(rd, NETD_NSOCK);
        if (n) {                         /* the push of this turn (NETSRV_READY) */
            unsigned char m[8 + sizeof rd];
            int op = OP_PUSH;
            memcpy(m, &op, 4); memcpy(m + 4, &n, 4); memcpy(m + 8, rd, n * sizeof rd[0]);
            if (send(kfd, m, 8 + n * sizeof rd[0], 0) < 0) return 0;
        }
        pf[0].fd = kfd; pf[0].events = POLLIN;
        nh = netd_wait_handles(h, ww, NETD_IFMAX);
        for (i = 0; i < nh; i++) { pf[np].fd = h[i]; pf[np].events = POLLIN | (ww[i] ? POLLOUT : 0); np++; }
        if (poll(pf, np, (int)t) < 0 && errno != EINTR) return 1;
        if (pf[0].revents & (POLLHUP | POLLERR)) return 0;
        if (pf[0].revents & POLLIN) {
            ssize_t r = recv(kfd, &q, sizeof q, 0);
            if (r <= 0) return 0;        /* the "kernel" is gone */
            netd_serve(&q, &p);
            if (send(kfd, &p, REP_HDR + p.len, 0) < 0) return 0;
        }
    }
}

/* =================== the kernel side (this process) =================== */
static int kconn = -1;              /* the kernel's end of the message socket to the running netd */
static pid_t netd_pid;
static unsigned epoch = 1;
static int spins;                   /* replies of -EAGAIN whose bits said "ready" (must stay 0) */
static struct ksock { int used; unsigned gen, bits, epoch; } ks[NETD_NSOCK];
static unsigned next_gen = 1;
static struct req kq;
static struct rep kp;
static const char *logdir;

static void apply_push(const unsigned char *m, ssize_t len)
{
    int n, i;
    struct netd_ready rd;
    memcpy(&n, m + 4, 4);
    for (i = 0; i < n && 8 + (i + 1) * (ssize_t)sizeof rd <= len; i++) {
        memcpy(&rd, m + 8 + i * sizeof rd, sizeof rd);
        if (rd.idx < NETD_NSOCK && ks[rd.idx].used && ks[rd.idx].gen == rd.gen && ks[rd.idx].epoch == epoch)
            ks[rd.idx].bits = rd.bits;
    }
}
static void netd_gone(void)
{
    int i;
    if (kconn >= 0) close(kconn);
    kconn = -1;
    epoch++;                        /* rule 9: every ksock of the old epoch is dead */
    for (i = 0; i < NETD_NSOCK; i++) if (ks[i].used) ks[i].bits = NETD_POLLERR | NETD_POLLHUP;
}
/* one message from netd within ms (a push: applied); 1 if one came, 0 on time out, -1 if netd is gone */
static int k_wait(int ms, int want_reply)
{
    static unsigned char buf[sizeof(struct rep)];
    struct pollfd pf;
    ssize_t r;
    int op;
    if (kconn < 0) return -1;
    pf.fd = kconn; pf.events = POLLIN;
    if (poll(&pf, 1, ms) <= 0) return 0;
    r = recv(kconn, buf, sizeof buf, 0);
    if (r <= 0) { netd_gone(); return -1; }
    memcpy(&op, buf, 4);
    if (op == OP_PUSH) { apply_push(buf, r); return 1; }
    if (want_reply) { memcpy(&kp, buf, (size_t)r); return 2; }
    return 1;
}
/* a call to netd: the request, then its reply (pushes on the way are applied) */
static long k_call(int op, unsigned idx)
{
    int w;
    kq.op = op;
    kq.idx = idx;
    kq.gen = idx < NETD_NSOCK ? ks[idx].gen : 0;
    if (idx < NETD_NSOCK && ks[idx].used && ks[idx].epoch != epoch) return -ENETDOWN;    /* the kernel answers (rule 9) */
    if (kconn < 0) return -ENETDOWN;
    if (send(kconn, &kq, REQ_HDR + (op == OP_SEND || op == OP_SETOPT || op == OP_RESOLVE ? kq.len : 0), 0) < 0) {
        netd_gone();
        return -ENETDOWN;
    }
    while ((w = k_wait(10000, 1)) != 2) if (w < 0) return -ENETDOWN;
    if (idx < NETD_NSOCK && ks[idx].used) ks[idx].bits = kp.bits;
    if (kp.r == -EAGAIN) {          /* rule 1: -EAGAIN comes with bits that say "not ready" */
        unsigned need = op == OP_SEND ? NETD_POLLOUT : NETD_POLLIN;
        if (kp.bits & need) { spins++; printf("FAIL netd-host: op %d on %u answered -EAGAIN with bits %#x\n", op, idx, kp.bits); fails++; }
    }
    return kp.r;
}

static int k_alloc(void)
{
    int i;
    for (i = 0; i < NETD_NSOCK; i++)
        if (!ks[i].used) { ks[i].used = 1; ks[i].gen = next_gen++ & 0xffff; ks[i].bits = 0; ks[i].epoch = epoch; return i; }
    return -1;
}
static void k_free(int i) { ks[i].used = 0; }

static int k_socket(int type, int proto, unsigned uid)
{
    int i = k_alloc();
    long r;
    if (i < 0) return -ENFILE;
    kq.a = NETD_AF_INET; kq.b = type; kq.c = proto; kq.uid = uid;
    r = k_call(OP_SOCKET, (unsigned)i);
    if (r < 0) { k_free(i); return (int)r; }
    return i;
}
static void mk_sin(unsigned char *a, const char *ip, unsigned port)
{
    unsigned short fam = NETD_AF_INET;
    struct in_addr ia;
    memset(a, 0, 16);
    memcpy(a, &fam, 2);
    a[2] = (unsigned char)(port >> 8); a[3] = (unsigned char)port;
    inet_aton(ip, &ia);
    memcpy(a + 4, &ia, 4);
}
static long k_bind(int s, const char *ip, unsigned port, unsigned uid)
{
    mk_sin(kq.addr, ip, port); kq.alen = 16; kq.uid = uid;
    return k_call(OP_BIND, (unsigned)s);
}
/* rule 3's loop: wait on the bits, call, and call again only after the bits changed */
static long k_block(int s, unsigned need, int op, int timeout_ms)
{
    long long end = ms_now() + timeout_ms;
    for (;;) {
        long r;
        if (ks[s].epoch != epoch) return -ENETDOWN;
        if (ks[s].bits & (need | NETD_POLLERR | NETD_POLLHUP)) {
            r = k_call(op, (unsigned)s);
            if (r != -EAGAIN) return r;
            continue;
        }
        if (ms_now() >= end) return -ETIMEDOUT;
        if (k_wait((int)(end - ms_now()), 0) < 0) return -ENETDOWN;
    }
}
static long k_connect(int s, const char *ip, unsigned port, int timeout_ms)
{
    long r;
    unsigned len = 4;
    mk_sin(kq.addr, ip, port); kq.alen = 16;
    r = k_call(OP_CONNECT, (unsigned)s);
    if (r != -EINPROGRESS) return r;
    /* rule 6: wait for POLLOUT|POLLERR, then ask SO_ERROR; CONNECT is never sent again */
    {
        long long end = ms_now() + timeout_ms;
        while (!(ks[s].bits & (NETD_POLLOUT | NETD_POLLERR))) {
            if (ks[s].epoch != epoch) return -ENETDOWN;
            if (ms_now() >= end) return -ETIMEDOUT;
            if (k_wait((int)(end - ms_now()), 0) < 0) return -ENETDOWN;
        }
    }
    kq.a = NETD_SOL_SOCKET; kq.b = NETD_SO_ERROR; kq.len = len;
    r = k_call(OP_GETOPT, (unsigned)s);
    if (r) return r;
    {
        int e;
        memcpy(&e, kp.data, 4);
        return -e;
    }
}
static long k_send(int s, const void *b, unsigned n, int block)
{
    kq.len = n; memcpy(kq.data, b, n); kq.a = 0; kq.alen = 0;
    return block ? k_block(s, NETD_POLLOUT, OP_SEND, 10000) : k_call(OP_SEND, (unsigned)s);
}
static long k_sendto(int s, const void *b, unsigned n, const char *ip, unsigned port)
{
    kq.len = n; memcpy(kq.data, b, n); kq.a = 0; mk_sin(kq.addr, ip, port); kq.alen = 16;
    return k_call(OP_SEND, (unsigned)s);
}
static long k_recv(int s, void *b, unsigned n, int block, int timeout_ms)
{
    long r;
    kq.len = n; kq.a = 0; kq.alen = 16;
    r = block ? k_block(s, NETD_POLLIN, OP_RECV, timeout_ms) : k_call(OP_RECV, (unsigned)s);
    if (r > 0) memcpy(b, kp.data, (size_t)r);
    return r;
}
static long k_close(int s)
{
    long r = k_call(OP_CLOSE, (unsigned)s);
    k_free(s);
    return r;
}
static int k_accept(int l, int timeout_ms)
{
    int n = k_alloc();
    long r;
    if (n < 0) return -ENFILE;
    kq.nidx = (unsigned)n; kq.ngen = ks[n].gen; kq.alen = 16;     /* reserved before the call (rule 7) */
    r = k_block(l, NETD_POLLIN, OP_ACCEPT, timeout_ms);
    if (r < 0) { k_free(n); return (int)r; }
    ks[n].bits = 0;
    return n;
}
static long k_resolve(const char *name, unsigned char *addrs, int timeout_ms)
{
    int i = k_alloc();
    long r;
    if (i < 0) return -ENFILE;
    kq.len = (unsigned)strlen(name) + 1; memcpy(kq.data, name, kq.len); kq.uid = 1000;
    r = k_call(OP_RESOLVE, (unsigned)i);
    if (r < 0) { k_free(i); return r; }
    r = k_recv(i, addrs, 16, 1, timeout_ms);
    k_close(i);
    return r;
}

static void start_netd(const char *ifc)
{
    int sp[2];
    pid_t pid;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp)) { perror("socketpair"); exit(2); }
    pid = fork();
    if (pid == 0) {
        char name[256];
        snprintf(name, sizeof name, "%s/netd-host.netd.log", logdir);
        if (!freopen(name, "a", stderr)) _exit(2);
        setvbuf(stderr, NULL, _IONBF, 0);
        close(sp[0]);
        kfd = sp[1];
        cfg_if = ifc;
        cfg_resolv = "nameserver 10.0.2.3\noptions edns0\nsearch zenix.test\n";
        cfg_hosts = "127.0.0.1 localhost\n::1 localhost\n10.9.9.9 fromhosts.zenix.test fromhosts\n";
        _exit(netd_main());
    }
    close(sp[1]);
    kconn = sp[0];
    netd_pid = pid;
}

/* ---- the PC's servers (10.0.2.2 for the Amiga): TCP and UDP echo, a DNS resolver on 53 ---- */
static void *tcp_echo_conn(void *a)
{
    int c = (int)(long)a;
    char b[4096];
    ssize_t n;
    while ((n = recv(c, b, sizeof b, 0)) > 0) send(c, b, (size_t)n, MSG_NOSIGNAL);
    close(c);
    return NULL;
}
static void *tcp_echo(void *a)
{
    int l = (int)(long)a;
    for (;;) {
        pthread_t t;
        int c = accept(l, NULL, NULL);
        if (c < 0) continue;
        pthread_create(&t, NULL, tcp_echo_conn, (void *)(long)c);
        pthread_detach(t);
    }
    return NULL;
}
static void *udp_echo(void *a)
{
    int s = (int)(long)a;
    char b[2048];
    struct sockaddr_in f;
    for (;;) {
        socklen_t fl = sizeof f;
        ssize_t n = recvfrom(s, b, sizeof b, 0, (struct sockaddr *)&f, &fl);
        if (getenv("NETD_HOST_SLIRP_V")) fprintf(stderr, "udp echo: %zd bytes from port %u\n", n, ntohs(f.sin_port));
        if (n > 0) sendto(s, b, (size_t)n, 0, (struct sockaddr *)&f, fl);
    }
    return NULL;
}
static void *resolver(void *a)
{
    int s = (int)(long)a;
    unsigned char q[512], r[600];
    struct sockaddr_in f;
    for (;;) {
        socklen_t fl = sizeof f;
        ssize_t n = recvfrom(s, q, sizeof q, 0, (struct sockaddr *)&f, &fl);
        int i = 12, found;
        char name[256];
        unsigned nl = 0;
        if (n < 17) continue;
        while (i < n && q[i] && nl + q[i] + 1 < sizeof name) {
            if (nl) name[nl++] = '.';
            memcpy(name + nl, q + i + 1, q[i]); nl += q[i]; i += 1 + q[i];
        }
        name[nl] = 0;
        if (!strcasecmp(name, "slow.zenix.test")) continue;     /* never answered: netd must give up (-ETIMEDOUT) */
        found = !strcasecmp(name, "amiga.zenix.test");
        memcpy(r, q, (size_t)i + 5);
        r[2] = 0x81; r[3] = found ? 0x80 : 0x83;
        r[6] = 0; r[7] = (unsigned char)found; r[8] = r[9] = r[10] = r[11] = 0;
        n = i + 5;
        if (found) {
            static const unsigned char ans[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 1, 2, 3 };
            memcpy(r + n, ans, sizeof ans); n += sizeof ans;
        }
        sendto(s, r, (size_t)n, 0, (struct sockaddr *)&f, fl);
    }
    return NULL;
}
static void serve(int type, int port, void *(*fn)(void *))
{
    int s = socket(AF_INET, type, 0), one = 1;
    struct sockaddr_in a;
    pthread_t t;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&a, sizeof a)) { perror("bind"); exit(2); }
    if (type == SOCK_STREAM) listen(s, 16);
    pthread_create(&t, NULL, fn, (void *)(long)s);
    pthread_detach(t);
}

static unsigned long sum32(const unsigned char *p, unsigned long n)
{
    unsigned long s = 0, i;
    for (i = 0; i < n; i++) s = s * 31 + p[i];
    return s;
}

/* ---- the tests ---- */
static void test_loopback(void)
{
    int l, c, a;
    char b[64];
    long r;
    l = k_socket(NETD_SOCK_STREAM, 0, 1000);
    ok(l >= 0 && k_bind(l, "127.0.0.1", 5000, 1000) == 0, "loopback: a TCP socket bound to 127.0.0.1:5000");
    kq.a = 8;
    ok(k_call(OP_LISTEN, (unsigned)l) == 0, "loopback: listen");
    c = k_socket(NETD_SOCK_STREAM, 0, 1000);
    ok(k_connect(c, "127.0.0.1", 5000, 3000) == 0, "loopback: connect (EINPROGRESS, then POLLOUT and SO_ERROR 0)");
    a = k_accept(l, 3000);
    ok(a >= 0, "loopback: accept gives the connection");
    if (a < 0) return;
    ok(k_recv(a, b, sizeof b, 0, 0) == -EAGAIN, "loopback: recv with nothing there is -EAGAIN");
    ok(k_send(c, "hello", 5, 1) == 5, "loopback: send 5 bytes");
    r = k_recv(a, b, sizeof b, 1, 3000);
    ok(r == 5 && !memcmp(b, "hello", 5), "loopback: the other end reads them (%ld)", r);
    ok(k_send(a, "back", 4, 1) == 4 && k_recv(c, b, sizeof b, 1, 3000) == 4 && !memcmp(b, "back", 4), "loopback: and back");
    kq.a = 0; kq.alen = 16;
    ok(k_call(OP_GETNAME, (unsigned)a) == 0 && kp.addr[2] == (5000 >> 8) && kp.addr[3] == (5000 & 255),
       "loopback: getsockname of the accepted socket: port 5000");
    ok(k_close(c) == 0, "loopback: close one end");
    r = k_recv(a, b, sizeof b, 1, 3000);
    ok(r == 0, "loopback: the other end reads the end of the stream (%ld)", r);
    r = k_send(a, "x", 1, 0);
    ok(r == 1 || r == -EPIPE || r == -ECONNRESET, "loopback: a write after the peer closed (%ld)", r);
    k_close(a);
    k_close(l);
}

static void test_tcp_slip(void)
{
    enum { N = 100 * 1024 };
    static unsigned char out[N], in[N];
    unsigned long sent = 0, got = 0, i;
    long long t0, end;
    int s = k_socket(NETD_SOCK_STREAM, 0, 1000);
    long r;
    for (i = 0; i < N; i++) out[i] = (unsigned char)(i * 7 + (i >> 9));
    r = k_connect(s, "10.0.2.2", 7778, 5000);
    ok(r == 0, "SLIP: TCP connect to the PC's echo server at 10.0.2.2:7778 (%ld)", r);
    if (r) { k_close(s); return; }
    t0 = ms_now();
    end = t0 + 60000;
    while ((sent < N || got < N) && ms_now() < end) {
        if (ks[s].epoch != epoch) break;
        if (sent < N && (ks[s].bits & NETD_POLLOUT)) {
            unsigned n = N - sent > 8192 ? 8192 : (unsigned)(N - sent);
            r = k_send(s, out + sent, n, 0);
            if (r > 0) sent += (unsigned long)r;
            else if (r != -EAGAIN) { ok(0, "SLIP: send failed %ld", r); break; }
            continue;
        }
        if (ks[s].bits & NETD_POLLIN) {
            r = k_recv(s, in + got, (unsigned)(N - got > 16384 ? 16384 : N - got), 0, 0);
            if (r > 0) got += (unsigned long)r;
            else if (r != -EAGAIN) { ok(0, "SLIP: recv failed %ld", r); break; }
            continue;
        }
        k_wait(200, 0);
    }
    ok(sent == N && got == N && sum32(in, N) == sum32(out, N) && !memcmp(in, out, N),
       "SLIP: 100 KB sent and echoed back, the same bytes (%lu sent, %lu back, %lld ms)", sent, got, ms_now() - t0);
    k_close(s);
}

static void test_udp_slip(void)
{
    int s = k_socket(NETD_SOCK_DGRAM, 0, 1000);
    char b[64];
    long r;
    unsigned port;
    ok(s >= 0, "UDP: a socket");
    ok(k_sendto(s, "udp over slip", 13, "10.0.2.2", 7777) == 13, "UDP: sendto 10.0.2.2:7777");
    r = k_recv(s, b, sizeof b, 1, 5000);
    port = (unsigned)kp.addr[2] << 8 | kp.addr[3];
    ok(r == 13 && !memcmp(b, "udp over slip", 13) && port == 7777, "UDP: the echo, from port %u (%ld)", port, r);
    {
        static unsigned char big[2000];
        r = k_sendto(s, big, 1200, "10.0.2.2", 7777);
        ok(r == -EMSGSIZE, "UDP: a datagram bigger than SLIP's MTU is -EMSGSIZE, never fragments (%ld)", r);
    }
    k_close(s);
}

static void test_dns(void)
{
    unsigned char a[16];
    long r;
    r = k_resolve("amiga.zenix.test", a, 8000);
    ok(r == 4 && a[0] == 10 && a[1] == 1 && a[2] == 2 && a[3] == 3, "DNS: amiga.zenix.test is 10.1.2.3 (from the test resolver through 10.0.2.3) (%ld)", r);
    r = k_resolve("amiga.zenix.test", a, 1000);
    ok(r == 4 && a[3] == 3, "DNS: asked again, from the cache");
    r = k_resolve("nothing.zenix.test", a, 8000);
    ok(r == -ENOENT, "DNS: a missing name is -ENOENT (%ld)", r);
    r = k_resolve("fromhosts", a, 1000);
    ok(r == 4 && a[0] == 10 && a[1] == 9, "DNS: /etc/hosts first (%ld)", r);
    r = k_resolve("10.0.2.99", a, 1000);
    ok(r == 4 && a[3] == 99, "DNS: a number is answered at once");
    r = k_resolve("amiga", a, 8000);
    ok(r == 4 && a[0] == 10 && a[3] == 3, "DNS: a name without a dot gets the search domain (amiga.zenix.test) (%ld)", r);
    r = k_resolve("slow.zenix.test", a, 30000);
    ok(r == -ETIMEDOUT, "DNS: a name no server answers is -ETIMEDOUT, not -ENOENT (%ld)", r);
}

static void test_icmp(void)
{
    int s = k_socket(NETD_SOCK_DGRAM, NETD_IPPROTO_ICMP, 1000);
    unsigned char e[16] = { 8, 0, 0, 0, 0, 0, 0, 1, 'z', 'e', 'n', 'i', 'x', '!', '!', '!' }, b[64];
    long r;
    unsigned id;
    ok(s >= 0, "ICMP: an echo socket for a user (Linux's ping sockets)");
    kq.a = 0; kq.alen = 16;
    k_call(OP_GETNAME, (unsigned)s);
    id = (unsigned)kp.addr[2] << 8 | kp.addr[3];
    ok(k_sendto(s, e, sizeof e, "10.0.2.2", 0) == (long)sizeof e, "ICMP: an echo request to 10.0.2.2");
    r = k_recv(s, b, sizeof b, 1, 5000);
    ok(r == (long)sizeof e && b[0] == 0 && ((unsigned)b[4] << 8 | b[5]) == id && !memcmp(b + 8, "zenix!!!", 8),
       "ICMP: the echo reply, with netd's echo id %u (%ld)", id, r);
    e[0] = 13;
    ok(k_sendto(s, e, sizeof e, "10.0.2.2", 0) == -EINVAL, "ICMP: anything but an echo request is refused");
    k_close(s);
}

static void test_rules(void)
{
    int s = k_socket(NETD_SOCK_STREAM, 0, 1000), r2;
    long r;
    ok(k_bind(s, "0.0.0.0", 80, 1000) == -EACCES, "a user may not bind port 80");
    ok(k_bind(s, "0.0.0.0", 80, 0) == 0, "root may");
    k_close(s);
    s = k_socket(NETD_SOCK_DGRAM, 0, 1000);
    ok(k_bind(s, "10.0.2.77", 4000, 1000) == -EADDRNOTAVAIL, "bind to an address that is not netd's: -EADDRNOTAVAIL");
    k_close(s);
    /* SO_REUSEADDR never shares a port between users */
    s = k_socket(NETD_SOCK_STREAM, 0, 1000);
    r2 = k_socket(NETD_SOCK_STREAM, 0, 1001);
    {
        int one = 1;
        kq.a = NETD_SOL_SOCKET; kq.b = NETD_SO_REUSEADDR; kq.len = 4; memcpy(kq.data, &one, 4);
        k_call(OP_SETOPT, (unsigned)s);
        kq.a = NETD_SOL_SOCKET; kq.b = NETD_SO_REUSEADDR; kq.len = 4; memcpy(kq.data, &one, 4);
        k_call(OP_SETOPT, (unsigned)r2);
    }
    ok(k_bind(s, "0.0.0.0", 6000, 1000) == 0 && k_bind(r2, "0.0.0.0", 6000, 1001) == -EADDRINUSE,
       "SO_REUSEADDR: a second user cannot take the same port");
    k_close(s); k_close(r2);
    /* a name netd does not know (a wrong generation) */
    s = k_socket(NETD_SOCK_DGRAM, 0, 1000);
    ks[s].gen ^= 0x5555;
    r = k_call(OP_RECV, (unsigned)s);
    ok(r == -ENETDOWN, "an (index, generation) netd does not know is -ENETDOWN (rule 9) (%ld)", r);
    ks[s].gen ^= 0x5555;
    k_close(s);
    /* connect to a closed port on the PC: refused */
    s = k_socket(NETD_SOCK_STREAM, 0, 1000);
    r = k_connect(s, "10.0.2.2", 9, 8000);
    ok(r == -ECONNREFUSED || r == -ECONNRESET, "connect to a closed port on the PC is refused (%ld)", r);
    k_close(s);
}

static void test_limits(void)
{
    int l, c[12], a[12], i, nc = 0, na = 0, refused = 0, n;
    unsigned long m[5];
    /* the backlog: listen(2), three clients; two wait in the queue, the third's SYN is dropped */
    l = k_socket(NETD_SOCK_STREAM, 0, 1000);
    k_bind(l, "127.0.0.1", 5001, 1000);
    kq.a = 2;
    ok(k_call(OP_LISTEN, (unsigned)l) == 0, "limits: listen with a backlog of 2");
    for (i = 0; i < 3; i++) {
        c[i] = k_socket(NETD_SOCK_STREAM, 0, 1000);
        kq.alen = 16; mk_sin(kq.addr, "127.0.0.1", 5001);
        if (c[i] >= 0 && k_call(OP_CONNECT, (unsigned)c[i]) == -EINPROGRESS) nc++;
    }
    for (i = 0; i < 20; i++) k_wait(50, 0);     /* let the handshakes run */
    while (na < 3 && (a[na] = k_accept(l, 300)) >= 0) na++;
    ok(nc == 3 && na == 2 && !(ks[c[2]].bits & NETD_POLLOUT),
       "limits: 3 connects to a backlog of 2: 2 accepted, the third still waits for its SYN-ACK (%d, %d)", nc, na);
    for (i = 0; i < na; i++) k_close(a[i]);
    for (i = 0; i < 3; i++) if (c[i] >= 0) k_close(c[i]);
    /* a listen() asking for more than the machine's cap (4) gets the cap */
    kq.a = 100;
    ok(k_call(OP_LISTEN, (unsigned)l) == 0, "limits: listen again asking for a backlog of 100 (capped at 4)");
    k_close(l);
    for (i = 0; i < 20; i++) k_wait(50, 0);
    /* the pool of TCP connections (8 on the stock A1200): the ninth socket is -ENOBUFS, netd lives */
    for (n = 0; n < 12; n++) {
        c[n] = k_socket(NETD_SOCK_STREAM, 0, 1000);
        if (c[n] < 0) { refused = c[n]; break; }
    }
    ok(refused == -ENOBUFS && n <= 8, "limits: %d TCP sockets, then -ENOBUFS (the pool holds 8 with TIME_WAITs) (%d)", n, refused);
    for (i = 0; i < n; i++) k_close(c[i]);
    kq.len = 0;
    k_call(OP_STATS, (unsigned)NETD_NSOCK);
    memcpy(m, kp.data, sizeof m);
    ok(m[0] <= m[1] || !m[1], "limits: netd's heap stays within its limit (%lu of %lu bytes)", m[0], m[1]);
    /* after all that a new connection works */
    for (i = 0; i < 10; i++) k_wait(50, 0);
    {
        int s = k_socket(NETD_SOCK_STREAM, 0, 1000);
        long r = k_connect(s, "10.0.2.2", 7778, 8000);
        char b[8];
        ok(r == 0 && k_send(s, "again", 5, 1) == 5 && k_recv(s, b, sizeof b, 1, 5000) == 5,
           "limits: afterwards a new connection works (%ld)", r);
        k_close(s);
    }
}

static void test_netstat(void)
{
    int l = k_socket(NETD_SOCK_STREAM, 0, 1000), n, i, seen = 0;
    k_bind(l, "0.0.0.0", 5002, 1000);
    kq.a = 2;
    k_call(OP_LISTEN, (unsigned)l);
    n = (int)k_call(OP_NETSTAT, NETD_NSOCK);
    for (i = 0; i < n; i++) {
        struct netd_connrow r;
        memcpy(&r, kp.data + i * sizeof r, sizeof r);
        if (r.proto == NETD_IPPROTO_TCP && r.lport == 5002 && r.state == 1 && r.uid == 1000) seen = 1;
    }
    ok(seen, "netstat lists the listener on port 5002 with its user (%d rows)", n);
    k_close(l);
}

static void *killer(void *a)
{
    (void)a;
    usleep(400000);
    kill(netd_pid, SIGKILL);
    return NULL;
}
static void test_netd_end(const char *ifc)
{
    int s, s2, st;
    char b[16];
    long r;
    long long t0;
    pthread_t t;
    s = k_socket(NETD_SOCK_STREAM, 0, 1000);
    r = k_connect(s, "10.0.2.2", 7778, 5000);
    ok(r == 0, "netd's end: a connection to the PC");
    s2 = k_socket(NETD_SOCK_DGRAM, 0, 1000);
    pthread_create(&t, NULL, killer, NULL);
    t0 = ms_now();
    r = k_recv(s, b, sizeof b, 1, 20000);       /* blocked: nothing comes, then netd dies */
    ok(r == -ENETDOWN && ms_now() - t0 < 3000, "netd's end: a blocked recv returns -ENETDOWN at once (%ld, %lld ms)", r, ms_now() - t0);
    pthread_join(t, NULL);
    waitpid(netd_pid, &st, 0);
    ok(k_send(s, "x", 1, 0) == -ENETDOWN && k_recv(s2, b, sizeof b, 0, 0) == -ENETDOWN,
       "netd's end: every later call on an old socket is -ENETDOWN");
    k_free(s); k_free(s2);                       /* close only: the kernel frees them, nothing goes to netd */
    start_netd(ifc);
    s = k_socket(NETD_SOCK_STREAM, 0, 1000);
    r = k_connect(s, "10.0.2.2", 7778, 8000);
    ok(r == 0 && k_send(s, "fresh", 5, 1) == 5 && k_recv(s, b, sizeof b, 1, 5000) == 5 && !memcmp(b, "fresh", 5),
       "netd's end: the next netd serves a new connection (%ld)", r);
    k_close(s);
}

static void test_ifinfo(void)
{
    struct netd_ifinfo f;
    int i, found = 0;
    for (i = 0; i < 4; i++) {
        kq.a = i;
        if (k_call(OP_IFINFO, NETD_NSOCK) != 0) break;
        memcpy(&f, kp.data, sizeof f);
        if (!strcmp(f.name, "sl0") && f.mtu == 1006 && f.up && f.rx_frames > 10 && f.tx_frames > 10) found = 1;
    }
    ok(found, "ifconfig's numbers: sl0 up, MTU 1006, frames counted both ways");
}

/* ---- the parsers alone ---- */
static int config_test(void)
{
    struct netd_config c;
    int n;
    const char *ifc =
        "# a comment\n"
        "auto eth0 sl0\n"
        "iface eth0 inet dhcp\n"
        "    driver genet\n"
        "    hostname amiga\n"
        "\n"
        "iface sl0 inet static\n"
        "    driver slip /dev/ttyS0 115200\n"
        "    address 10.0.2.15/24\n"
        "    gateway 10.0.2.2\n"
        "    mtu 1006\n"
        "    frobnicate yes\n"
        "iface bad inet loopback\n"
        "    address 1.2.3.4/8\n"
        "iface eth1 inet static\n"
        "    driver ../evil\n"
        "    address 192.168.1.300/24\n";
    memset(&c, 0, sizeof c);
    logbuf[0] = 0;
    n = netd_parse_interfaces(ifc, strlen(ifc), &c);
    ok(c.nifs == 3 && !strcmp(c.ifs[0].name, "eth0") && c.ifs[0].method == NETD_INET_DHCP && c.ifs[0].is_auto &&
       !strcmp(c.ifs[0].driver, "genet") && !strcmp(c.ifs[0].hostname, "amiga"), "interfaces: the dhcp stanza");
    ok(!strcmp(c.ifs[1].name, "sl0") && c.ifs[1].method == NETD_INET_STATIC && c.ifs[1].is_auto &&
       !strcmp(c.ifs[1].driver, "slip") && !strcmp(c.ifs[1].args, "/dev/ttyS0 115200") &&
       c.ifs[1].addr == inet_addr("10.0.2.15") && c.ifs[1].mask == inet_addr("255.255.255.0") &&
       c.ifs[1].gateway == inet_addr("10.0.2.2") && c.ifs[1].mtu == 1006, "interfaces: the static SLIP stanza");
    ok(!c.ifs[2].is_auto && !c.ifs[2].driver[0] && !c.ifs[2].addr, "interfaces: a driver with '/' and a bad address are refused");
    ok(n == 6 && strstr(logbuf, "unknown word 'frobnicate'") && strstr(logbuf, "unknown method (dhcp or static) 'loopback'") &&
       strstr(logbuf, "bad driver name '../evil'"),
       "interfaces: unknown words reported with their line and ignored, an ignored stanza's lines with it (%d reports)", n);
    {
        const char *rc = "nameserver 10.0.2.3\nnameserver ::1\nnameserver 8.8.8.8\nnameserver 1.1.1.1\nsearch lan\noptions x\nbogus\n";
        n = netd_parse_resolv(rc, strlen(rc), &c);
    }
    ok(c.ndns == 2 && c.dns[0] == inet_addr("10.0.2.3") && c.dns[1] == inet_addr("8.8.8.8") && !strcmp(c.search, "lan") && n == 2,
       "resolv.conf: two name servers, IPv6 skipped, a third and an unknown word reported (%d)", n);
    ok(netd_config_trusted(0100644, 0) && !netd_config_trusted(0100664, 0) && !netd_config_trusted(0100644, 1000),
       "interfaces is used only when root owns it and nobody else may write it");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *zs;
    char slipfd[16], ifc[512];
    int sp[2];
    pid_t zp;
    if (argc == 2 && !strcmp(argv[1], "--config")) {
        int r;
        config_mode = 1;
        r = config_test();
        printf("netd-host --config: %s\n", r ? "FAILED" : "all ok");
        if (r) printf("the reports:\n%s", logbuf);
        return r;
    }
    if (argc != 3) { fprintf(stderr, "usage: netd-host ZENIX-SLIRP LOGDIR | --config\n"); return 2; }
    zs = argv[1];
    logdir = argv[2];
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    serve(SOCK_STREAM, 7778, tcp_echo);
    serve(SOCK_DGRAM, 7777, udp_echo);
    serve(SOCK_DGRAM, 53, resolver);

    /* the SLIP line: one end to zenix-slirp, the other kept for every netd this test starts */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp)) { perror("socketpair"); return 2; }
    zp = fork();
    if (zp == 0) {
        char fd[16], log[256], err[256];
        snprintf(fd, sizeof fd, "%d", sp[1]);
        snprintf(log, sizeof log, "%s/netd-host.console", logdir);
        snprintf(err, sizeof err, "%s/netd-host.slirp.log", logdir);
        close(sp[0]);
        if (!freopen(err, "w", stderr)) _exit(2);
        if (getenv("NETD_HOST_SLIRP_V")) execl(zs, zs, "-v", "-v", "--fd", fd, "--log", log, (char *)NULL);
        execl(zs, zs, "--fd", fd, "--log", log, (char *)NULL);
        _exit(127);
    }
    close(sp[1]);
    line_fd = sp[0];
    snprintf(slipfd, sizeof slipfd, "fd:%d", line_fd);
    snprintf(ifc, sizeof ifc, "auto sl0\niface sl0 inet static\n    driver slip %s 115200\n    address 10.0.2.15/24\n"
             "    gateway 10.0.2.2\n", slipfd);
    usleep(200000);
    start_netd(ifc);

    test_loopback();
    test_udp_slip();
    test_icmp();
    test_dns();
    test_tcp_slip();
    test_rules();
    test_netstat();
    test_limits();
    test_ifinfo();
    test_netd_end(ifc);
    ok(spins == 0, "no reply of -EAGAIN came with bits that said ready (%d)", spins);

    kill(netd_pid, SIGTERM);
    close(kconn);
    waitpid(netd_pid, NULL, 0);
    close(line_fd);
    kill(zp, SIGTERM);
    waitpid(zp, NULL, 0);
    printf("netd-host: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
