/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A PC client over zpkg's core and the HTTP core (DESIGN D22 8.1, tools/host/zpkg-test.sh): the order of work zpkg
 * follows for -Sy and -Sw (4.3, 5.1, 7.5), with POSIX sockets standing in for Zenix's socket layer (the main
 * session's step 5). Not shipped; a test driver that exercises the cores against a real server.
 *   zpkg-client get [OPTS] URL OUT
 *        one file: redirects, keep-alive (--repeat N fetches it N times on one connection), resume from OUT.part
 *        "ok BYTES STATUS fresh|resumed|restarted conns=K" or "error CODE: WHY"
 *   zpkg-client sync [OPTS] STATEDIR CHANNEL test|release PUBHEX[,PUBHEX] MIRROR... [-- NAME...]
 *        -Sy against the mirrors in order (latest, the list and its .sig, zc_sig_verify, the trust rules, stored as
 *        STATEDIR/sync/CHANNEL.index + .sig), then -Sw of NAME... ("system" for the system bundle) into
 *        STATEDIR/cache: size and SHA-256 against the list while streaming, the header's version and channel class,
 *        the .sig fetched beside it
 * OPTS: --limit N, --total N (get), --connect-ms, --idle-ms, --file-ms (timeouts), --repeat N (get),
 *       --clock YYYYMMDD, --clockfloor YYYYMMDD, --floor SERIAL, --snapshot NAME, --ignore-expiry (sync)
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
#include <time.h>
#include <unistd.h>
#include "../../user/zpkg/zpkg.h"
#include "../../net/http/http.h"
#include "../../user/sha256.h"
#include "../../crypto/zc/zc.h"

static int connect_ms = 15000, idle_ms = 60000, file_ms = 600000;

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

struct conn {
    int fd;
    char host[HTTP_HOSTMAX + 1];
    unsigned port;
    int n;                          /* connections made */
    unsigned char buf[4096];
    size_t have, off;               /* unread bytes buf[off..have) */
};

static void conn_close(struct conn *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->have = c->off = 0;
}

static int conn_open(struct conn *c, const struct http_url *u, const char **why)
{
    struct addrinfo hints, *ai = 0;
    char port[8];
    struct pollfd p;
    int e = 0;
    socklen_t el = sizeof e;
    if (c->fd >= 0 && !strcmp(c->host, u->host) && c->port == u->port)
        return 0;
    conn_close(c);
    if (u->tls) {
        *why = "https needs D21's TLS wrapper (not in this test client)";
        return HTTP_EPROTO;
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%u", u->port);
    if (getaddrinfo(u->host, port, &hints, &ai) || !ai) {
        *why = "the name does not resolve";
        return -ENOENT;
    }
    c->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (c->fd < 0 || (connect(c->fd, ai->ai_addr, ai->ai_addrlen) && errno != EINPROGRESS)) {
        freeaddrinfo(ai);
        conn_close(c);
        *why = "connect failed";
        return -ECONNREFUSED;
    }
    freeaddrinfo(ai);
    p.fd = c->fd;
    p.events = POLLOUT;
    if (poll(&p, 1, connect_ms) != 1) {
        conn_close(c);
        *why = "connect timed out";
        return -ETIMEDOUT;
    }
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &e, &el);
    if (e) {
        conn_close(c);
        *why = "connect refused";
        return -ECONNREFUSED;
    }
    strcpy(c->host, u->host);
    c->port = u->port;
    c->n++;
    return 0;
}

/* bytes from the connection into c->buf: >0, 0 closed, <0 timeout or error */
static int conn_fill(struct conn *c, long deadline, const char **why)
{
    struct pollfd p;
    long left = deadline - now_ms();
    ssize_t k;
    if (left <= 0) {
        *why = "the file took longer than its total time";
        return -ETIMEDOUT;
    }
    p.fd = c->fd;
    p.events = POLLIN;
    if (poll(&p, 1, left < idle_ms ? (int)left : idle_ms) != 1) {
        *why = left < idle_ms ? "the file took longer than its total time" : "the server stalled";
        return -ETIMEDOUT;
    }
    k = read(c->fd, c->buf, sizeof c->buf);
    if (k < 0) {
        *why = "read error (reset?)";
        return -ECONNRESET;
    }
    c->off = 0;
    c->have = (size_t)k;
    return (int)k;
}

static int send_all(struct conn *c, const char *s, size_t n)
{
    while (n) {
        ssize_t k = write(c->fd, s, n);
        if (k < 0 && errno == EAGAIN) {
            struct pollfd p = { c->fd, POLLOUT, 0 };
            if (poll(&p, 1, idle_ms) != 1)
                return -ETIMEDOUT;
            continue;
        }
        if (k <= 0)
            return -EPIPE;
        s += k;
        n -= (size_t)k;
    }
    return 0;
}

struct sink {
    FILE *f;
    struct sha256 sha;
    unsigned long n;
};

/* one GET with its redirects, the body into s (opened by the caller at offset from). Returns the final status, or
 * <0. *resumed: HTTP_RESUME / HTTP_RESTART / 0 */
static int get(struct conn *c, struct http_url u, unsigned long from, unsigned long limit, unsigned long total,
               struct sink *s, int *how, const char **why)
{
    static struct http_resp r;
    int hops = 0, rc, tries;
    long deadline = now_ms() + file_ms;
    char req[HTTP_PATHMAX + HTTP_HOSTMAX + 128];
    *how = 0;
    for (;;) {
        for (tries = 0; tries < 2; tries++) {   /* a kept connection the server closed meanwhile: once again */
            int reused = c->fd >= 0 && !strcmp(c->host, u.host) && c->port == u.port;
            if ((rc = conn_open(c, &u, why)) < 0)
                return rc;
            if ((rc = http_request(req, sizeof req, &u, from, 0)) < 0) {
                *why = "the request does not fit";
                return rc;
            }
            if (send_all(c, req, (size_t)rc) < 0) {
                conn_close(c);
                if (reused)
                    continue;
                *why = "the request could not be sent";
                return -EPIPE;
            }
            http_resp_init(&r, limit, 0);
            if (c->off == c->have) {
                rc = conn_fill(c, deadline, why);
                if (rc == 0 && reused) {
                    conn_close(c);
                    continue;
                }
                if (rc <= 0) {
                    conn_close(c);
                    if (!rc)
                        *why = "the server closed the connection";
                    return rc ? rc : HTTP_EIO;
                }
            }
            break;
        }
        if (tries == 2)
            return HTTP_EIO;
        /* the response */
        for (;;) {
            size_t used, bl;
            const unsigned char *b;
            rc = http_resp_feed(&r, c->buf + c->off, c->have - c->off, &used, &b, &bl);
            c->off += used;
            if (rc == HTTP_NEED) {
                rc = conn_fill(c, deadline, why);
                if (rc < 0) {
                    conn_close(c);
                    return rc;
                }
                if (rc == 0) {
                    rc = http_resp_eof(&r);
                    conn_close(c);
                    if (rc < 0) {
                        *why = "the connection ended before the response did";
                        return rc;
                    }
                    break;
                }
                continue;
            }
            if (rc < 0) {
                *why = rc == HTTP_E2BIG ? "the server sent more than expected" : "a response this client does not take";
                conn_close(c);
                return rc;
            }
            if (rc == HTTP_HEAD) {
                if (http_is_redirect(r.status))
                    continue;           /* its body is read and dropped (bounded by the limit) */
                if (r.status != 200 && r.status != 206) {
                    *why = "the server answered with an error status";
                    if (r.status == 404)
                        *why = "not found";
                    continue;
                }
                if (from) {
                    *how = http_resume(&r, from, total);
                    if (*how < 0) {
                        *why = "a wrong Content-Range";
                        conn_close(c);
                        return *how;
                    }
                    if (*how == HTTP_RESTART) {
                        fflush(s->f);
                        if (ftruncate(fileno(s->f), 0) || fseek(s->f, 0, SEEK_SET))
                            return -EIO;
                        sha256_init(&s->sha);
                        s->n = 0;
                    }
                }
                continue;
            }
            if (rc == HTTP_DATA) {
                if (http_is_redirect(r.status) || (r.status != 200 && r.status != 206))
                    continue;
                if (fwrite(b, 1, bl, s->f) != bl)
                    return -EIO;
                sha256_update(&s->sha, b, (unsigned)bl);
                s->n += (unsigned long)bl;
                continue;
            }
            if (rc == HTTP_END)
                break;
        }
        if (r.close)
            conn_close(c);
        if (http_is_redirect(r.status)) {
            if (!r.location[0]) {
                *why = "a redirect without a Location";
                return HTTP_EPROTO;
            }
            if ((rc = http_redirect(&u, r.location, &hops)) < 0) {
                *why = rc == HTTP_ELOOP ? "more than 3 redirects" : "a redirect this client does not follow";
                return rc;
            }
            continue;
        }
        return r.status;
    }
}

/* fetch URL into PATH (via PATH.part, resumed if there); the SHA-256 of the whole file into digest */
static int fetch(struct conn *c, const char *url, const char *path, unsigned long limit, unsigned long total,
                 unsigned char digest[32], unsigned long *size, int *how, const char **why)
{
    struct http_url u;
    struct sink s;
    char part[4096];
    struct stat st;
    unsigned long from = 0;
    int rc;
    if (http_url_parse(&u, url)) {
        *why = "a bad URL";
        return HTTP_EINVAL;
    }
    snprintf(part, sizeof part, "%s.part", path);
    sha256_init(&s.sha);
    s.n = 0;
    if (total && !stat(part, &st) && st.st_size > 0 && (unsigned long)st.st_size < total) {
        /* resume: the hash of what is there first */
        FILE *f = fopen(part, "rb");
        unsigned char b[4096];
        size_t k;
        if (f) {
            while ((k = fread(b, 1, sizeof b, f)) > 0)
                sha256_update(&s.sha, b, (unsigned)k);
            fclose(f);
            from = (unsigned long)st.st_size;
            s.n = from;
        }
    }
    s.f = fopen(part, from ? "ab" : "wb");
    if (!s.f) {
        *why = "cannot write";
        return -EIO;
    }
    rc = get(c, u, from, limit - from, total, &s, how, why);
    fclose(s.f);
    if (rc < 0)
        return rc;
    if (rc != 200 && rc != 206) {
        unlink(part);
        return -ENOENT;
    }
    sha256_final(&s.sha, digest);
    *size = s.n;
    if (rename(part, path))
        return -EIO;
    return 0;
}

static int hexval(const char *h, unsigned char *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (unsigned char)v;
    }
    return 0;
}

static char *slurp(const char *path, size_t *n, size_t max)
{
    FILE *f = fopen(path, "rb");
    char *b;
    size_t k;
    if (!f)
        return 0;
    b = malloc(max + 2);
    k = fread(b, 1, max + 1, f);
    fclose(f);
    if (k > max) {
        free(b);
        return 0;
    }
    b[k] = 0;
    *n = k;
    return b;
}

static int cmd_get(int argc, char **argv)
{
    unsigned long limit = 16ul << 20, total = 0, size = 0;
    int repeat = 1, i, rc = 0, how = 0;
    const char *why = "";
    unsigned char d[32];
    static struct conn c;
    c.fd = -1;
    for (i = 2; i + 1 < argc && argv[i][0] == '-' && argv[i][1] == '-'; i += 2) {
        long v = atol(argv[i + 1]);
        if (!strcmp(argv[i], "--limit")) limit = (unsigned long)v;
        else if (!strcmp(argv[i], "--total")) total = (unsigned long)v;
        else if (!strcmp(argv[i], "--repeat")) repeat = (int)v;
        else if (!strcmp(argv[i], "--connect-ms")) connect_ms = (int)v;
        else if (!strcmp(argv[i], "--idle-ms")) idle_ms = (int)v;
        else if (!strcmp(argv[i], "--file-ms")) file_ms = (int)v;
    }
    if (i + 2 != argc)
        return 2;
    for (int k = 0; k < repeat && rc == 0; k++)
        rc = fetch(&c, argv[i], argv[i + 1], limit, total, d, &size, &how, &why);
    if (rc < 0) {
        printf("error %d: %s\n", rc, why);
        return 1;
    }
    printf("ok %lu %s conns=%d sha=", size, how == HTTP_RESUME ? "resumed" : how == HTTP_RESTART ? "restarted" : "fresh",
           c.n);
    for (i = 0; i < 32; i++)
        printf("%02x", d[i]);
    printf("\n");
    return 0;
}

static int write_atomic(const char *path, const void *data, size_t n)
{
    char tmp[4096];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = fopen(tmp, "wb");
    if (!f || fwrite(data, 1, n, f) != n || fclose(f))
        return -1;
    return rename(tmp, path);
}

static zp_u32 rd32(const unsigned char *b)
{
    return (zp_u32)b[0] << 24 | (zp_u32)b[1] << 16 | (zp_u32)b[2] << 8 | b[3];
}

static int cmd_sync(int argc, char **argv)
{
    struct zp_trust t;
    struct zp_index ix;
    const char *state, *chan, *why = "", *snapshot = 0;
    char path[4096], url[4096], *stored = 0, *list = 0;
    size_t slen = 0;
    unsigned char pubs[4][32], digest[32];
    int npub = 0, i, rc, have = 0, m, mirror0, mirrors;
    unsigned long size;
    int how;
    static struct conn c;
    c.fd = -1;
    memset(&t, 0, sizeof t);
    for (i = 2; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--ignore-expiry")) { t.flags |= ZP_T_IGNORE_EXPIRY; continue; }
        if (i + 1 >= argc) return 2;
        if (!strcmp(argv[i], "--clock")) t.clock = (zp_u32)atol(argv[++i]);
        else if (!strcmp(argv[i], "--clockfloor")) t.clockfloor = (zp_u32)atol(argv[++i]);
        else if (!strcmp(argv[i], "--floor")) t.floor = (zp_u32)atol(argv[++i]);
        else if (!strcmp(argv[i], "--snapshot")) { snapshot = argv[++i]; t.snapshot = snapshot; }
        else if (!strcmp(argv[i], "--idle-ms")) idle_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--file-ms")) file_ms = atoi(argv[++i]);
        else return 2;
    }
    if (argc - i < 5)
        return 2;
    state = argv[i];
    chan = argv[i + 1];
    if (snapshot && !zp_snapshot_name_ok(snapshot))
        return 2;
    t.channel = snapshot ? 0 : chan;
    t.keyclass = !strcmp(argv[i + 2], "test") ? ZP_KEY_TEST : ZP_KEY_RELEASE;
    for (char *p = argv[i + 3]; *p && npub < 4; p += 64 + (p[64] == ',')) {
        if (strlen(p) < 64 || hexval(p, pubs[npub++], 32))
            return 2;
        if (!p[64])
            break;
    }
    mirror0 = i + 4;
    for (mirrors = 0; mirror0 + mirrors < argc && strcmp(argv[mirror0 + mirrors], "--"); mirrors++)
        ;
    snprintf(path, sizeof path, "%s/sync", state);
    mkdir(state, 0755);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/cache", state);
    mkdir(path, 0755);
    /* the stored list */
    if (snapshot)
        snprintf(path, sizeof path, "%s/sync/snapshot-%s.index", state, snapshot);
    else
        snprintf(path, sizeof path, "%s/sync/%s.index", state, chan);
    stored = slurp(path, &slen, ZP_INDEX_MAX);
    if (stored) {
        if (zp_index_parse(&ix, stored, slen, &why)) {
            printf("the stored list is damaged: %s\n", why);
            return 1;
        }
        if (snapshot && zp_index_usable(&ix, &t, 0, &why) == ZP_ESNAPSHOT) {
            /* a stored file that is not this snapshot's (damaged, or older than amendment 1): not a floor */
            printf("the stored snapshot list is not %s's: ignored\n", snapshot);
            zp_index_free(&ix);
            free(stored);
            stored = 0;
        } else {
            t.stored = ix.serial;
            have = 1;
        }
    }
    for (m = 0; m < mirrors; m++) {
        const char *base = argv[mirror0 + m];
        char lat[ZP_LATEST_MAX + 1];
        char sigbuf[ZC_SIG_SIZE + 1];
        zp_u32 serial;
        unsigned warn = 0;
        size_t n;
        char *where = snapshot ? (char *)"snapshots/" : (char *)"";
        const char *dir = snapshot ? snapshot : chan;
        snprintf(url, sizeof url, "%s/%s%s/latest", base, where, dir);
        snprintf(path, sizeof path, "%s/latest.tmp", state);
        if ((rc = fetch(&c, url, path, ZP_LATEST_MAX, 0, digest, &size, &how, &why)) < 0) {
            printf("mirror %d: latest: %s\n", m + 1, why);
            continue;
        }
        {
            char *b = slurp(path, &n, ZP_LATEST_MAX);
            unlink(path);
            if (!b || zp_latest_parse(b, n, &serial)) {
                printf("mirror %d: a bad latest\n", m + 1);
                free(b);
                continue;
            }
            memcpy(lat, b, n);
            free(b);
        }
        if (!snapshot) {
            int d = zp_latest_decide(serial, t.stored);
            if (d == ZP_LATEST_BEHIND) {
                printf("mirror %d: this mirror is behind\n", m + 1);
                continue;
            }
            if (d == ZP_LATEST_SAME && have) {
                rc = zp_index_usable(&ix, &t, &warn, &why);
                if (rc) {
                    printf("refused: %s (expired %u, today %u)\n", why, ix.expires, zp_today(&t));
                    return 1;
                }
                printf("same %u%s\n", ix.serial, warn & ZP_W_CLOCK_BEHIND ? " (the clock is behind)" : "");
                goto fetch_pkgs;
            }
        }
        /* the list and its .sig */
        snprintf(url, sizeof url, "%s/%s%s/index-%u.sig", base, where, dir, serial);
        snprintf(path, sizeof path, "%s/sig.tmp", state);
        if ((rc = fetch(&c, url, path, ZC_SIG_SIZE, 0, digest, &size, &how, &why)) < 0 || size != ZC_SIG_SIZE) {
            printf("mirror %d: the list's .sig: %s\n", m + 1, rc < 0 ? why : "not 80 bytes");
            unlink(path);
            continue;
        }
        {
            char *b = slurp(path, &n, ZC_SIG_SIZE);
            unlink(path);
            if (!b)
                continue;
            memcpy(sigbuf, b, ZC_SIG_SIZE);
            free(b);
        }
        snprintf(url, sizeof url, "%s/%s%s/index-%u", base, where, dir, serial);
        snprintf(path, sizeof path, "%s/index.tmp", state);
        if ((rc = fetch(&c, url, path, ZP_INDEX_MAX, 0, digest, &size, &how, &why)) < 0) {
            printf("mirror %d: the list: %s\n", m + 1, why);
            unlink(path);
            continue;
        }
        list = slurp(path, &n, ZP_INDEX_MAX);
        unlink(path);
        if (!list)
            continue;
        /* the signature over the bytes fetched, before a line is parsed (5.1) */
        rc = zc_sig_verify((unsigned char *)sigbuf, ZC_SIG_SIZE, ZC_SIG_LABEL_INDEX, digest,
                           (const unsigned char (*)[32])pubs, npub, 0);
        if (rc != ZC_SIG_OK) {
            printf("mirror %d: the list's signature does not verify (%d)\n", m + 1, rc);
            free(list);
            list = 0;
            continue;
        }
        {
            char *copy = malloc(n + 1);
            struct zp_index nx;
            memcpy(copy, list, n);
            if (zp_index_parse(&nx, copy, n, &why)) {
                printf("mirror %d: refused: %s\n", m + 1, why);
                free(copy);
                free(list);
                list = 0;
                continue;
            }
            if (nx.serial != serial) {
                printf("mirror %d: the list's serial is not latest's\n", m + 1);
                zp_index_free(&nx);
                free(copy);
                free(list);
                list = 0;
                continue;
            }
            rc = zp_index_check(&nx, &t, &warn, &why);
            if (rc) {
                printf("mirror %d: refused (%d): %s\n", m + 1, rc, why);
                zp_index_free(&nx);
                free(copy);
                free(list);
                list = 0;
                continue;
            }
            /* stored: the .sig, then the list as .new renamed, the list last (4.1) */
            snprintf(path, sizeof path, "%s/sync/%s%s.index.sig", state, snapshot ? "snapshot-" : "", dir);
            write_atomic(path, sigbuf, ZC_SIG_SIZE);
            snprintf(path, sizeof path, "%s/sync/%s%s.index", state, snapshot ? "snapshot-" : "", dir);
            write_atomic(path, list, n);
            if (have)
                zp_index_free(&ix);
            free(stored);
            stored = copy;
            ix = nx;
            have = 1;
            printf("new %u%s%s%s\n", ix.serial, warn & ZP_W_CLOCK_BEHIND ? " (the clock is behind)" : "",
                   warn & ZP_W_UNCHECKED ? " (a snapshot: its age and expiry not checked)" : "",
                   warn & ZP_W_EXPIRY_IGNORED ? " (expiry ignored)" : "");
            free(list);
            list = 0;
            (void)lat;
            goto fetch_pkgs;
        }
    }
    printf("no mirror answered with a usable list\n");
    return 1;
fetch_pkgs:
    rc = 0;
    for (i = mirror0 + mirrors + 1; i < argc; i++) {
        const struct zp_pkg *p = strcmp(argv[i], "system") ? zp_index_find(&ix, argv[i]) : 0;
        const char *file = p ? p->file : ix.sys.file, *sha = p ? p->sha : ix.sys.sha;
        zp_u32 fsize = p ? p->fsize : ix.sys.fsize;
        unsigned char want[32], hdr[96];
        int ok = 0;
        if (strcmp(argv[i], "system") && (!p || !p->file)) {
            printf("%s: not a package file in the list\n", argv[i]);
            rc = 1;
            continue;
        }
        zp_hex32(want, sha);
        snprintf(path, sizeof path, "%s/cache/%s", state, file);
        {
            /* a cached file is checked against the list again before use */
            size_t n;
            char *b = slurp(path, &n, ZP_BUNDLE_MAX);
            if (b) {
                unsigned char d[32];
                sha256(b, (unsigned)n, d);
                if (n == fsize && !memcmp(d, want, 32)) {
                    printf("%s: cached\n", file);
                    ok = 1;
                } else
                    unlink(path);
                free(b);
            }
        }
        for (m = 0; m < mirrors && !ok; m++) {
            snprintf(url, sizeof url, "%s/pool/%s", argv[mirror0 + m], file);
            for (int attempt = 0; attempt < 2 && !ok; attempt++) {
                rc = fetch(&c, url, path, fsize, fsize, digest, &size, &how, &why);
                if (rc < 0) {
                    printf("%s: mirror %d: %s\n", file, m + 1, why);
                    break;
                }
                if (size != fsize || memcmp(digest, want, 32)) {
                    /* a wrong prefix, or a damaged file: deleted, fetched once from the start */
                    printf("%s: mirror %d: size or SHA-256 is not the list's%s\n", file, m + 1,
                           how == HTTP_RESUME ? " (after a resume)" : "");
                    unlink(path);
                    if (how != HTTP_RESUME)
                        break;
                    continue;
                }
                ok = 1;
                printf("%s: fetched %s\n", file, how == HTTP_RESUME ? "resumed" : how == HTTP_RESTART ? "restarted" : "whole");
            }
        }
        if (!ok) {
            rc = 1;
            continue;
        }
        /* the header's version and channel class (the name comes from the manifest through zpkgfmt.c in zpkg) */
        {
            FILE *f = fopen(path, "rb");
            char chn[9];
            if (!f || fread(hdr, 1, 96, f) != 96) {
                if (f)
                    fclose(f);
                printf("%s: short\n", file);
                rc = 1;
                continue;
            }
            fclose(f);
            memcpy(chn, hdr + 40, 8);
            chn[8] = 0;
            if (memcmp(hdr, "ZUP1", 4) || zp_bundle_fits(&ix, p, p ? p->name : 0, rd32(hdr + 12), chn, t.keyclass, &why)) {
                printf("%s: refused: %s\n", file, memcmp(hdr, "ZUP1", 4) ? "not a bundle" : why);
                unlink(path);
                rc = 1;
                continue;
            }
        }
        /* its .sig beside it, for -U and --back later (not verified now: the signed list covers the file) */
        snprintf(url, sizeof url, "%s/pool/%s.sig", argv[mirror0], file);
        snprintf(path, sizeof path, "%s/cache/%s.sig", state, file);
        if (fetch(&c, url, path, ZC_SIG_SIZE, 0, digest, &size, &how, &why) < 0 || size != ZC_SIG_SIZE) {
            printf("%s: its .sig is missing\n", file);
            unlink(path);
            rc = 1;
            continue;
        }
        printf("%s: ok\n", file);
    }
    if (have)
        zp_index_free(&ix);
    free(stored);
    conn_close(&c);
    return rc;
}

int main(int argc, char **argv)
{
    int rc = 2;
    if (argc >= 2 && !strcmp(argv[1], "get"))
        rc = cmd_get(argc, argv);
    else if (argc >= 2 && !strcmp(argv[1], "sync"))
        rc = cmd_sync(argc, argv);
    if (rc == 2)
        fprintf(stderr, "usage: zpkg-client get [OPTS] URL OUT | sync [OPTS] STATEDIR CHANNEL test|release PUBHEX MIRROR... "
                        "[-- NAME...]\n");
    fflush(stdout);
    return rc;
}
