/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * For the linked m68k sizes of D21 step 7's parts (tools/host/tls-test.sh size): one entry per row (-DROW_<NAME>)
 * that calls what that program calls, so --gc-sections keeps exactly that. The C library is a stand-in
 * (tools/host/crypto-size/stubs.c and the STUBS part below), counted apart: on Zenix it is libz.
 *   TLS_GLUE   crypto/zc/zc_tls.c with BearSSL's client, the X.509 wrapper, the anchors reader, the time rules, PEM
 *   WGET_CORE  user/wget/ with net/http/ (no TLS)
 *   WGET_TLS   both: what wget links besides its own main and libz
 *   SNTP_CORE  net/sntp/ with the clock file (zc_clock.c)
 */
#ifdef STUBS                    /* the stand-ins crypto-size/stubs.c does not have */
typedef unsigned long size_t;
int strncmp(const char *a, const char *b, size_t n) { (void)b; (void)n; return *a; }
char *strcat(char *d, const char *s) { (void)s; return d; }
#else
#include "../../crypto/zc/zc_tls.h"
#include "../../crypto/zc/zc_clock.h"
#include "../../user/wget/wget.h"
#include "../../net/http/http.h"
#include "../../net/sntp/sntp.h"

void size_entry(void);
extern volatile unsigned char b[600];         /* (in stubs.c: counted with the stand-ins) */

#if defined(ROW_TLS_GLUE) || defined(ROW_WGET_TLS)
static struct zc_tls t;
static struct zc_tls_cache cache;
static unsigned char iob[ZC_TLS_BUFSIZE], chain[ZC_X509_CHAINMAX], store[16384];
static int rd(void *u, unsigned long off, void *buf, unsigned long n) { (void)u; (void)off; (void)buf; return (int)n; }
static int find(void *u, struct zc_x509 *x, const unsigned char hash[8], const unsigned char *dn, size_t dnlen)
{
    (void)u; (void)dn;
    return zc_anchors_find(x, rd, 0, hash) + zc_x509_add_cert(x, (void *)b, dnlen);
}
static long io_s(void *u, const unsigned char *p, size_t n) { (void)u; (void)p; return (long)n; }
static long io_r(void *u, unsigned char *p, size_t n) { (void)u; (void)p; return (long)n; }
static void tls_part(void)
{
    struct zc_tls_io io = { 0, io_s, io_r };
    struct zc_clock c = { 1, 1, 1, 0 };
    size_t off = 0;
    zc_tls_init(&t, iob, chain, store, sizeof store, find, 0, &cache);
    zc_tls_connect(&t, &io, "x", 443, &c, (void *)b, "http/1.1");
    zc_tls_write(&t, (void *)b, 10);
    zc_tls_read(&t, (void *)b, 10);
    (void)zc_tls_strerror(zc_tls_error(&t));
    (void)zc_tls_alpn(&t);
    zc_tls_close(&t);
    zc_tls_cache_wipe(&cache);
    zc_pem_cert((void *)b, 10, &off, (void *)b, 10);
    zc_clock_parse((void *)b, 10, (void *)b);
}
#endif

#if defined(ROW_WGET_CORE) || defined(ROW_WGET_TLS)
static struct http_resp r;
static void wget_part(void)
{
    struct wget_opts o;
    struct wget_plan p;
    const char *why;
    char *argv[2] = { "wget", "x" };
    const unsigned char *body;
    size_t used, blen;
    int ex;
    char req[1400];
    wget_args(&o, 2, argv, &why);
    wget_plan_start(&p, &o, 0, &why);
    wget_plan_have(&p, 1, 10);
    wget_nextname(p.name, sizeof p.name, p.name, 1);
    http_request(req, sizeof req, &p.url, p.from, 0);
    http_resp_init(&r, 100, 0);
    http_resp_feed(&r, (void *)b, 10, &used, &body, &blen);
    wget_plan_answer(&p, &r, &ex, &why);
    http_resp_eof(&r);
    ex = wget_exit_merge(ex, wget_same_server(&p.url, &p.url));
    b[0] = (unsigned char)ex;
}
#endif

#ifdef ROW_TLS_GLUE
void size_entry(void) { tls_part(); }
#endif
#ifdef ROW_WGET_CORE
void size_entry(void) { wget_part(); }
#endif
#ifdef ROW_WGET_TLS
void size_entry(void) { tls_part(); wget_part(); }
#endif
#ifdef ROW_SNTP_CORE
void size_entry(void)
{
    struct sntp_conf c;
    struct sntp_answer a;
    char line[ZC_CLOCK_LINE];
    uint32_t v;
    sntp_conf_parse(&c, (void *)b, 10, 0, 0);
    sntp_request((void *)b, (void *)b);
    if (sntp_parse(&a, (void *)b, 48, (void *)b) == 0) {
        v = sntp_time(&a, b[1]);
        b[0] = (unsigned char)sntp_judge(v, zc_clock_floor(1, zc_clock_parse((void *)b, 10, &v) == 0, v), 1, 2);
        zc_clock_line(line, v);
        b[1] = (unsigned char)line[3];
    }
}
#endif
#endif
