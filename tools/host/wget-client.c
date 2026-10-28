/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * wget on the PC over the cores (DESIGN D21 10 step 7, tools/host/tls-test.sh): user/wget/'s plan, net/http/ and
 * crypto/zc/zc_tls.h's glue, with POSIX sockets standing in for Zenix's socket layer (the main session's wget
 * program). Not shipped; a test driver that runs the cores against real servers.
 *   wget-client [HOSTOPTS] [-q] [-c] [-O FILE] URL...
 * HOSTOPTS (before wget's own options):
 *   --connect IP         every host name connects to IP (the URL's port): the test names need no DNS
 *   --anchors FILE       the trust anchors file (tools/mkanchors.py's format)
 *   --local PEM          a root of the owner's own (/etc/ssl/local/'s stand-in), may repeat
 *   --clock T --floor F --untrusted   the time rules (D21 8.3): now, the floor, and no trusted clock
 *   --idle-ms N          the socket layer's limit between bytes (default 60000)
 *   --seed HEX64         BearSSL's 32 seed bytes (default from /dev/urandom)
 *   --record FILE        every byte received, appended (a fuzzing seed)
 *   --no-cache           no session resumption
 * stderr: one "wget-client: url N: ..." line per URL and "wget-client: exit E conns C handshakes H resumed R".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include "../../user/wget/wget.h"
#include "../../net/http/http.h"
#include "../../crypto/zc/zc_tls.h"

static const char *connect_ip;
static int idle_ms = 60000, nocache;
static FILE *rec;
static unsigned char seed[32];
static int seed_given;
static struct zc_clock clk;
static FILE *anchors;
static unsigned char *locals[8];
static size_t localn[8];
static int nlocal;
static int conns, handshakes, resumed;

static int rd(void *u, unsigned long off, void *buf, unsigned long n)
{
    (void)u;
    if (!anchors || fseek(anchors, (long)off, SEEK_SET) || fread(buf, 1, n, anchors) != n)
        return -1;
    return 0;
}

static int find(void *u, struct zc_x509 *x, const unsigned char hash[8], const unsigned char *dn, size_t dnlen)
{
    int i, r;

    (void)u;
    (void)dn;
    (void)dnlen;
    if (anchors && (r = zc_anchors_find(x, rd, 0, hash)))
        return r;
    for (i = 0; i < nlocal; i++)        /* the owner's roots: every one (the engine picks by name) */
        if ((r = zc_x509_add_cert(x, locals[i], localn[i])))
            return r;
    return 0;
}

struct conn {
    int fd;
    struct http_url at;                 /* scheme, host and port it is open to */
    int tls;
    struct zc_tls t;
    unsigned char buf[4096];
    size_t have, off;
};

static unsigned char iobuf[ZC_TLS_BUFSIZE], chainbuf[ZC_X509_CHAINMAX], store[16384];
static struct zc_tls_cache cache;
static struct conn C = { .fd = -1 };

static long sock_send(void *u, const unsigned char *p, size_t n)
{
    struct conn *c = u;

    for (;;) {
        ssize_t k = write(c->fd, p, n);

        if (k > 0)
            return k;
        if (k < 0 && errno == EAGAIN) {
            struct pollfd q = { c->fd, POLLOUT, 0 };

            if (poll(&q, 1, idle_ms) != 1)
                return -ETIMEDOUT;
            continue;
        }
        return -EPIPE;
    }
}

static long sock_recv(void *u, unsigned char *p, size_t n)
{
    struct conn *c = u;
    struct pollfd q = { c->fd, POLLIN, 0 };
    ssize_t k;

    if (poll(&q, 1, idle_ms) != 1)
        return -ETIMEDOUT;
    k = read(c->fd, p, n);
    if (k < 0)
        return -ECONNRESET;
    if (k > 0 && rec)
        fwrite(p, 1, (size_t)k, rec);
    return k;
}

static void conn_close(struct conn *c)
{
    if (c->fd < 0)
        return;
    if (c->tls)
        zc_tls_close(&c->t);
    close(c->fd);
    c->fd = -1;
    c->have = c->off = 0;
}

/* 0, or an exit status with *why */
static int conn_open(struct conn *c, const struct http_url *u, const char **why)
{
    struct addrinfo hints, *ai = 0;
    struct sockaddr_in sin;
    char port[8];
    int r;

    if (c->fd >= 0 && wget_same_server(&c->at, u))
        return 0;
    conn_close(c);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%u", u->port);
    if (connect_ip) {
        memset(&sin, 0, sizeof sin);
        sin.sin_family = AF_INET;
        sin.sin_port = htons((uint16_t)u->port);
        inet_pton(AF_INET, connect_ip, &sin.sin_addr);
    } else if (getaddrinfo(u->host, port, &hints, &ai) || !ai) {
        *why = "the name does not resolve";
        return WGET_EXIT_NET;
    }
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0 || connect(c->fd, ai ? ai->ai_addr : (struct sockaddr *)&sin, ai ? ai->ai_addrlen : sizeof sin)) {
        if (ai)
            freeaddrinfo(ai);
        if (c->fd >= 0)
            close(c->fd);
        c->fd = -1;
        *why = "connect failed";
        return WGET_EXIT_NET;
    }
    if (ai)
        freeaddrinfo(ai);
    conns++;
    c->at = *u;
    c->tls = u->tls;
    c->have = c->off = 0;
    if (u->tls) {
        struct zc_tls_io io = { c, sock_send, sock_recv };
        unsigned char s[32];

        if (seed_given)
            memcpy(s, seed, 32);
        else {
            int f = open("/dev/urandom", O_RDONLY);

            if (f < 0 || read(f, s, 32) != 32)
                abort();
            close(f);
        }
        zc_tls_init(&c->t, iobuf, chainbuf, store, sizeof store, find, 0, nocache ? 0 : &cache);
        handshakes++;
        r = zc_tls_connect(&c->t, &io, u->host, u->port, &clk, s, "http/1.1");
        zc_wipe(s, 32);
        if (c->t.clock.used_floor)
            fprintf(stderr, "wget-client: the clock is not set: certificate dates checked only against %u\n",
                    (unsigned)clk.floor);
        if (r < 0) {
            int e = r == ZC_TLS_EENGINE ? zc_tls_error(&c->t) : r;

            fprintf(stderr, "wget-client: tls error %d: %s\n", e, r == ZC_TLS_EENGINE || r == ZC_TLS_ETRUNC ?
                    zc_tls_strerror(e) : strerror(-r));
            close(c->fd);
            c->fd = -1;
            *why = "the TLS handshake failed";
            return r == ZC_TLS_EENGINE && e >= BR_ERR_X509_OK && e < BR_ERR_RECV_FATAL_ALERT ? WGET_EXIT_TLS
                 : r == ZC_TLS_EENGINE && (e == ZC_X509_ERR_EKU || e == ZC_X509_ERR_CHAIN) ? WGET_EXIT_TLS
                 : r == ZC_TLS_EENGINE ? WGET_EXIT_PROTO : WGET_EXIT_NET;
        }
        if (c->t.resumed)
            resumed++;
        fprintf(stderr, "wget-client: tls %s %s alpn=%s\n", u->host, c->t.resumed ? "resumed" : "full",
                zc_tls_alpn(&c->t) ? zc_tls_alpn(&c->t) : "-");
    }
    return 0;
}

/* bytes into c->buf: > 0, 0 the end (TCP close or close_notify), < 0 an error (*trunc: TLS ended without
   close_notify) */
static long conn_fill(struct conn *c, int *trunc)
{
    long k;

    *trunc = 0;
    if (c->tls) {
        k = zc_tls_read(&c->t, c->buf, sizeof c->buf);
        if (k == ZC_TLS_ETRUNC) {
            *trunc = 1;
            return 0;
        }
    } else {
        k = sock_recv(c, c->buf, sizeof c->buf);
    }
    if (k > 0) {
        c->off = 0;
        c->have = (size_t)k;
    }
    return k;
}

static int conn_send(struct conn *c, const char *s, size_t n)
{
    if (c->tls)
        return zc_tls_write(&c->t, s, n);
    while (n) {
        long k = sock_send(c, (const unsigned char *)s, n);

        if (k < 0)
            return (int)k;
        s += k;
        n -= (size_t)k;
    }
    return 0;
}

/* One URL: 0 or an exit status. */
static int fetch(const struct wget_opts *o, int i)
{
    static struct http_resp r;
    struct wget_plan p;
    const char *why = "";
    char req[HTTP_PATHMAX + HTTP_HOSTMAX + 128];
    int rc, act, ex, tries, need;
    FILE *out = 0;
    unsigned long wrote = 0;

    if ((rc = wget_plan_start(&p, o, i, &why))) {
        fprintf(stderr, "wget-client: url %d: fail %d: %s\n", i, rc, why);
        return rc;
    }
    if (p.need_size) {
        struct stat st;

        wget_plan_have(&p, stat(p.name, &st) == 0, stat(p.name, &st) == 0 ? (unsigned long)st.st_size : 0);
    }
    if (p.need_free_name) {
        struct stat st;
        char base[WGET_NAMEMAX + 1];
        unsigned n = 1;

        strcpy(base, p.name);
        while (stat(p.name, &st) == 0)
            if (wget_nextname(p.name, sizeof p.name, base, n++)) {
                fprintf(stderr, "wget-client: url %d: fail 3: no free name\n", i);
                return WGET_EXIT_FILE;
            }
    }
    for (;;) {
        for (tries = 0; tries < 2; tries++) {   /* a kept connection the server closed meanwhile: once again */
            int reused = C.fd >= 0 && wget_same_server(&C.at, &p.url), tr;

            if ((rc = conn_open(&C, &p.url, &why))) {
                fprintf(stderr, "wget-client: url %d: fail %d: %s\n", i, rc, why);
                return rc;
            }
            rc = http_request(req, sizeof req, &p.url, p.from, 0);
            if (rc < 0) {
                fprintf(stderr, "wget-client: url %d: fail 1: the request does not fit\n", i);
                return WGET_EXIT_GENERIC;
            }
            if (conn_send(&C, req, (size_t)rc) < 0) {
                conn_close(&C);
                if (reused)
                    continue;
                fprintf(stderr, "wget-client: url %d: fail 4: the request could not be sent\n", i);
                return WGET_EXIT_NET;
            }
            http_resp_init(&r, 0xFFFFFFFFul, 0);
            if (C.off == C.have) {
                long k = conn_fill(&C, &tr);

                if (k <= 0) {
                    conn_close(&C);
                    if (reused && k == 0)
                        continue;
                    fprintf(stderr, "wget-client: url %d: fail 4: %s (%ld)\n", i,
                            k == -ETIMEDOUT ? "the server stalled" : "no answer", k);
                    return WGET_EXIT_NET;
                }
            }
            break;
        }
        if (tries == 2) {
            fprintf(stderr, "wget-client: url %d: fail 4: the connection keeps closing\n", i);
            return WGET_EXIT_NET;
        }
        act = 0;
        need = 0;
        for (;;) {                      /* the headers, then the body */
            const unsigned char *body;
            size_t used, blen;
            int tr;

            if (need) {                 /* only when the parser took everything and wants more */
                need = 0;
                long k = conn_fill(&C, &tr);

                if (k < 0) {
                    fprintf(stderr, "wget-client: url %d: fail 4: %s\n", i, k == -ETIMEDOUT ? "the server stalled" :
                            C.tls ? zc_tls_strerror(k == ZC_TLS_EENGINE ? zc_tls_error(&C.t) : (int)k) : "read error");
                    conn_close(&C);
                    if (out && out != stdout)
                        fclose(out);
                    return WGET_EXIT_NET;
                }
                if (k == 0) {
                    rc = tr ? HTTP_EIO : http_resp_eof(&r);
                    conn_close(&C);
                    if (rc == HTTP_END)
                        break;
                    fprintf(stderr, "wget-client: url %d: fail 4: %s after %lu bytes\n", i,
                            tr ? "truncated (no TLS close_notify)" : "the connection ended early", wrote);
                    if (out && out != stdout)
                        fclose(out);
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
                fprintf(stderr, "wget-client: url %d: fail 7: http error %d\n", i, rc);
                conn_close(&C);
                if (out && out != stdout)
                    fclose(out);
                return WGET_EXIT_PROTO;
            }
            if (rc == HTTP_HEAD) {
                act = wget_plan_answer(&p, &r, &ex, &why);
                if (act == WGET_FAIL) {
                    fprintf(stderr, "wget-client: url %d: fail %d: %s (status %d)\n", i, ex, why, r.status);
                    conn_close(&C);
                    return ex;
                }
                if (act == WGET_WRITE) {
                    if (p.to_stdout)
                        out = stdout;
                    else {
                        out = fopen(p.name, p.append ? "ab" : "wb");
                        if (!out) {
                            fprintf(stderr, "wget-client: url %d: fail 3: cannot write %s\n", i, p.name);
                            conn_close(&C);
                            return WGET_EXIT_FILE;
                        }
                    }
                }
                continue;
            }
            if (rc == HTTP_DATA) {
                if (act == WGET_WRITE && fwrite(body, 1, blen, out) != blen) {
                    fprintf(stderr, "wget-client: url %d: fail 3: write error\n", i);
                    conn_close(&C);
                    return WGET_EXIT_FILE;
                }
                if (act == WGET_WRITE)
                    wrote += blen;
                continue;
            }
            /* HTTP_END */
            if (r.close)
                conn_close(&C);
            break;
        }
        if (act == WGET_FOLLOW)
            continue;
        if (out && out != stdout)
            fclose(out);
        else if (out)
            fflush(stdout);
        fprintf(stderr, "wget-client: url %d: %s %d name=%s bytes=%lu hops=%d%s\n", i,
                act == WGET_DONE ? "done" : p.append && p.from ? "resumed" : "saved", r.status, p.name, wrote,
                p.hops, act == WGET_DONE ? " (already complete)" : "");
        return 0;
    }
}

int main(int argc, char **argv)
{
    struct wget_opts o;
    const char *why = "";
    int i = 1, rc, ex = 0;

    clk.now = (uint32_t)time(0);
    clk.trusted = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2) && argv[i][2]; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--connect") && i + 1 < argc)
            connect_ip = argv[++i];
        else if (!strcmp(a, "--anchors") && i + 1 < argc)
            anchors = fopen(argv[++i], "rb");
        else if (!strcmp(a, "--local") && i + 1 < argc && nlocal < 8) {
            FILE *f = fopen(argv[++i], "rb");
            static char pem[65536];
            size_t n, off = 0;
            long k;

            if (!f)
                return 2;
            n = fread(pem, 1, sizeof pem, f);
            fclose(f);
            while (nlocal < 8) {
                locals[nlocal] = malloc(8192);
                k = zc_pem_cert(pem, n, &off, locals[nlocal], 8192);
                if (k <= 0) {
                    free(locals[nlocal]);
                    if (k < 0)
                        fprintf(stderr, "wget-client: a damaged PEM file\n");
                    break;
                }
                localn[nlocal++] = (size_t)k;
            }
        } else if (!strcmp(a, "--clock") && i + 1 < argc)
            clk.now = (uint32_t)strtoul(argv[++i], 0, 10);
        else if (!strcmp(a, "--floor") && i + 1 < argc)
            clk.floor = (uint32_t)strtoul(argv[++i], 0, 10);
        else if (!strcmp(a, "--untrusted"))
            clk.trusted = 0;
        else if (!strcmp(a, "--idle-ms") && i + 1 < argc)
            idle_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--seed") && i + 1 < argc) {
            int k;

            for (k = 0; k < 32; k++)
                sscanf(argv[i + 1] + 2 * k, "%2hhx", &seed[k]);
            seed_given = 1;
            i++;
        } else if (!strcmp(a, "--record") && i + 1 < argc)
            rec = fopen(argv[++i], "ab");
        else if (!strcmp(a, "--no-cache"))
            nocache = 1;
        else
            break;
    }
    argv[i - 1] = argv[0];
    if ((rc = wget_args(&o, argc - i + 1, argv + i - 1, &why))) {
        fprintf(stderr, "wget-client: usage: %s\nwget-client: exit %d conns 0 handshakes 0 resumed 0\n", why, rc);
        return rc;
    }
    for (i = 0; i < o.nurl; i++)
        ex = wget_exit_merge(ex, fetch(&o, i));
    conn_close(&C);
    zc_tls_cache_wipe(&cache);
    if (rec)
        fclose(rec);
    fprintf(stderr, "wget-client: exit %d conns %d handshakes %d resumed %d\n", ex, conns, handshakes, resumed);
    return ex;
}
