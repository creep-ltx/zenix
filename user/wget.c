/* SPDX-License-Identifier: GPL-2.0-only */
/* wget [-q] [-c] [-O FILE] URL...: fetch files over HTTP and HTTPS (DESIGN D21 8, step 7; docs/man/wget.txt). The
 * command line, the names, the redirects and the resume rule are wget's core (user/wget/), the protocol net/http/,
 * TLS the crypto library's glue (crypto/zc/zc_tls.h: TLS 1.2, the server's chain checked against /etc/ssl/anchors
 * and the owner's roots in /etc/ssl/local/, the dates by zc_clock's rules); this file gives them the sockets (with
 * time limits), the files, the random seed and the messages. One connection is kept open while the next URL goes to
 * the same server (keep-alive), and TLS sessions are remembered for the run (resumption). Exit statuses: GNU wget's
 * (wget.h). Not a pure program: the X.509 code's tables are written at run time (D21 3.5). */
#include "socket.h"
#include "wget/wget.h"
#include "../net/http/http.h"
#include "../crypto/zc/zc_tls.h"
#include "builddate.h"

#define CONNECT_MS 60000                /* a connection that is not made in a minute fails */
#define IDLE_MS    60000                /* nor may the server be silent longer (the handshake's arithmetic is ours) */
#define LOCALMAX   16                   /* the owner's roots taken from /etc/ssl/local/ */
#define PEMMAX     65536                /* a PEM file read there */
#define DERMAX     8192                 /* one certificate in it */

static int quiet;

static void say(const char *fmt, ...)
{
    va_list ap;
    if (quiet)
        return;
    va_start(ap, fmt);
    fprintf(2, "wget: ");
    vfprintf(2, fmt, ap);
    va_end(ap);
}

/* ---- the clock (D21 8.3) ---- */
/* Is the clock trusted this boot (the battery clock read at boot, or sntp's guarded answer: SYS_ticks 2)? If not, the
 * certificates' "not after" is judged against the floor and "not before" skipped (zc_clock.h). */
static int clock_trusted(void)
{
    int b = sys_clockbits();
    return b > 0 && (b & (CLOCK_RTC | CLOCK_NET));
}

/* the floor: the newer of the build date (the commit's, builddate.h) and /var/lib/clock */
static u32 clock_floor(void)
{
    char b[ZC_CLOCK_LINE];
    uint32_t t = 0;
    int fd = sys_open(ZC_CLOCK_PATH, O_RDONLY), n = 0, ok = 0;
    if (fd >= 0) {
        n = sys_read(fd, b, sizeof b);
        sys_close(fd);
        ok = n > 0 && zc_clock_parse(b, (size_t)n, &t) == 0;
    }
    return zc_clock_floor(ZENIX_BUILD_DATE, ok, t);
}

static void civil(u32 t, int *y, int *m, int *d)
{
    static const unsigned char ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    u32 days = t / 86400;
    *y = 1970;
    for (;;) {
        u32 yl = (*y % 4 == 0 && (*y % 100 != 0 || *y % 400 == 0)) ? 366 : 365;
        if (days < yl)
            break;
        days -= yl;
        ++*y;
    }
    for (*m = 0; *m < 11; ++*m) {
        u32 l = ml[*m] + (*m == 1 && *y % 4 == 0 && (*y % 100 != 0 || *y % 400 == 0));
        if (days < l)
            break;
        days -= l;
    }
    ++*m;
    *d = (int)days + 1;
}

/* ---- the random seed: 32 bytes from /dev/random for each handshake (D21 5); said when the pool is not ready ---- */
static int seed32(unsigned char *b)
{
    int fd = sys_open("/dev/random", O_RDONLY | O_NONBLOCK), r;
    if (fd < 0)
        return fd;
    r = sys_read(fd, b, 32);
    if (r == -EAGAIN) {
        say("waiting for the random pool (type on the keyboard)\n");
        sys_close(fd);
        if ((fd = sys_open("/dev/random", O_RDONLY)) < 0)
            return fd;
        r = sys_read(fd, b, 32);
    }
    sys_close(fd);
    return r == 32 ? 0 : r < 0 ? r : -EIO;
}

/* ---- the trust anchors: /etc/ssl/anchors (D21 8.1) and the owner's PEM roots in /etc/ssl/local/ ---- */
static int anchors_fd = -2;             /* -2: not opened yet */
static struct { unsigned char *der; size_t n; unsigned char hash[8]; } local[LOCALMAX];
static int nlocal;

static int anchors_rd(void *u, unsigned long off, void *buf, unsigned long n)
{
    unsigned char *p = buf;
    (void)u;
    if (anchors_fd < 0 || sys_lseek(anchors_fd, (int)off, 0) != (int)off)
        return -1;
    while (n) {
        int r = sys_read(anchors_fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (unsigned long)r;
    }
    return 0;
}

static void dn_hash(void *ctx, const void *buf, size_t len)
{
    br_sha256_update(ctx, buf, len);
}

/* a local root's subject name hash (as the anchors file's index: the first 8 bytes of SHA-256 of the DER name) */
static int subject_hash(const unsigned char *der, size_t n, unsigned char out[8])
{
    static br_x509_decoder_context d;
    br_sha256_context c;
    unsigned char h[32];
    br_sha256_init(&c);
    br_x509_decoder_init(&d, dn_hash, &c);
    br_x509_decoder_push(&d, der, n);
    if (br_x509_decoder_last_error(&d) || !br_x509_decoder_get_pkey(&d))
        return -1;
    br_sha256_out(&c, h);
    memcpy(out, h, 8);
    return 0;
}

static void load_local(void)
{
    char **names, *pem = 0;
    int n = dir_list("/etc/ssl/local", &names), i;
    for (i = 0; i < n; i++) {
        char *p = path_join("/etc/ssl/local", names[i]);
        int fd = p ? sys_open(p, O_RDONLY) : -1, len = 0, r;
        struct ustat st;
        size_t off = 0;
        free(p);
        if (fd < 0)
            continue;
        if (sys_fstat(fd, &st) == 0 && (st.mode & S_IFMT) == S_IFREG) {
            if (st.size > PEMMAX)
                say("/etc/ssl/local/%s: over %d bytes, not read\n", names[i], PEMMAX);
            else if (pem || (pem = malloc(PEMMAX + 1)))
                while (len < (int)st.size && (r = sys_read(fd, pem + len, st.size - (u32)len)) > 0)
                    len += r;
        }
        sys_close(fd);
        while (len > 0) {
            unsigned char *der;
            long k;
            if (nlocal == LOCALMAX) {
                if (strstr(pem + off, "-----BEGIN CERTIFICATE"))
                    say("/etc/ssl/local: more than %d roots, the rest not used\n", LOCALMAX);
                break;
            }
            pem[len] = 0;
            if (!(der = malloc(DERMAX)))
                break;
            k = zc_pem_cert(pem, (size_t)len, &off, der, DERMAX);
            if (k > 0 && subject_hash(der, (size_t)k, local[nlocal].hash) == 0) {
                local[nlocal].der = der;
                local[nlocal++].n = (size_t)k;
                continue;
            }
            free(der);
            if (k == 0)
                break;
            say("/etc/ssl/local/%s: a damaged certificate, not used\n", names[i]);
            if (k < 0)
                break;
        }
    }
    free(pem);
    if (n >= 0)
        dir_free(names, n);
}

static int find(void *u, struct zc_x509 *x, const unsigned char hash[8], const unsigned char *dn, size_t dnlen)
{
    int i, r;
    (void)u;
    (void)dn;
    (void)dnlen;
    if (anchors_fd >= 0 && (r = zc_anchors_find(x, anchors_rd, 0, hash)))
        return r;
    for (i = 0; i < nlocal; i++)
        if (!memcmp(local[i].hash, hash, 8) && (r = zc_x509_add_cert(x, local[i].der, local[i].n)))
            return r;
    return 0;
}

/* ---- the connection ---- */
struct tlsmem {                         /* only when an https: URL is fetched (malloc'd: plain HTTP does not pay) */
    struct zc_tls t;
    struct zc_tls_cache cache;
    unsigned char iobuf[ZC_TLS_BUFSIZE], chain[ZC_X509_CHAINMAX], store[16384];
};
static struct tlsmem *T;
static struct zc_clock clk;
static int said_clock;

struct conn {
    int fd, tls, eof;
    struct http_url at;                 /* the scheme, host and port it is open to */
    unsigned char buf[4096];
    size_t have, off;
};
static struct conn C;

/* wait for fd to be ready for ev, at most ms: 1 ready, 0 the time is up, < 0 an error */
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
    struct conn *c = u;
    for (;;) {
        int k = send(c->fd, p, (u32)n, MSG_NOSIGNAL);
        if (k > 0)
            return k;
        if (k < 0 && zerrno == EAGAIN) {
            if (wait_fd(c->fd, POLLOUT, IDLE_MS) != 1)
                return -ETIMEDOUT;
            continue;
        }
        return k < 0 ? -zerrno : -EPIPE;
    }
}

static long sock_recv(void *u, unsigned char *p, size_t n)
{
    struct conn *c = u;
    for (;;) {
        int k = recv(c->fd, p, (u32)n, 0);
        if (k >= 0)
            return k;
        if (zerrno != EAGAIN)
            return -zerrno;
        if (wait_fd(c->fd, POLLIN, IDLE_MS) != 1)
            return -ETIMEDOUT;
    }
}

static void conn_close(struct conn *c)
{
    if (c->fd < 0)
        return;
    if (c->tls)
        zc_tls_close(&T->t);
    sys_close(c->fd);
    c->fd = -1;
    c->have = c->off = 0;
}

static const char *errtext(int e)
{
    const char *t = zstrerror(e);
    return t ? t : "failed";
}

/* a TCP connection to u's host and port with the time limit: the socket, or -1 and *why */
static int tcp_open(const struct http_url *u, const char **why)
{
    static char msg[80];
    struct addrinfo h, *r = 0;
    char ps[8];
    int e, s;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%u", u->port);
    if ((e = getaddrinfo(u->host, ps, &h, &r)) != 0) {
        *why = e == EAI_NONAME ? "no such host" : gai_strerror(e);
        return -1;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)r->ai_addr)->sin_addr, ip, sizeof ip);
    say("connecting to %s (%s) port %u\n", u->host, ip, u->port);
    s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s < 0) {
        freeaddrinfo(r);
        snprintf(msg, sizeof msg, "no socket: %s", errtext(zerrno));
        *why = msg;
        return -1;
    }
    e = connect(s, r->ai_addr, r->ai_addrlen) < 0 ? zerrno : 0;
    freeaddrinfo(r);
    if (e == EINPROGRESS) {
        int w = wait_fd(s, POLLOUT, CONNECT_MS);
        socklen_t el = sizeof e;
        if (w != 1)
            e = w == 0 ? ETIMEDOUT : -w;
        else if (getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) < 0)
            e = zerrno;
    }
    if (e) {
        sys_close(s);
        snprintf(msg, sizeof msg, "connect: %s", errtext(e));
        *why = msg;
        return -1;
    }
    return s;
}

/* the connection for u (the kept one when it goes to the same server): 0, or an exit status with *why */
static int conn_open(struct conn *c, const struct http_url *u, const char **why)
{
    static char msg[160];
    int r;
    if (c->fd >= 0 && wget_same_server(&c->at, u))
        return 0;
    conn_close(c);
    if ((c->fd = tcp_open(u, why)) < 0)
        return WGET_EXIT_NET;
    c->at = *u;
    c->tls = u->tls;
    c->have = c->off = 0;
    c->eof = 0;
    if (!u->tls)
        return 0;
    if (!T && !(T = calloc(1, sizeof *T))) {
        conn_close(c);
        *why = "out of memory for TLS";
        return WGET_EXIT_GENERIC;
    }
    if (anchors_fd == -2) {             /* the first https: URL of the run */
        anchors_fd = sys_open("/etc/ssl/anchors", O_RDONLY);
        load_local();
        if (anchors_fd < 0 && !nlocal)
            say("no /etc/ssl/anchors and no roots in /etc/ssl/local: no certificate can be checked\n");
    }
    unsigned char s[32];
    if ((r = seed32(s)) < 0) {
        sys_close(c->fd);
        c->fd = -1;
        snprintf(msg, sizeof msg, "no random numbers: %s", errtext(r));
        *why = msg;
        return WGET_EXIT_GENERIC;
    }
    struct zc_tls_io io = { c, sock_send, sock_recv };
    zc_tls_init(&T->t, T->iobuf, T->chain, T->store, sizeof T->store, find, 0, &T->cache);
    clk.now = sys_time();
    say("TLS handshake with %s (on a 68020 this takes up to a minute)\n", u->host);
    r = zc_tls_connect(&T->t, &io, u->host, u->port, &clk, s, "http/1.1");
    zc_wipe(s, sizeof s);
    if (T->t.clock.used_floor && !said_clock) {
        int y, m, d;
        civil(clk.floor, &y, &m, &d);
        say("the clock is not set: certificate dates checked only against %d-%02d-%02d\n", y, m, d);
        said_clock = 1;
    }
    if (r < 0) {
        int e = r == ZC_TLS_EENGINE ? zc_tls_error(&T->t) : r;
        int x509 = r == ZC_TLS_EENGINE && ((e >= BR_ERR_X509_OK && e < BR_ERR_RECV_FATAL_ALERT) ||
                                           e == ZC_X509_ERR_EKU || e == ZC_X509_ERR_CHAIN);
        snprintf(msg, sizeof msg, "TLS: %s", r == ZC_TLS_EENGINE || r == ZC_TLS_ETRUNC ? zc_tls_strerror(e)
                                             : r == -ETIMEDOUT ? "the server stopped answering" : errtext(r));
        *why = msg;
        sys_close(c->fd);               /* no close_notify: the handshake never finished */
        c->fd = -1;
        return x509 ? WGET_EXIT_TLS : r == ZC_TLS_EENGINE ? WGET_EXIT_PROTO : WGET_EXIT_NET;
    }
    return 0;
}

/* more bytes into c->buf: > 0, 0 the end (the server closed; *trunc: TLS ended without close_notify), < 0 an error */
static long conn_fill(struct conn *c, int *trunc)
{
    long k;
    *trunc = 0;
    if (c->tls) {
        k = zc_tls_read(&T->t, c->buf, sizeof c->buf);
        if (k == ZC_TLS_ETRUNC) {
            *trunc = 1;
            return 0;
        }
    } else
        k = sock_recv(c, c->buf, sizeof c->buf);
    if (k > 0) {
        c->off = 0;
        c->have = (size_t)k;
    }
    return k;
}

static int conn_send(struct conn *c, const char *s, size_t n)
{
    if (c->tls)
        return zc_tls_write(&T->t, s, n);
    while (n) {
        long k = sock_send(c, (const unsigned char *)s, n);
        if (k < 0)
            return (int)k;
        s += k;
        n -= (size_t)k;
    }
    return 0;
}

/* ---- one URL ---- */
static int writeall(int fd, const unsigned char *b, size_t n)
{
    while (n) {
        int w = sys_write(fd, b, (u32)n);
        if (w <= 0)
            return w < 0 ? w : -EIO;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static int fail(int i, const struct wget_opts *o, int ex, const char *why)
{
    say("%s: %s\n", o->url[i], why);
    return ex;
}

static int fetch(const struct wget_opts *o, int i)
{
    static struct http_resp r;
    static char req[HTTP_PATHMAX + HTTP_HOSTMAX + 128];
    static char base[WGET_NAMEMAX + 1];
    struct wget_plan p;
    const char *why = "";
    int rc, act, ex, tries, need, out = -1, own = 0;
    unsigned long wrote = 0;
    u32 t0 = sys_uptime();
    if ((rc = wget_plan_start(&p, o, i, &why)))
        return fail(i, o, rc, why);
    if (p.need_size) {
        struct ustat st;
        int ok = sys_stat(p.name, &st) == 0;
        wget_plan_have(&p, ok, ok ? st.size : 0);
    }
    if (p.need_free_name) {
        struct ustat st;
        unsigned n = 1;
        strcpy(base, p.name);
        while (sys_stat(p.name, &st) == 0)
            if (wget_nextname(p.name, sizeof p.name, base, n++))
                return fail(i, o, WGET_EXIT_FILE, "no free file name");
    }
    for (;;) {
        for (tries = 0; tries < 2; tries++) {   /* a kept connection the server closed meanwhile: once again */
            int reused = C.fd >= 0 && wget_same_server(&C.at, &p.url), tr;
            if ((rc = conn_open(&C, &p.url, &why)))
                return fail(i, o, rc, why);
            rc = http_request(req, sizeof req, &p.url, p.from, 0);
            if (rc < 0)
                return fail(i, o, WGET_EXIT_GENERIC, "the request does not fit");
            if (conn_send(&C, req, (size_t)rc) < 0) {
                conn_close(&C);
                if (reused)
                    continue;
                return fail(i, o, WGET_EXIT_NET, "the request could not be sent");
            }
            http_resp_init(&r, 0xFFFFFFFFul, 0);
            if (C.off == C.have) {
                long k = conn_fill(&C, &tr);
                if (k <= 0) {
                    conn_close(&C);
                    if (reused && k == 0)
                        continue;
                    return fail(i, o, WGET_EXIT_NET, k == -ETIMEDOUT ? "the server stopped answering" : "no answer");
                }
            }
            break;
        }
        if (tries == 2)
            return fail(i, o, WGET_EXIT_NET, "the connection keeps closing");
        act = 0;
        need = 0;
        for (;;) {                      /* the headers, then the body */
            const unsigned char *body;
            size_t used, blen;
            int tr;
            if (need) {                 /* the parser took everything and wants more */
                long k = conn_fill(&C, &tr);
                need = 0;
                if (k < 0) {
                    conn_close(&C);
                    if (own)
                        sys_close(out);
                    return fail(i, o, WGET_EXIT_NET, k == -ETIMEDOUT ? "the server stopped answering"
                                : C.tls ? "the TLS connection failed" : "read error");
                }
                if (k == 0) {
                    rc = tr ? HTTP_EIO : http_resp_eof(&r);
                    conn_close(&C);
                    if (rc == HTTP_END)
                        break;
                    if (own)
                        sys_close(out);
                    say("%s: %s after %lu bytes\n", o->url[i], tr ? "truncated (no TLS close_notify)"
                        : "the connection ended early", wrote);
                    return WGET_EXIT_NET;
                }
            }
            rc = http_resp_feed(&r, C.buf + C.off, C.have - C.off, &used, &body, &blen);
            C.off += used;
            if (rc == HTTP_NEED) {
                need = 1;
                continue;
            }
            if (rc < 0) {
                conn_close(&C);
                if (own)
                    sys_close(out);
                return fail(i, o, WGET_EXIT_PROTO, rc == HTTP_E2BIG ? "the answer is too long" :
                            rc == HTTP_ENOTSUP ? "a transfer coding wget does not take" : "not an HTTP answer it takes");
            }
            if (rc == HTTP_HEAD) {
                act = wget_plan_answer(&p, &r, &ex, &why);
                if (act == WGET_FAIL) {
                    conn_close(&C);
                    say("%s: %d: %s\n", o->url[i], r.status, why);
                    return ex;
                }
                if (act == WGET_FOLLOW)
                    say("%d: following to %s://%s:%u%s\n", r.status, p.url.tls ? "https" : "http", p.url.host,
                        p.url.port, p.url.path);
                else if (act == WGET_DONE)
                    say("%s: the file is already complete\n", p.name);
                else if (act == WGET_WRITE) {
                    if (p.to_stdout)
                        out = 1;
                    else if ((out = sys_open(p.name, O_WRONLY | O_CREAT | (p.append ? O_APPEND : O_TRUNC))) >= 0)
                        own = 1;
                    else {
                        int e = out;
                        conn_close(&C);
                        say("%s: cannot write: %s\n", p.name, errtext(e));
                        return WGET_EXIT_FILE;
                    }
                    if (p.from)
                        say("%d: resuming '%s' at %lu of %lu bytes\n", r.status, p.name, p.from, p.total);
                    else if (!p.to_stdout)
                        say("%d: %s%lu bytes, saving to '%s'\n", r.status, p.total ? "" : "length unknown, ",
                            p.total, p.name);
                }
                continue;
            }
            if (rc == HTTP_DATA) {
                if (act == WGET_WRITE) {
                    int e = writeall(out, body, blen);
                    if (e < 0) {
                        conn_close(&C);
                        if (own)
                            sys_close(out);
                        say("%s: write error: %s\n", p.to_stdout ? "standard output" : p.name, errtext(e));
                        return WGET_EXIT_FILE;
                    }
                    wrote += blen;
                }
                continue;
            }
            if (r.close)                /* HTTP_END */
                conn_close(&C);
            break;
        }
        if (act == WGET_FOLLOW)
            continue;
        if (own)
            sys_close(out);
        if (act == WGET_WRITE && !p.to_stdout)
            say("'%s' saved: %lu bytes in %u s\n", p.name, wrote, sys_uptime() - t0);
        return 0;
    }
}

int main(int argc, char **argv)
{
    struct wget_opts o;
    const char *why = "";
    int i, rc, ex = 0;
    if ((rc = wget_args(&o, argc, argv, &why))) {
        fprintf(2, "wget: %s\nusage: wget [-q] [-c] [-O FILE] URL...\n", why);
        return rc;
    }
    quiet = o.quiet;
    C.fd = -1;
    signal(SIGPIPE, SIG_IGN);
    int pr = getpriority(PRIO_PROCESS, 0);      /* one level lower, as ssh: the handshake's arithmetic takes up to a
                                                 * minute on a 68020, netd keeps up, other consoles stay snappy */
    if (pr < 19)
        setpriority(PRIO_PROCESS, 0, pr + 1);
    clk.trusted = clock_trusted();
    clk.floor = clock_floor();
    for (i = 0; i < o.nurl; i++)
        ex = wget_exit_merge(ex, fetch(&o, i));
    conn_close(&C);
    if (T) {
        zc_tls_cache_wipe(&T->cache);
        zc_wipe(T, sizeof *T);
    }
    return ex;
}
