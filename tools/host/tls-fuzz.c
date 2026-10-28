/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * libFuzzer harnesses for D21 step 7's cores (tools/host/tls-test.sh fuzz), one per build (-DFUZZ_<NAME>):
 *   TLS   the glue (crypto/zc/zc_tls.c) and BearSSL's client engine fed a server's records: the input is a 4-byte
 *         clock, the server's name and a NUL, then the bytes the server sends (the seeds are real flights recorded
 *         by tls-test.py with the same seed bytes, so a seed gets through the handshake and into the HTTP answer);
 *         the decrypted data goes through net/http/; a second connection over what is left tries resumption
 *   URL   net/http/'s URLs and redirects, wget's names, its command line and plans
 *   PLAN  a response through net/http/ in pieces, wget_plan_answer on its headers
 *   SNTP  sntp_parse, sntp_time, sntp_judge, /etc/sntp, the clock file, the PEM reader
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/wget/wget.h"
#include "../../net/http/http.h"
#include "../../net/sntp/sntp.h"
#include "../../crypto/zc/zc_tls.h"
#include "../../crypto/zc/zc_clock.h"

#ifdef FUZZ_TLS
static unsigned char *anchors;
static size_t anchors_n;
static const uint8_t *in;
static size_t in_n, in_off;

static int memrd(void *u, unsigned long off, void *buf, unsigned long n)
{
    (void)u;
    if (!anchors || off > anchors_n || n > anchors_n - off)
        return -1;
    memcpy(buf, anchors + off, n);
    return 0;
}

static int find(void *u, struct zc_x509 *x, const unsigned char hash[8], const unsigned char *dn, size_t dnlen)
{
    (void)u; (void)dn; (void)dnlen;
    return zc_anchors_find(x, memrd, 0, hash);
}

static long f_send(void *u, const unsigned char *p, size_t n)
{
    (void)u; (void)p;
    return (long)n;
}

static long f_recv(void *u, unsigned char *p, size_t n)
{
    size_t k = in_n - in_off;

    (void)u;
    if (!k)
        return 0;
    if (k > n)
        k = n;
    memcpy(p, in + in_off, k);
    in_off += k;
    return (long)k;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    const char *p = getenv("TLS_FUZZ_ANCHORS");
    FILE *f = p ? fopen(p, "rb") : 0;

    (void)argc; (void)argv;
    if (f) {
        anchors = malloc(1 << 20);
        anchors_n = fread(anchors, 1, 1 << 20, f);
        fclose(f);
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static unsigned char iobuf[ZC_TLS_BUFSIZE], chain[ZC_X509_CHAINMAX], store[16384], app[4096];
    static struct zc_tls t;
    static struct zc_tls_cache cache;
    static struct http_resp r;
    struct zc_tls_io io = { 0, f_send, f_recv };
    struct zc_clock c;
    unsigned char seed[32];
    char host[64];
    size_t i, h;
    int round;

    if (n < 6)
        return 0;
    memset(&c, 0, sizeof c);
    c.now = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    c.trusted = 1;
    for (h = 4; h < n && h < 4 + sizeof host - 1 && d[h]; h++)
        ;
    if (h == n || d[h] || h == 4)
        return 0;
    memcpy(host, d + 4, h - 4);
    host[h - 4] = 0;
    in = d;
    in_n = n;
    in_off = h + 1;
    memset(seed, 0x42, sizeof seed);
    memset(&cache, 0, sizeof cache);
    for (round = 0; round < 2 && in_off < in_n; round++) {
        zc_tls_init(&t, iobuf, chain, store, sizeof store, find, 0, &cache);
        if (zc_tls_connect(&t, &io, host, 443, &c, seed, "http/1.1") == 0) {
            static const char req[] = "GET /close/small.txt HTTP/1.1\r\nHost: x\r\n\r\n";

            if (zc_tls_write(&t, req, sizeof req - 1) == 0) {
                http_resp_init(&r, 1 << 20, 0);
                for (i = 0; i < 4096; i++) {
                    long k = zc_tls_read(&t, app, sizeof app);
                    size_t off = 0;

                    if (k <= 0)
                        break;
                    while (off < (size_t)k) {
                        const unsigned char *body;
                        size_t used, blen;
                        int rc = http_resp_feed(&r, app + off, (size_t)k - off, &used, &body, &blen);

                        off += used;
                        if (rc < 0 || rc == HTTP_END)
                            break;
                    }
                }
            }
        }
        if (getenv("TLS_FUZZ_SAY"))     /* how far a seed got (tls-test.sh checks the recorded one) */
            fprintf(stderr, "tls-fuzz: round %d: error %d, status %d, body %lu bytes\n", round, zc_tls_error(&t),
                    r.status, r.got);
        r.status = 0;
        r.got = 0;
        (void)zc_tls_strerror(zc_tls_error(&t));
        zc_tls_close(&t);
    }
    zc_tls_cache_wipe(&cache);
    return 0;
}
#endif

#ifdef FUZZ_URL
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static char buf[4096], name[WGET_NAMEMAX + 1], req[HTTP_PATHMAX + HTTP_HOSTMAX + 128];
    char *lines[4], *argv[24];
    struct http_url u;
    struct wget_opts o;
    struct wget_plan p;
    const char *why;
    int nl = 0, argc = 1, hops = 0, i;
    char *s;

    if (n >= sizeof buf)
        return 0;
    memcpy(buf, d, n);
    buf[n] = 0;
    for (s = buf, lines[nl++] = s; *s && nl < 4; s++)
        if (*s == '\n') {
            *s = 0;
            lines[nl++] = s + 1;
        }
    if (http_url_parse(&u, lines[0]) == 0) {
        wget_name(name, sizeof name, &u);
        wget_nextname(name, sizeof name, name, 7);
        http_request(req, sizeof req, &u, n, n & 1);
        for (i = 1; i < nl; i++)
            if (http_redirect(&u, lines[i], &hops) == 0) {
                wget_name(name, sizeof name, &u);
                http_request(req, sizeof req, &u, 0, 0);
            }
    }
    /* the whole input as a command line */
    memcpy(buf, d, n);
    buf[n] = 0;
    argv[0] = "wget";
    for (s = strtok(buf, " \n"); s && argc < 24; s = strtok(0, " \n"))
        argv[argc++] = s;
    if (wget_args(&o, argc, argv, &why) == 0)
        for (i = 0; i < o.nurl; i++)
            if (wget_plan_start(&p, &o, i, &why) == 0)
                wget_plan_have(&p, 1, n);
    return 0;
}
#endif

#ifdef FUZZ_PLAN
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static struct http_resp r;
    struct wget_opts o;
    struct wget_plan p;
    const char *why;
    char *argv[5] = { "wget", "-c", "-O", "out", "https://h/a/b" };
    size_t off = 2, step;
    int ex, act;

    if (n < 2)
        return 0;
    if (wget_args(&o, (d[0] & 1) ? 5 : 3, (d[0] & 1) ? argv : (char *[]){ "wget", "-q", "https://h/a/b" }, &why) ||
        wget_plan_start(&p, &o, 0, &why))
        return 0;
    wget_plan_have(&p, 1, (unsigned long)d[1] * 13);
    http_resp_init(&r, (d[0] & 2) ? 1000 : 0xFFFFFFFFul, (d[0] & 4) != 0);
    step = (d[0] >> 3) + 1;
    while (off <= n) {
        const unsigned char *body;
        size_t used, blen, k = n - off < step ? n - off : step;
        int rc = http_resp_feed(&r, d + off, k, &used, &body, &blen);

        if (blen && (body < d + off || body + blen > d + off + k))
            abort();                    /* the body points into the input given */
        off += used;
        if (rc == HTTP_HEAD) {
            act = wget_plan_answer(&p, &r, &ex, &why);
            if (act == WGET_FOLLOW)
                (void)wget_same_server(&p.url, &p.url);
            continue;
        }
        if (rc < 0 || rc == HTTP_END)
            break;
        if (rc == HTTP_NEED && off == n) {
            http_resp_eof(&r);
            break;
        }
    }
    return 0;
}
#endif

#ifdef FUZZ_SNTP
static void warn(void *u, int line, const char *what)
{
    (void)u; (void)line; (void)what;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static const unsigned char nonce[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static unsigned char der[4096];
    struct sntp_answer a;
    struct sntp_conf c;
    uint32_t v;
    size_t off = 0;

    if (sntp_parse(&a, d, n, nonce) == SNTP_OK) {
        uint32_t e = n > 52 ? (uint32_t)d[48] << 24 | (uint32_t)d[49] << 16 | (uint32_t)d[50] << 8 | d[51] : 0;
        uint32_t t = sntp_time(&a, e);

        sntp_judge(t, e, n & 1, a.rx_secs);
    }
    if (n > 48) {
        sntp_conf_parse(&c, (const char *)d + 48, n - 48, warn, 0);
        zc_clock_parse((const char *)d + 48, n - 48, &v);
        while (zc_pem_cert((const char *)d + 48, n - 48, &off, der, sizeof der) > 0)
            ;
    }
    return 0;
}
#endif
