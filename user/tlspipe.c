/* SPDX-License-Identifier: GPL-2.0-only */
/* /lib/tls/tlspipe HOST PORT: TLS for a program that does not carry the crypto library itself (DESIGN D58 section 5,
 * D59 section 4; irc and mail start it for a TLS server, mail also after STARTTLS, so they stay small and the library
 * is loaded only when TLS is used; the package tls). The socket,
 * already connected to HOST, is descriptor 2; the plain text comes in on 0 and goes out on 1. The handshake is the
 * crypto library's (crypto/zc/zc_tls.h, as wget's: TLS 1.2, the chain checked against /etc/ssl/anchors and the
 * owner's roots in /etc/ssl/local/, the name HOST, the dates by zc_clock's rules: user/tlstrust.h). The first line on
 * 1 says how it went: "ok\n", "ok clock YYYY-MM-DD\n" (the clock is not trusted: dates checked only against that
 * floor) or "error TEXT\n" (then the end, status 3), after "info TEXT\n" lines while it waits (the random pool); after "ok" the stream both ways until either side ends it: an
 * end of 0 sends close_notify (status 0); the server's end: status 0 with its close_notify, 1 without or on a
 * network error, 3 on a TLS error. Not a command: no page; docs/man/irc.txt and mail.txt name it. */
#include "socket.h"
#include "tlstrust.h"
#include "builddate.h"

#define IDLE_MS 240000                  /* a record that has begun must end within this (the handshake's arithmetic is
                                         * ours, not the server's silence) */

static struct {
    struct zc_tls t;
    unsigned char iobuf[ZC_TLS_BUFSIZE], chain[ZC_X509_CHAINMAX], store[16384];
} *T;
static int sock = 2;

static void put(const char *s)
{
    sys_write(1, s, (u32)strlen(s));
}

/* tlstrust's words before the handshake ends (the random pool): "info TEXT" lines, which irc shows */
static void info(const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b + 5, sizeof b - 6, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    memcpy(b, "info ", 5);
    n = (int)strlen(b);
    if (n && b[n - 1] != '\n') b[n++] = '\n';
    sys_write(1, b, (u32)n);
}

static int wait_fd(int fd, int ev, int ms)
{
    struct pollfd q;
    q.fd = fd;
    q.events = (short)ev;
    q.revents = 0;
    int r = poll(&q, 1, ms);
    return r < 0 ? -zerrno : r;
}

static long sock_send(void *u, const unsigned char *p, size_t n)
{
    (void)u;
    for (;;) {
        int k = send(sock, p, (u32)n, MSG_NOSIGNAL);
        if (k > 0)
            return k;
        if (k < 0 && zerrno == EAGAIN) {
            if (wait_fd(sock, POLLOUT, IDLE_MS) != 1)
                return -ETIMEDOUT;
            continue;
        }
        return k < 0 ? -zerrno : -EPIPE;
    }
}

static long sock_recv(void *u, unsigned char *p, size_t n)
{
    (void)u;
    for (;;) {
        int k = recv(sock, p, (u32)n, 0);
        if (k >= 0)
            return k;
        if (zerrno != EAGAIN)
            return -zerrno;
        if (wait_fd(sock, POLLIN, IDLE_MS) != 1)
            return -ETIMEDOUT;
    }
}

static int writeall(int fd, const unsigned char *b, long n)
{
    while (n > 0) {
        int w = sys_write(fd, b, (u32)n);
        if (w <= 0)
            return -1;
        b += w;
        n -= w;
    }
    return 0;
}

/* the server's decrypted bytes to 1 while the engine holds some: 0, 1 the server closed, < 0 an error */
static int drain(void)
{
    static unsigned char b[1024];
    do {
        long k = zc_tls_read(&T->t, b, sizeof b);
        if (k == 0)
            return 1;
        if (k < 0)
            return (int)k;
        if (writeall(1, b, k) < 0)
            return -EPIPE;
    } while (br_ssl_engine_current_state(&T->t.cc.eng) & BR_SSL_RECVAPP);
    return 0;
}

static int status(int r)
{
    if (r == ZC_TLS_EENGINE || r == ZC_TLS_ESTATE)
        return 3;
    return r < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    static struct zc_clock clk;
    unsigned char seed[32];
    char line[160];
    int r, port;
    if (argc != 3 || (port = atoi(argv[2])) <= 0 || port > 65535) {
        put("error usage: tlspipe HOST PORT (the socket on descriptor 2)\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    if (!(T = calloc(1, sizeof *T))) {
        put("error out of memory for TLS\n");
        return 3;
    }
    tt_say = info;
    clk.trusted = tt_clock_trusted();
    clk.floor = tt_clock_floor(ZENIX_BUILD_DATE);
    clk.now = sys_time();
    tt_anchors_open();
    if ((r = tt_seed32(seed)) < 0) {
        snprintf(line, sizeof line, "error no random numbers: %s\n", zstrerror(r) ? zstrerror(r) : "failed");
        put(line);
        return 3;
    }
    int pr = getpriority(PRIO_PROCESS, 0);      /* as wget: the handshake's arithmetic one level lower */
    if (pr < 19)
        setpriority(PRIO_PROCESS, 0, pr + 1);
    struct zc_tls_io io = { 0, sock_send, sock_recv };
    zc_tls_init(&T->t, T->iobuf, T->chain, T->store, sizeof T->store, tt_find, 0, 0);
    r = zc_tls_connect(&T->t, &io, argv[1], (unsigned)port, &clk, seed, 0);
    zc_wipe(seed, sizeof seed);
    if (pr < 19)
        setpriority(PRIO_PROCESS, 0, pr);
    if (r < 0) {
        int e = r == ZC_TLS_EENGINE ? zc_tls_error(&T->t) : r;
        snprintf(line, sizeof line, "error TLS: %s\n", r == ZC_TLS_EENGINE || r == ZC_TLS_ETRUNC ? zc_tls_strerror(e)
                 : r == -ETIMEDOUT ? "the server stopped answering" : zstrerror(r) ? zstrerror(r) : "failed");
        put(line);
        return 3;
    }
    if (T->t.clock.used_floor) {
        int y, m, d;
        tt_civil(clk.floor, &y, &m, &d);
        snprintf(line, sizeof line, "ok clock %d-%02d-%02d\n", y, m, d);
        put(line);
    } else
        put("ok\n");
    static unsigned char b[1024];
    for (;;) {
        struct pollfd p[2];
        p[0].fd = 0; p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = sock; p[1].events = POLLIN; p[1].revents = 0;
        if (poll(p, 2, -1) < 0)
            continue;
        if (p[1].revents) {
            if ((r = drain()) != 0)
                break;
        }
        if (p[0].revents) {
            int k = sys_read(0, b, sizeof b);
            if (k == -EAGAIN)           /* irc's end of the pipe is non-blocking, and so is ours */
                continue;
            if (k <= 0) {               /* the program is done: close_notify */
                zc_tls_close(&T->t);
                r = 0;
                break;
            }
            for (;;) {
                r = zc_tls_write(&T->t, b, (size_t)k);
                /* the server's data came in while ours waited (one buffer for both ways): out first, then again */
                if (r == ZC_TLS_ESTATE && (br_ssl_engine_current_state(&T->t.cc.eng) & BR_SSL_RECVAPP)) {
                    if ((r = drain()) != 0)
                        break;
                    continue;
                }
                break;
            }
            if (r)
                break;
        }
    }
    r = r == 1 ? 0 : r == ZC_TLS_ETRUNC ? 1 : status(r);
    zc_wipe(T, sizeof *T);
    return r;
}
