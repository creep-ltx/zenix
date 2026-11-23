/* SPDX-License-Identifier: GPL-2.0-only */
/* BSD sockets (DESIGN D19 4.4, user/socket.h): each call fills a struct usockreq and makes one SYS_sock; the kernel
 * checks every pointer and waits (D19 4.3). Also fcntl and ioctl (TIOCGWINSZ, TIOCSWINSZ, TIOCSCTTY: DESIGN D27
 * 5.4), the addresses as text and the names (netd's lookups). One source for libz and the C library (user/zshare.h). */
#include "zshare.h"
#include <stdarg.h>
#include "../kernel/netproto.h"
#ifdef ZENIX_LIBC
#include <sys/socket.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#else
#include "socket.h"
#endif
static int sk(u32 op, int fd, struct usockreq *r)
{
    int v = sys_sock(op | (u32)sizeof *r << 16, fd, r);
    return v < 0 ? -1 : v;
}
#define SKREQ(r) struct usockreq r; memset(&r, 0, sizeof r)
int socket(int domain, int type, int protocol)
{
    SKREQ(r);
    if ((u32)domain > 0xFFFF || (u32)protocol > 0xFFFF) { zerrno = EINVAL; return -1; }
    r.level = (unsigned short)domain;
    r.flags = (u32)type;
    r.option = (unsigned short)protocol;
    return sk(SOCK_SOCKET, 0, &r);
}
static int sk_addr(u32 op, int fd, const struct sockaddr *a, socklen_t *len)
{
    SKREQ(r);
    r.addr = (u32)a;
    r.alen = (u32)len;
    return sk(op, fd, &r);
}
int bind(int fd, const struct sockaddr *a, socklen_t len) { return sk_addr(SOCK_BIND, fd, a, &len); }
int connect(int fd, const struct sockaddr *a, socklen_t len) { return sk_addr(SOCK_CONNECT, fd, a, &len); }
int getsockname(int fd, struct sockaddr *a, socklen_t *len) { return sk_addr(SOCK_GETSOCKNAME, fd, a, len); }
int getpeername(int fd, struct sockaddr *a, socklen_t *len) { return sk_addr(SOCK_GETPEERNAME, fd, a, len); }
int listen(int fd, int backlog)
{
    SKREQ(r);
    r.len = backlog < 0 ? 0 : (u32)backlog;
    return sk(SOCK_LISTEN, fd, &r);
}
int accept4(int fd, struct sockaddr *a, socklen_t *len, int flags)
{
    SKREQ(r);
    r.addr = (u32)a;
    r.alen = a ? (u32)len : 0;
    r.flags = (u32)flags;
    return sk(SOCK_ACCEPT, fd, &r);
}
int accept(int fd, struct sockaddr *a, socklen_t *len) { return accept4(fd, a, len, 0); }
__zx_ssize sendto(int fd, const void *buf, __zx_size n, int flags, const struct sockaddr *a, socklen_t len)
{
    SKREQ(r);
    r.buf = (u32)buf;
    r.len = n;
    r.flags = (u32)flags;
    if (a) { r.addr = (u32)a; r.alen = (u32)&len; }
    return sk(SOCK_SENDTO, fd, &r);
}
__zx_ssize send(int fd, const void *buf, __zx_size n, int flags) { return sendto(fd, buf, n, flags, 0, 0); }
__zx_ssize recvfrom(int fd, void *buf, __zx_size n, int flags, struct sockaddr *a, socklen_t *len)
{
    SKREQ(r);
    r.buf = (u32)buf;
    r.len = n;
    r.flags = (u32)flags;
    if (a) { r.addr = (u32)a; r.alen = (u32)len; }
    return sk(SOCK_RECVFROM, fd, &r);
}
__zx_ssize recv(int fd, void *buf, __zx_size n, int flags) { return recvfrom(fd, buf, n, flags, 0, 0); }
int shutdown(int fd, int how)
{
    SKREQ(r);
    r.flags = (u32)how;
    return sk(SOCK_SHUTDOWN, fd, &r);
}
static int sk_opt(u32 op, int fd, int level, int opt, const void *val, socklen_t *len)
{
    SKREQ(r);
    if ((u32)level > 0xFFFF || (u32)opt > 0xFFFF) { zerrno = ENOPROTOOPT; return -1; }
    r.level = (unsigned short)level;
    r.option = (unsigned short)opt;
    r.val = (u32)val;
    r.vlen = (u32)len;
    return sk(op, fd, &r);
}
int getsockopt(int fd, int level, int opt, void *val, socklen_t *len) { return sk_opt(SOCK_GETSOCKOPT, fd, level, opt, val, len); }
int setsockopt(int fd, int level, int opt, const void *val, socklen_t len) { return sk_opt(SOCK_SETSOCKOPT, fd, level, opt, val, &len); }
static int sk_ioctl(int fd, u32 req, void *p)
{
    SKREQ(r);
    r.option = (unsigned short)req;
    r.val = (u32)p;
    return sk(SOCK_IOCTL, fd, &r);
}
int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *p = va_arg(ap, void *);
    va_end(ap);
    /* a terminal's size and the controlling tty (DESIGN D27 5.4, D20 7.2): SYS_ttyctl on that descriptor (ENOTTY when
     * it is not a terminal) */
    if (req == TIOCGWINSZ || req == TIOCSWINSZ)
        return sys_ttyctl(fd, req == TIOCGWINSZ ? TTYC_GWINSZ : TTYC_SWINSZ, (u32)p) < 0 ? -1 : 0;
    if (req == TIOCSCTTY)
        return sys_ttyctl(fd, TTYC_SCTTY, (u32)p) < 0 ? -1 : 0;   /* the argument is an int, 0 (Linux) */
    if (req != FIONREAD && req != FIONBIO) { zerrno = ENOTTY; return -1; }
    if (!p) { zerrno = EFAULT; return -1; }
    return sk_ioctl(fd, (u32)req, p);
}
int fcntl(int fd, int cmd, ...)
{
    int arg = 0;
    if (cmd == F_SETFL || cmd == F_SETFD) { va_list ap; va_start(ap, cmd); arg = va_arg(ap, int); va_end(ap); }
    if (cmd == F_GETFD || cmd == F_SETFD) { struct ustat st; return sys_fstat(fd, &st) < 0 ? -1 : 0; }
    if (cmd != F_GETFL && cmd != F_SETFL) { zerrno = EINVAL; return -1; }
    int on = (arg & O_NONBLOCK) != 0;
    int v = sk_ioctl(fd, FIONBIO, cmd == F_GETFL ? 0 : &on);    /* FIONBIO with no value asks (netproto.h) */
    if (v >= 0) return cmd == F_GETFL ? O_RDWR | (v ? O_NONBLOCK : 0) : 0;
    if (zerrno != ENOTSOCK) return -1;
    if (cmd == F_GETFL) return O_RDWR;                          /* other files: no flag to report yet */
    if (on) { zerrno = EINVAL; return -1; }                     /* O_NONBLOCK on pipes and ttys: not yet (D19 4.1) */
    return 0;
}

/* addresses as text */
int inet_pton(int af, const char *s, void *dst)
{
    if (af != AF_INET) { zerrno = EAFNOSUPPORT; return -1; }
    unsigned char a[4];
    for (int i = 0; i < 4; i++) {
        u32 v = 0, nd = 0;
        while (*s >= '0' && *s <= '9') {
            if (nd && !v) return 0;                     /* no leading zeros, as glibc */
            v = v * 10 + (u32)(*s++ - '0');
            if (++nd > 3 || v > 255) return 0;
        }
        if (!nd || *s != (i < 3 ? '.' : 0)) return 0;
        a[i] = (unsigned char)v;
        s++;
    }
    memcpy(dst, a, 4);
    return 1;
}
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    const unsigned char *a = src;
    char t[16];
    if (af != AF_INET) { zerrno = EAFNOSUPPORT; return 0; }
    int n = snprintf(t, sizeof t, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    if ((u32)n >= size) { zerrno = 28; return 0; }      /* ENOSPC */
    memcpy(dst, t, (u32)n + 1);
    return dst;
}
int inet_aton(const char *s, struct in_addr *out)
{
    u32 part[4], n = 0;
    for (;;) {
        if (!isdigit((unsigned char)*s)) return 0;
        char *e;
        unsigned long v = strtoul(s, &e, 0);
        if (e == s || n == 4) return 0;
        part[n++] = (u32)v;
        s = e;
        if (*s == '.') { s++; continue; }
        if (*s && !isspace((unsigned char)*s)) return 0;
        break;
    }
    u32 v = part[n - 1], max = n == 1 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFFFu : n == 3 ? 0xFFFFu : 0xFFu;
    if (v > max) return 0;
    for (u32 i = 0; i + 1 < n; i++) {
        if (part[i] > 255) return 0;
        v |= part[i] << (24 - 8 * i);
    }
    if (out) out->s_addr = htonl(v);
    return 1;
}
in_addr_t inet_addr(const char *s)
{
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}
char *inet_ntoa(struct in_addr a)
{
    static char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, b, sizeof b);
    return b;
}

/* names (DESIGN D19 4.5) */
static const struct { const char *name; unsigned short port; } services[] = {
    { "echo", 7 }, { "ftp", 21 }, { "ssh", 22 }, { "telnet", 23 }, { "smtp", 25 }, { "domain", 53 }, { "http", 80 },
    { "pop3", 110 }, { "nntp", 119 }, { "ntp", 123 }, { "imap", 143 }, { "https", 443 },
};
/* a name looked up by netd: up to 4 addresses into a[], their count, or an EAI_* error */
static int resolve(const char *name, u32 *a)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return zerrno == ENETDOWN ? EAI_FAIL : EAI_SYSTEM;
    SKREQ(r);
    r.buf = (u32)name;
    r.len = strlen(name);
    int n = sk(SOCK_RESOLVE, s, &r);
    if (n >= 0) n = recv(s, a, 16, 0);
    int e = zerrno;
    sys_close(s);
    if (n > 0) return n / 4;
    zerrno = e;
    if (n == 0 || e == ENOENT) return EAI_NONAME;
    if (e == ETIMEDOUT || e == EAGAIN) return EAI_AGAIN;
    if (e == ENOMEM || e == ENOBUFS) return EAI_MEMORY;
    return e == EINTR ? EAI_SYSTEM : EAI_FAIL;
}
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    int flags = hints ? hints->ai_flags : 0, fam = hints ? hints->ai_family : AF_UNSPEC;
    int type = hints ? hints->ai_socktype : 0, proto = hints ? hints->ai_protocol : 0;
    u32 addr[4];
    int na = 1;
    *res = 0;
    if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV)) return EAI_BADFLAGS;
    if (fam != AF_UNSPEC && fam != AF_INET) return EAI_FAMILY;
    if (type && type != SOCK_STREAM && type != SOCK_DGRAM) return EAI_SOCKTYPE;
    if (!node && !service) return EAI_NONAME;
    u32 port = 0;
    if (service && *service) {
        char *e;
        port = strtoul(service, &e, 10);
        if (*e || !isdigit((unsigned char)*service)) {
            if (flags & AI_NUMERICSERV) return EAI_NONAME;
            u32 i = 0;
            while (i < sizeof services / sizeof services[0] && strcmp(services[i].name, service)) i++;
            if (i == sizeof services / sizeof services[0]) return EAI_SERVICE;
            port = services[i].port;
        } else if (port > 65535) return EAI_SERVICE;
    }
    struct in_addr in;
    if (!node) addr[0] = htonl(flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK);
    else if (inet_aton(node, &in)) addr[0] = in.s_addr;
    else if (flags & AI_NUMERICHOST) return EAI_NONAME;
    else if ((na = resolve(node, addr)) < 0) return na;
    struct addrinfo **tail = res;
    for (int i = 0; i < na; i++)
        for (int t = SOCK_STREAM; t <= SOCK_DGRAM; t++) {
            if (type && type != t) continue;
            struct addrinfo *ai = malloc(sizeof *ai + sizeof(struct sockaddr_in));
            if (!ai) { freeaddrinfo(*res); *res = 0; return EAI_MEMORY; }
            struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
            memset(ai, 0, sizeof *ai + sizeof *sin);
            sin->sin_family = AF_INET;
            sin->sin_port = htons((unsigned short)port);
            sin->sin_addr.s_addr = addr[i];
            ai->ai_family = AF_INET;
            ai->ai_socktype = t;
            ai->ai_protocol = proto ? proto : t == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
            ai->ai_addrlen = sizeof *sin;
            ai->ai_addr = (struct sockaddr *)sin;
            if ((flags & AI_CANONNAME) && !*res && node && !(ai->ai_canonname = strdup(node))) {
                free(ai);
                return EAI_MEMORY;
            }
            *tail = ai;
            tail = &ai->ai_next;
        }
    return 0;
}
void freeaddrinfo(struct addrinfo *ai)
{
    while (ai) {
        struct addrinfo *n = ai->ai_next;
        free(ai->ai_canonname);
        free(ai);
        ai = n;
    }
}
const char *gai_strerror(int e)
{
    switch (e) {
    case 0: return "success";
    case EAI_BADFLAGS: return "bad value for ai_flags";
    case EAI_NONAME: return "name or service not known";
    case EAI_AGAIN: return "temporary failure in name resolution";
    case EAI_FAIL: return "non-recoverable failure in name resolution";
    case EAI_FAMILY: return "address family not supported";
    case EAI_SOCKTYPE: return "socket type not supported";
    case EAI_SERVICE: return "service not supported for this socket type";
    case EAI_MEMORY: return "out of memory";
    case EAI_SYSTEM: return zwhy("system error");
    }
    return "unknown error";
}

/* the one connection helper (socket.h; docs/audit-2026-11-23-one-system.md OS-38) */
static const char *zc_text(int e)
{
    const char *t = zstrerror(e);
    return t ? t : "cannot connect";
}
int zconnect_begin(const char *host, int port, int flags, const char **why)
{
    struct addrinfo h, *r = 0;
    char ps[8];
    int e, s, dg = flags & ZC_UDP;
    static const char *dummy;
    if (!why) why = &dummy;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_socktype = dg ? SOCK_DGRAM : SOCK_STREAM;
    if (port <= 0 || port > 65535) { *why = "not a port number"; return -EINVAL; }
    snprintf(ps, sizeof ps, "%d", port);
    if ((e = getaddrinfo(host, ps, &h, &r)) != 0) {
        *why = e == EAI_NONAME ? "no such host" : gai_strerror(e);
        return e == EAI_MEMORY ? -ENOMEM : -ENOENT;
    }
    s = socket(AF_INET, dg ? SOCK_DGRAM : SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s < 0) {
        e = zerrno;
        freeaddrinfo(r);
        *why = zc_text(e);
        return -e;
    }
    e = connect(s, r->ai_addr, r->ai_addrlen) < 0 ? zerrno : 0;
    freeaddrinfo(r);
    if (e && e != EINPROGRESS) {
        sys_close(s);
        *why = zc_text(e);
        return -e;
    }
    return s;
}
int zconnect_end(int fd, int ms, const char **why)
{
    struct pollfd q;
    int e = 0, w;
    socklen_t el = sizeof e;
    static const char *dummy;
    if (!why) why = &dummy;
    q.fd = fd;
    q.events = POLLOUT;
    q.revents = 0;
    w = poll(&q, 1, ms);
    if (w == 0)
        return 0;
    if (w < 0)
        e = zerrno;
    else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) < 0)
        e = zerrno;
    if (!e)
        return 1;
    sys_close(fd);
    *why = zc_text(e);
    return -e;
}
int zconnect(const char *host, int port, int flags, const char **why)
{
    int s = zconnect_begin(host, port, flags, why), r;
    if (s < 0 || (flags & ZC_UDP))
        return s;
    if ((r = zconnect_end(s, ZCONNECT_MS, why)) <= 0) {
        if (r == 0) {
            sys_close(s);
            if (why) *why = zc_text(ETIMEDOUT);
            return -ETIMEDOUT;
        }
        return r;
    }
    if (!(flags & ZC_NONBLOCK)) {
        int off = 0;
        ioctl(s, FIONBIO, &off);
    }
    return s;
}
