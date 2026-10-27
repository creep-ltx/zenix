/* SPDX-License-Identifier: GPL-2.0-only */
/* netd, Zenix's network server (DESIGN D19, docs/d19-network.md; docs/man/netd.txt): the service "net" that boot
 * registers as /sbin/netd and the kernel starts at the first socket call. This file is netd's main program on Zenix:
 * the operating system layer the core asks for (netd.h: the clocks, the serial port, the log) and the message loop
 * that turns the kernel's NET_* requests (kernel/netproto.h) into the core's calls (net/netd: sockets, interfaces,
 * names over lwIP's raw API). The rules of 4.3 it keeps: every kernel request is answered at once with NETSRV_REPLY
 * (the result and the socket's new ready bits in one trap); the bits that changed otherwise are pushed with
 * NETSRV_READY once a turn; the only waits are in the loop's evwait (the port, the serial lines, the timers). */
#include "../../user/libz.h"
#include "../../kernel/netproto.h"
#include "netd.h"
#include "../port/netd_port.h"

/* ---- the operating system layer (netd.h, netd_port.h) ---- */
unsigned long netd_os_now_ms(void) { return (unsigned long)sys_ticks() * 10; }
/* RFC 6528's clock: the E-clock (709 or 716 kHz) counts 1.4 us; a quarter of it is near enough to 4 us */
unsigned long netd_os_clock4us(void) { return (unsigned long)sys_hrtime() >> 2; }

void netd_os_log(const char *line)
{
    char b[200];
    u32 n = strlen(line);
    if (n > sizeof b - 1) n = sizeof b - 1;
    memcpy(b, line, n);
    b[n++] = '\n';
    sys_write(1, b, n);                 /* a service's output is the kernel log (dmesg) */
}
void netd_os_fatal(void) { sys_exit(70); }

/* a serial line for SLIP (D19 5.3): /dev/ttyS0 at 115,200 baud 8N1, never waiting */
int netd_os_serial_open(const char *dev, unsigned long baud)
{
    if (baud != 115200) return -EINVAL;         /* Zenix's serial port runs at 115,200 only (D12) */
    int fd = sys_open(dev, O_RDWR | O_NONBLOCK);
    return fd;
}
long netd_os_read(int h, void *buf, unsigned long n)
{
    int r = sys_read(h, buf, (u32)n);
    if (r == -EAGAIN || r == -EINTR) return 0;
    return r ? r : -EIO;
}
long netd_os_write(int h, const void *buf, unsigned long n)
{
    int r = sys_write(h, buf, (u32)n);          /* O_NONBLOCK: what fits in the send ring now */
    return r == -EAGAIN || r == -EINTR ? 0 : r;
}
void netd_os_close(int h) { sys_close(h); }

/* ---- the machine's profile (D19 7.3): small on chip RAM alone, large with enough fast RAM ---- */
#define LARGE_FAST (2u << 20)                   /* free fast RAM for the large profile (measured: section 14) */
static int profile(void)
{
    u32 m[4];
    if (sys_meminfo(m) < 0) return NETD_PROFILE_SMALL;
    return m[1] >= LARGE_FAST ? NETD_PROFILE_LARGE : NETD_PROFILE_SMALL;
}

/* ---- random keys (D19 9.3): from /dev/urandom before lwIP starts; fresh ones when /dev/random says the pool is
 * seeded, and every RESEED ticks after ---- */
#define RESEED (5 * 60 * 100)
static int rnd_fd = -1;                         /* /dev/random until the pool is seeded, then closed */
static u32 last_seed;
static int seed(unsigned char s[48], const char *dev)
{
    int fd = sys_open(dev, O_RDONLY), n = fd < 0 ? fd : sys_read(fd, s, 48);
    if (fd >= 0) sys_close(fd);
    return n == 48 ? 0 : -1;
}
static void reseed(const char *dev)
{
    unsigned char s[48];
    if (!seed(s, dev)) netd_reseed(s);
    memset(s, 0, sizeof s);
    last_seed = sys_ticks();
}

/* ---- the configuration files (D19 6.1): read at the start (and on a reload), never per request ---- */
#define CONF_MAX 16384
static char *slurp(const char *path, unsigned long *len)
{
    struct ustat st;
    int fd = sys_open(path, O_RDONLY);
    char *b = 0;
    *len = 0;
    if (fd < 0) return 0;
    if (sys_fstat(fd, &st) >= 0 && st.size <= CONF_MAX && (b = malloc(st.size + 1))) {
        int n = sys_read(fd, b, st.size);
        *len = n > 0 ? (unsigned long)n : 0;
        b[*len] = 0;
    } else if (st.size > CONF_MAX)
        printf("netd: %s: over %u bytes, not read\n", path, CONF_MAX);
    sys_close(fd);
    return b;
}
static struct netd_config conf;
static void load_config(void)
{
    unsigned long n;
    char *t = slurp("/etc/hosts", &n);
    netd_hosts_load(t ? t : "", n);
    free(t);
    memset(&conf, 0, sizeof conf);
    if ((t = slurp("/etc/resolv.conf", &n))) netd_parse_resolv(t, n, &conf);
    free(t);
    netd_set_dns(conf.dns, conf.ndns);
}

/* ---- the message loop ---- */
static int port;
static u32 epoch;
#define NAME_IDX(n) ((n) & 0xFF)
#define NAME_GEN(n) (((n) >> 8) & 0xFFFF)

/* an address length the program gets back: the true one (BSD's in-out socklen_t) */
static void put_len(u32 p, unsigned v) { if (p) *(u32 *)p = v; }

static struct netd_connrow rows[64];
static long do_netstat(struct netreq *q)
{
    u32 rs = q->r.flags ? q->r.flags : UNETSTAT_FIRST, max, i, n;
    if (rs < UNETSTAT_FIRST || rs > 4096) return -EINVAL;
    max = q->len / rs;
    n = (u32)netd_netstat(rows, max < 64 ? (int)max : 64);
    for (i = 0; i < n; i++) {
        struct unetstat u;
        unsigned char *o = (unsigned char *)q->r.buf + i * rs;
        memset(&u, 0, sizeof u);
        u.proto = rows[i].proto;
        u.state = rows[i].state;
        u.lport = rows[i].lport;
        u.laddr = rows[i].laddr;
        u.raddr = rows[i].raddr;
        u.rport = rows[i].rport;
        u.rxq = rows[i].rxq;
        u.txq = rows[i].txq;
        u.uid = rows[i].uid;
        memset(o, 0, rs);
        memcpy(o, &u, rs < sizeof u ? rs : sizeof u);
    }
    return (long)n;
}

/* one kernel request: the result (>= 0 or -errno); the core writes into the program's memory in place, never more
 * than the lengths the kernel checked (netreq's len, alen, vlen) */
static long serve(struct umsg *m)
{
    struct netreq *q = (struct netreq *)m->a[1];
    struct usockreq *r = &q->r;
    unsigned i = NAME_IDX(m->a[0]), g = NAME_GEN(m->a[0]), al = q->alen;
    void *addr = r->addr ? (void *)r->addr : 0;
    long v;
    switch (m->type) {
    case SOCK_SOCKET:
        return netd_socket(i, g, r->level, (int)r->flags, r->option, q->uid);
    case SOCK_BIND:
        return netd_bind(i, g, addr, al, q->uid);
    case SOCK_CONNECT:
        return netd_connect(i, g, addr, al);
    case SOCK_LISTEN:
        return netd_listen(i, g, (int)(r->len > 255 ? 255 : r->len));
    case SOCK_ACCEPT:
        v = netd_accept(i, g, NAME_IDX(q->newsock), NAME_GEN(q->newsock), addr, addr ? &al : 0);
        if (v >= 0 && addr) put_len(r->alen, al);
        return v;
    case SOCK_SENDTO:
        return netd_send(i, g, (const void *)r->buf, q->len, (int)r->flags, addr, al);
    case SOCK_RECVFROM:
        v = netd_recv(i, g, (void *)r->buf, q->len, (int)r->flags, addr, addr ? &al : 0);
        if (v >= 0 && addr) put_len(r->alen, al);
        return v;
    case SOCK_SHUTDOWN:
        return netd_shutdown(i, g, (int)r->flags);
    case SOCK_GETSOCKOPT: {
        unsigned vl = q->vlen;
        v = netd_getsockopt(i, g, r->level, r->option, (void *)r->val, &vl);
        if (!v) put_len(r->vlen, vl);
        return v;
    }
    case SOCK_SETSOCKOPT:
        return netd_setsockopt(i, g, r->level, r->option, (const void *)r->val, q->vlen);
    case SOCK_GETSOCKNAME:
    case SOCK_GETPEERNAME:
        v = netd_getname(i, g, m->type == SOCK_GETPEERNAME, addr, &al);
        if (!v) put_len(r->alen, al);
        return v;
    case SOCK_IOCTL: {
        long n = 0;
        v = netd_ioctl(i, g, r->option, &n);    /* FIONREAD: the core's long, the program's int */
        if (!v) *(int *)r->val = (int)n;
        return v;
    }
    case SOCK_RESOLVE: {
        /* the name in buf (not NUL-ended); the socket becomes the lookup (D19 4.5): the one the program opened is
         * given up first, the same (index, generation) names the lookup */
        static char name[256];
        u32 n = q->len < sizeof name - 1 ? q->len : sizeof name - 1;
        memcpy(name, (const void *)r->buf, n);
        name[n] = 0;
        if (!name[0] || strlen(name) != n) return -EINVAL;
        if ((v = netd_close(i, g)) < 0) return v;
        return netd_resolve(i, g, name, q->uid);
    }
    case SOCK_NETSTAT: {                        /* asked on any socket of the caller's: it must be one netd knows */
        long n;
        if ((v = netd_ioctl(i, g, NETD_FIONREAD, &n)) == -ENETDOWN) return v;
        return do_netstat(q);
    }
    case NET_CLOSE:
        return netd_close(i, g);
    }
    return -EOPNOTSUPP;
}

static void answer(struct umsg *m)
{
    long r;
    u32 b;
    if (!(m->kflags & UMSG_KERNEL)) {           /* only the kernel's requests (D19 9.1) */
        sys_netsrv(NETSRV_REPLY, (u32)m, (u32)-EACCES);
        return;
    }
    r = serve(m);
    if (m->type == SOCK_SOCKET) b = netd_bits(NAME_IDX(m->a[0]), NAME_GEN(m->a[0]));
    else {
        b = m->type == NET_CLOSE ? 0 : netd_bits(NAME_IDX(m->a[0]), NAME_GEN(m->a[0]));
        if (m->type == SOCK_ACCEPT && r >= 0) {
            struct netreq *q = (struct netreq *)m->a[1];
            b |= netd_bits(NAME_IDX(q->newsock), NAME_GEN(q->newsock)) << 16;
        }
    }
    m->a[2] = b;                                /* the truth with the reply (rule 1) */
    m->a[3] = epoch;
    sys_netsrv(NETSRV_REPLY, (u32)m, (u32)r);
}

static void push(void)
{
    static struct netd_ready rd[NETD_NSOCK];
    static struct unetready u[NETD_NSOCK];
    int n = netd_changed(rd, NETD_NSOCK);
    for (int k = 0; k < n; k++) {
        u[k].sock = (u32)rd[k].idx | (u32)rd[k].gen << 8;
        u[k].bits = rd[k].bits;
    }
    if (n) sys_netsrv(NETSRV_READY, (u32)u, (u32)n);
}

#define TURN_MSGS 8                             /* requests served a turn before lwIP's timers and the lines again */

int main(void)
{
    struct netd_limits lim;
    unsigned char s[48];
    signal(SIGPIPE, SIG_IGN);                   /* a dead driver's pipe must not end netd (D19 2.3) */
    port = sys_port_create("net");              /* the port first: the kernel's start deadline (D19 6.3) */
    if (port < 0) {
        printf("netd: port net: %s\n", zstrerror(port));
        return 1;
    }
    epoch = (u32)sys_netsrv(NETSRV_HELLO, 0, 0);
    int prof = profile();
    netd_limits_profile(&lim, prof);
    if (seed(s, "/dev/urandom")) {
        printf("netd: cannot read /dev/urandom\n");
        return 1;
    }
    if (netd_init(&lim, s)) {
        printf("netd: the limits are wrong\n");
        return 1;
    }
    memset(s, 0, sizeof s);
    last_seed = sys_ticks();
    rnd_fd = sys_open("/dev/random", O_RDONLY | O_NONBLOCK);
    load_config();
    printf("netd: serving net, epoch %u, %s profile (%u KB of buffers)\n", epoch,
           prof == NETD_PROFILE_LARGE ? "large" : "small", (u32)(lim.heap / 1024));
    for (;;) {
        unsigned long ms = netd_turn();
        push();
        struct pollfd pf[2 + NETD_IFMAX];
        int h[NETD_IFMAX], ww[NETD_IFMAX], nh = netd_wait_handles(h, ww, NETD_IFMAX), np = 0;
        pf[np].fd = port; pf[np].events = POLLPORT | POLLIN; pf[np].revents = 0; np++;
        for (int k = 0; k < nh; k++) {
            pf[np].fd = h[k];
            pf[np].events = (short)(POLLIN | (ww[k] ? POLLOUT : 0));
            pf[np].revents = 0;
            np++;
        }
        if (rnd_fd >= 0) { pf[np].fd = rnd_fd; pf[np].events = POLLIN; pf[np].revents = 0; np++; }
        u32 since = sys_ticks() - last_seed;
        if (since >= RESEED) { reseed("/dev/urandom"); since = 0; }
        if (ms > (RESEED - since) * 10) ms = (RESEED - since) * 10;
        sys_evwait(pf, (u32)np, (int)((ms + 9) / 10));
        if (rnd_fd >= 0 && (pf[np - 1].revents & POLLIN)) {    /* the pool is seeded: keys from it now */
            sys_close(rnd_fd);
            rnd_fd = -1;
            reseed("/dev/random");
        }
        for (int k = 0; k < TURN_MSGS; k++) {
            struct umsg *m = sys_recv(port, 0);
            if ((int)m < 0 && (int)m > -4096) return 0;         /* the port is gone: a kill between requests */
            if (!m) break;
            answer(m);
        }
    }
}
