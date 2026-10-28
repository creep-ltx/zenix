/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit tests of D21 step 7's cores on the PC (tools/host/tls-test.sh units): wget's command line, names, plans and
 * answers (user/wget/), net/http/'s fragment rule, the clock file, the floor and the certificate time callback
 * (crypto/zc/zc_clock.c), the PEM reader (zc_tls.c), SNTP packets, hostile replies, the time, the guard and
 * /etc/sntp (net/sntp/). Every check compares; the last line counts them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/wget/wget.h"
#include "../../net/http/http.h"
#include "../../net/sntp/sntp.h"
#include "../../crypto/zc/zc_clock.h"
#include "../../crypto/zc/zc_tls.h"

static int pass, failed;
#define CHECK(c, ...) do { if (c) pass++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the clock ---- */
static void t_clock(void)
{
    uint32_t v, d, s;
    char b[ZC_CLOCK_LINE];
    struct zc_clock c;
    int n;

    CHECK(zc_clock_parse("zenix-clock 1 1800000000\n", 25, &v) == 0 && v == 1800000000u, "clock: good line");
    CHECK(zc_clock_parse("zenix-clock 1 1800000000", 24, &v) == 0 && v == 1800000000u, "clock: no newline");
    CHECK(zc_clock_parse("zenix-clock 1 0\n", 16, &v) == 0 && v == 0, "clock: zero");
    CHECK(zc_clock_parse("zenix-clock 1 4294967295\n", 25, &v) == 0 && v == 4294967295u, "clock: the largest");
    CHECK(zc_clock_parse("zenix-clock 1 4294967296\n", 25, &v) < 0, "clock: overflow refused");
    CHECK(zc_clock_parse("zenix-clock 1 01\n", 17, &v) < 0, "clock: leading zero refused");
    CHECK(zc_clock_parse("zenix-clock 2 5\n", 16, &v) < 0, "clock: another format refused");
    CHECK(zc_clock_parse("zenix-clock 1 \n", 15, &v) < 0, "clock: no number refused");
    CHECK(zc_clock_parse("zenix-clock 1 12 \n", 18, &v) < 0, "clock: trailing space refused");
    CHECK(zc_clock_parse("zenix-clock 1 12\n\n", 18, &v) < 0, "clock: two newlines refused");
    CHECK(zc_clock_parse("zenix-clock 1 -5\n", 17, &v) < 0, "clock: sign refused");
    CHECK(zc_clock_parse("", 0, &v) < 0 && zc_clock_parse("z", 1, &v) < 0, "clock: empty refused");
    n = zc_clock_line(b, 4294967295u);
    CHECK(n == 25 && !strcmp(b, "zenix-clock 1 4294967295\n"), "clock line: %s", b);
    n = zc_clock_line(b, 0);
    CHECK(n == 16 && zc_clock_parse(b, (size_t)n, &v) == 0 && v == 0, "clock line: zero round trip");
    CHECK(zc_clock_floor(100, 0, 500) == 100 && zc_clock_floor(100, 1, 500) == 500 && zc_clock_floor(900, 1, 500) == 900,
          "clock: the floor is the newer of the two");
    zc_clock_days(0, &d, &s);
    CHECK(d == 719528 && s == 0, "days: the epoch");
    zc_clock_days(4294967295u, &d, &s);     /* 2106-02-07 06:28:15 */
    CHECK(d == 719528 + 49710 && s == 23295, "days: 2106 (%u %u)", d, s);
    /* a certificate valid from day D to day D + 90 */
    {
        uint32_t D = 719528 + 20000;        /* 2024-10-04 */
        uint32_t t_in = 20010u * 86400u, t_before = 19990u * 86400u, t_after = 20100u * 86400u;

        memset(&c, 0, sizeof c);
        c.trusted = 1;
        c.now = t_in;
        CHECK(zc_clock_check(&c, D, 0, D + 90, 0) == 0 && !c.used_floor, "time: valid now");
        c.now = t_before;
        CHECK(zc_clock_check(&c, D, 0, D + 90, 0) == -1, "time: not yet valid");
        c.now = t_after;
        CHECK(zc_clock_check(&c, D, 0, D + 90, 0) == 1, "time: expired");
        c.now = 1000;                       /* a 1970 clock, trusted: the floor stands in */
        c.floor = t_in;
        CHECK(zc_clock_check(&c, D, 0, D + 90, 0) == 0 && c.used_floor, "time: never below the floor");
        c.floor = t_after;
        c.used_floor = 0;
        CHECK(zc_clock_check(&c, D, 0, D + 90, 0) == 1, "time: expired before the floor");
        memset(&c, 0, sizeof c);            /* no trusted clock */
        c.now = 5;
        c.floor = t_before;
        CHECK(zc_clock_check(&c, D, 0, D + 90, 0) == 0 && c.used_floor, "time untrusted: not before skipped");
        c.floor = t_after;
        CHECK(zc_clock_check(&c, D, 0, D + 90, 0) == 1, "time untrusted: expired before the floor is refused");
        c.floor = (D + 90 - 719528) * 86400u;
        CHECK(zc_clock_check(&c, D, 0, D + 90, 0) == 0, "time untrusted: the last second still valid");
        CHECK(zc_clock_check(&c, D, 0, D + 89, 86399) == 1, "time untrusted: one second after");
    }
}

/* ---- SNTP ---- */
static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static const unsigned char nonce[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

static void reply(unsigned char *p, int li, int vn, int mode, int stratum, uint32_t rx, uint32_t tx)
{
    memset(p, 0, SNTP_LEN);
    p[0] = (unsigned char)(li << 6 | vn << 3 | mode);
    p[1] = (unsigned char)stratum;
    memcpy(p + 24, nonce, 8);
    put32(p + 32, rx);
    put32(p + 36, 0x40000000u);         /* .25 s */
    put32(p + 40, tx);
    put32(p + 44, 0x80000000u);         /* .5 s */
}

struct warns {
    int n, line[16];
};

static void warn(void *u, int line, const char *what)
{
    struct warns *w = u;

    (void)what;
    if (w->n < 16)
        w->line[w->n++] = line;
}

static void t_sntp(void)
{
    unsigned char q[SNTP_LEN], p[64];
    struct sntp_answer a;
    struct sntp_conf c;
    struct warns w;
    uint32_t ntp2026 = 1800000000u + 2208988800u;   /* 2027-01-15 */
    int i, zero = 1;

    sntp_request(q, nonce);
    for (i = 1; i < 40; i++)
        zero &= q[i] == 0;
    CHECK(q[0] == 0x23 && zero && !memcmp(q + 40, nonce, 8), "request: LI 0, VN 4, mode 3, only the nonce");
    reply(p, 0, 4, 4, 2, ntp2026, ntp2026 + 1);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_OK && a.tx_secs == 1800000001u && a.rx_secs == 1800000000u &&
          a.stratum == 2 && a.tx_frac == 0x80000000u, "reply: good (%u)", a.tx_secs);
    CHECK(sntp_parse(&a, p, 64, nonce) == SNTP_OK, "reply: with extension bytes");
    CHECK(sntp_parse(&a, p, 47, nonce) == SNTP_ESHORT, "reply: short");
    reply(p, 0, 3, 4, 2, ntp2026, ntp2026);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_OK, "reply: version 3");
    reply(p, 0, 2, 4, 2, ntp2026, ntp2026);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_EVERSION, "reply: version 2 refused");
    reply(p, 0, 4, 3, 2, ntp2026, ntp2026);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_EMODE, "reply: a client's packet refused");
    reply(p, 0, 4, 5, 2, ntp2026, ntp2026);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_EMODE, "reply: broadcast refused");
    reply(p, 0, 4, 4, 2, ntp2026, ntp2026);
    p[31] ^= 1;
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_EORIGIN, "reply: not our nonce");
    reply(p, 0, 4, 4, 0, 0, 0);
    memcpy(p + 12, "DENY", 4);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_EKOD && !strcmp(a.kod, "DENY"), "reply: kiss of death");
    p[24] ^= 1;
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_EORIGIN, "reply: a forged kiss of death (not our nonce) not obeyed");
    reply(p, 0, 4, 4, 0, 0, 0);
    memcpy(p + 12, "\x1b[2J", 4);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_EKOD && !strcmp(a.kod, "?[2J"), "reply: the code cleaned (%s)", a.kod);
    reply(p, 0, 4, 4, 16, ntp2026, ntp2026);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_ESTRATUM, "reply: stratum 16");
    reply(p, 0, 4, 4, 255, ntp2026, ntp2026);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_ESTRATUM, "reply: stratum 255");
    reply(p, 3, 4, 4, 1, ntp2026, ntp2026);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_ELEAP, "reply: unsynchronised (LI 3)");
    reply(p, 1, 4, 4, 1, ntp2026, ntp2026);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_OK, "reply: a leap second announced is fine");
    reply(p, 0, 4, 4, 1, ntp2026, 0);
    put32(p + 44, 0);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_ETIME, "reply: no transmit time");
    reply(p, 0, 4, 4, 1, ntp2026, 0x83AA7E7Fu);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_ETIME, "reply: one second before 1970");
    reply(p, 0, 4, 4, 1, 0x83AA7E80u, 0x83AA7E80u);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_OK && a.tx_secs == 0, "reply: 1970 exactly");
    reply(p, 0, 4, 4, 1, 5, 5);         /* era 1: 2036-02-07 06:28:21 */
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_OK && a.tx_secs == 2085978501u, "reply: after 2036 (%u)", a.tx_secs);
    reply(p, 0, 4, 4, 1, 0x7FFFFFFFu, 0x7FFFFFFFu);
    CHECK(sntp_parse(&a, p, 48, nonce) == SNTP_OK && a.tx_secs == 4233462143u, "reply: 2104, the last of era 1");
    /* the time: transmit .5 s, receive .25 s (the server's 1.25 s), a round trip of 2,250 ms: delay 1,000 ms */
    reply(p, 0, 4, 4, 2, ntp2026, ntp2026 + 1);
    sntp_parse(&a, p, 48, nonce);
    CHECK(sntp_time(&a, 2250) == 1800000002u, "time: .5 + .5 rounds up (%u)", sntp_time(&a, 2250));
    CHECK(sntp_time(&a, 1250) == 1800000002u, "time: no delay: .5 rounds up");
    CHECK(sntp_time(&a, 0) == 1800000002u, "time: a server slower than the round trip: no negative delay");
    put32(p + 44, 0x20000000u);         /* .125 s */
    sntp_parse(&a, p, 48, nonce);
    CHECK(sntp_time(&a, 1000) == 1800000001u, "time: .125 rounds down");
    CHECK(sntp_time(&a, 4000) == 1800000003u, "time: 3,125 ms of delay: 1.125 + 1.5625 rounds to 3");
    a.tx_secs = 0xFFFFFFFFu;
    CHECK(sntp_time(&a, 100000) == 0xFFFFFFFFu, "time: no wrap past 2106");
    /* the guard */
    CHECK(sntp_judge(999, 1000, 0, 0) == 0, "judge: before the floor refused");
    CHECK(sntp_judge(1000 + 400u * 86400u, 1000, 0, 0) == (SNTP_SET | SNTP_RAISE), "judge: 400 days ahead raises");
    CHECK(sntp_judge(1001 + 400u * 86400u, 1000, 0, 0) == SNTP_SET, "judge: more than 400 days only sets");
    CHECK(sntp_judge(5000, 1000, 1, 5600) == (SNTP_SET | SNTP_RAISE), "judge: 10 minutes from the battery clock raises");
    CHECK(sntp_judge(5000, 1000, 1, 5601) == SNTP_SET, "judge: 10 minutes and a second only sets");
    CHECK(sntp_judge(6200, 1000, 1, 5600) == (SNTP_SET | SNTP_RAISE), "judge: ahead of the battery clock");
    CHECK(sntp_judge(0xFFFFFFFFu, 1000, 1, 5000) == SNTP_SET, "judge: 2106 against a battery clock only sets");
    /* /etc/sntp */
    memset(&w, 0, sizeof w);
    {
        static const char f[] = "# the servers\nserver a.example\n  server\tb.example  # second\r\nfoo bar\n"
                                "server\nserver c.example extra\nserver bad_name\nserver d.example\nserver e.example\n"
                                "servers x\n\nserver f\0ignored\n";

        CHECK(sntp_conf_parse(&c, f, sizeof f - 1, warn, &w) == 4 && !strcmp(c.host[0], "a.example") &&
              !strcmp(c.host[1], "b.example") && !strcmp(c.host[2], "c.example") && !strcmp(c.host[3], "d.example"),
              "conf: four servers (%d)", c.n);
        CHECK(w.n == 7 && w.line[0] == 4 && w.line[1] == 5 && w.line[2] == 6 && w.line[3] == 7 && w.line[4] == 9 &&
              w.line[5] == 10 && w.line[6] == 12, "conf: the warnings (%d: %d %d %d %d %d %d %d)", w.n, w.line[0],
              w.line[1], w.line[2], w.line[3], w.line[4], w.line[5], w.line[6]);
    }
    CHECK(sntp_conf_parse(&c, "", 0, 0, 0) == 0, "conf: empty");
    CHECK(sntp_conf_parse(&c, "server x.y", 10, 0, 0) == 1 && !strcmp(c.host[0], "x.y"), "conf: no final newline");
}

/* ---- wget ---- */
static int args(struct wget_opts *o, const char *line)
{
    static char buf[512];
    char *v[32];
    int n = 0;
    const char *why;

    strcpy(buf, line);
    v[n++] = "wget";
    for (char *t = strtok(buf, " "); t; t = strtok(0, " "))
        v[n++] = t;
    return wget_args(o, n, v, &why);
}

static int name_of(const char *url, char *out)
{
    struct http_url u;

    if (http_url_parse(&u, url))
        return -1;
    return wget_name(out, WGET_NAMEMAX + 1, &u);
}

/* feed a response's headers; the status of wget_plan_answer */
static int answer(struct wget_plan *p, const char *resp, int *ex)
{
    static struct http_resp r;
    const unsigned char *body;
    size_t used, blen;
    const char *why;
    int rc;

    http_resp_init(&r, 0xFFFFFFFFul, 0);
    rc = http_resp_feed(&r, (const unsigned char *)resp, strlen(resp), &used, &body, &blen);
    if (rc != HTTP_HEAD) {
        *ex = -100 + rc;
        return -1;
    }
    return wget_plan_answer(p, &r, ex, &why);
}

static void t_wget(void)
{
    struct wget_opts o;
    struct wget_plan p;
    struct http_url u;
    char n[WGET_NAMEMAX + 1], big[2048];
    const char *why;
    int ex;

    CHECK(args(&o, "http://a/x") == 0 && o.nurl == 1 && !o.quiet && !o.cont && !o.output, "args: one URL");
    CHECK(args(&o, "-q -c -O out http://a/x http://b/y") == 0 && o.quiet && o.cont && !strcmp(o.output, "out") &&
          o.nurl == 2, "args: -q -c -O");
    CHECK(args(&o, "-qcOout http://a/x") == 0 && o.quiet && o.cont && !strcmp(o.output, "out"), "args: -qcOout");
    CHECK(args(&o, "-qO - http://a/x") == 0 && !strcmp(o.output, "-"), "args: -O -");
    CHECK(args(&o, "http://a/x -q") == 0 && o.quiet, "args: options after the URL");
    CHECK(args(&o, "-- -q") == 0 && o.nurl == 1 && !strcmp(o.url[0], "-q") && !o.quiet, "args: --");
    CHECK(args(&o, "--quiet --continue --output-document=f http://a/") == 0 && o.quiet && o.cont &&
          !strcmp(o.output, "f"), "args: long forms");
    CHECK(args(&o, "-x http://a/") == WGET_EXIT_USAGE, "args: unknown option");
    CHECK(args(&o, "--spider http://a/") == WGET_EXIT_USAGE, "args: unknown long option");
    CHECK(args(&o, "-q") == WGET_EXIT_USAGE, "args: no URL");
    CHECK(args(&o, "http://a/ -O") == WGET_EXIT_USAGE, "args: -O without a name");
    CHECK(args(&o, "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16") == 0 && args(&o, "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17")
          == WGET_EXIT_USAGE, "args: at most 16 URLs");
    /* names */
    CHECK(name_of("http://h", n) == 0 && !strcmp(n, "index.html"), "name: no path");
    CHECK(name_of("http://h/", n) == 0 && !strcmp(n, "index.html"), "name: /");
    CHECK(name_of("http://h/d/", n) == 0 && !strcmp(n, "index.html"), "name: a directory");
    CHECK(name_of("http://h/a/b.tar.gz", n) == 0 && !strcmp(n, "b.tar.gz"), "name: the last segment");
    CHECK(name_of("https://h:8443/a/b?x=1&y=/z", n) == 0 && !strcmp(n, "b?x=1&y=%2Fz"), "name: the query kept (%s)", n);
    CHECK(name_of("http://h/?q", n) == 0 && !strcmp(n, "index.html?q"), "name: a directory's query (%s)", n);
    CHECK(name_of("http://h/a%20b%2Fc%00d%41%zz%4", n) == 0 && !strcmp(n, "a b%2Fc%00dA%zz%4"), "name: %%XX (%s)", n);
    CHECK(name_of("http://h/x/..", n) == 0 && !strcmp(n, "index.html"), "name: ..");
    CHECK(name_of("http://h/x/%2E%2e", n) == 0 && !strcmp(n, "index.html"), "name: %%2E%%2E");
    CHECK(name_of("http://h/x/.", n) == 0 && !strcmp(n, "index.html"), "name: .");
    CHECK(name_of("http://h/a#frag", n) == 0 && !strcmp(n, "a"), "name: no fragment");
    memset(big, 'x', 600);
    memcpy(big, "http://h/", 9);
    big[600] = 0;
    CHECK(name_of(big, n) == 0 && strlen(n) == WGET_NAMEMAX - 10, "name: cut to leave room for .N (%zu)", strlen(n));
    CHECK(wget_nextname(n, sizeof n, "f.txt", 1) == 0 && !strcmp(n, "f.txt.1"), "nextname: .1");
    CHECK(wget_nextname(n, sizeof n, "f", 12345) == 0 && !strcmp(n, "f.12345"), "nextname: .12345");
    memset(big, 'y', 254);
    big[254] = 0;
    CHECK(wget_nextname(n, sizeof n, big, 1) != 0, "nextname: too long");
    /* the fragment is never sent */
    CHECK(http_url_parse(&u, "http://h/p?q#f") == 0 && !strcmp(u.path, "/p?q"), "url: fragment dropped (%s)", u.path);
    {
        int hops = 0;

        CHECK(http_redirect(&u, "/n#x", &hops) == 0 && !strcmp(u.path, "/n"), "redirect: fragment dropped");
    }
    /* relative Locations (RFC 3986 5.2, its 5.4.1 examples with the base http://a/b/c/d;p?q) */
    {
        static const char *const ex[][2] = {
            { "g", "/b/c/g" }, { "./g", "/b/c/g" }, { "g/", "/b/c/g/" }, { "/g", "/g" }, { "?y", "/b/c/d;p?y" },
            { "g?y", "/b/c/g?y" }, { "#s", "/b/c/d;p?q" }, { "g#s", "/b/c/g" }, { "g?y#s", "/b/c/g?y" },
            { ";x", "/b/c/;x" }, { "g;x", "/b/c/g;x" }, { ".", "/b/c/" }, { "./", "/b/c/" }, { "..", "/b/" },
            { "../", "/b/" }, { "../g", "/b/g" }, { "../..", "/" }, { "../../", "/" }, { "../../g", "/g" },
            { "../../../g", "/g" }, { "../../../../g", "/g" }, { "/./g", "/g" }, { "/../g", "/g" }, { "g.", "/b/c/g." },
            { ".g", "/b/c/.g" }, { "g..", "/b/c/g.." }, { "..g", "/b/c/..g" }, { "./../g", "/b/g" }, { "./g/.", "/b/c/g/" },
            { "g/./h", "/b/c/g/h" }, { "g/../h", "/b/c/h" }, { "g;x=1/./y", "/b/c/g;x=1/y" }, { "g;x=1/../y", "/b/c/y" },
            { "g?y/./x", "/b/c/g?y/./x" }, { "g?y/../x", "/b/c/g?y/../x" }, { "", "/b/c/d;p?q" },
        };
        unsigned i;

        for (i = 0; i < sizeof ex / sizeof ex[0]; i++) {
            int hops = 0;

            http_url_parse(&u, "http://a/b/c/d;p?q");
            CHECK(http_redirect_rel(&u, ex[i][0], &hops) == 0 && !strcmp(u.path, ex[i][1]) && !strcmp(u.host, "a") &&
                  hops == 1, "relative: \"%s\" -> %s (%s)", ex[i][0], ex[i][1], u.path);
        }
        {
            int hops = 0;

            http_url_parse(&u, "https://a/b");
            CHECK(http_redirect_rel(&u, "//g:81/x/../y", &hops) == 0 && u.tls && !strcmp(u.host, "g") && u.port == 81 &&
                  !strcmp(u.path, "/y"), "relative: //host keeps https (%s %s)", u.host, u.path);
            CHECK(http_redirect_rel(&u, "//g", &hops) == 0 && u.port == 443 && !strcmp(u.path, "/"),
                  "relative: //host without a port: the scheme's");
            CHECK(http_redirect_rel(&u, "x", &hops) == 0 && http_redirect_rel(&u, "y", &hops) == HTTP_ELOOP,
                  "relative: the fourth hop refused");
            hops = 0;
            CHECK(http_redirect_rel(&u, "http://a/x", &hops) == HTTP_EPERM, "relative: an absolute https to http refused");
            CHECK((hops = 0, http_redirect_rel(&u, "mailto:x@y", &hops)) == HTTP_EINVAL, "relative: another scheme refused");
            CHECK((hops = 0, http_redirect_rel(&u, "//bad_host/x", &hops)) == HTTP_EINVAL, "relative: a bad //host refused");
            CHECK((hops = 0, http_redirect_rel(&u, "a b", &hops)) == HTTP_EINVAL, "relative: a space refused");
            http_url_parse(&u, "http://a/b");
            memset(big, 'z', 1100);
            big[1100] = 0;
            hops = 0;
            CHECK(http_redirect_rel(&u, big, &hops) == HTTP_EINVAL && !strcmp(u.path, "/b"),
                  "relative: too long refused, the URL unchanged");
            hops = 0;
            CHECK(http_redirect(&u, "x", &hops) == HTTP_EPERM && http_redirect(&u, "//k/x", &hops) == HTTP_EPERM,
                  "http_redirect (zpkg's) still refuses relative and //host");
        }
    }
    /* plans */
    args(&o, "http://h/d/f.bin");
    CHECK(wget_plan_start(&p, &o, 0, &why) == 0 && !strcmp(p.name, "f.bin") && p.need_free_name && !p.need_size &&
          !p.append && !p.from, "plan: a plain fetch");
    CHECK(answer(&p, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", &ex) == WGET_WRITE && !p.append && p.total == 5,
          "plan: 200 writes");
    CHECK(answer(&p, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", &ex) == WGET_FAIL && ex == WGET_EXIT_SERVER,
          "plan: 404 exit 8");
    CHECK(answer(&p, "HTTP/1.1 500 X\r\n\r\n", &ex) == WGET_FAIL && ex == WGET_EXIT_SERVER, "plan: 500 exit 8");
    CHECK(answer(&p, "HTTP/1.1 304 X\r\n\r\n", &ex) == WGET_FAIL && ex == WGET_EXIT_PROTO, "plan: 304 not taken");
    CHECK(answer(&p, "HTTP/1.1 206 P\r\nContent-Range: bytes 0-4/5\r\nContent-Length: 5\r\n\r\n", &ex) == WGET_FAIL &&
          ex == WGET_EXIT_PROTO, "plan: an unasked 206 refused");
    /* redirects: at most 3, never https to http, relative not followed */
    args(&o, "https://h/a");
    wget_plan_start(&p, &o, 0, &why);
    CHECK(answer(&p, "HTTP/1.1 301 M\r\nLocation: https://k:444/b\r\n\r\n", &ex) == WGET_FOLLOW && p.url.tls &&
          !strcmp(p.url.host, "k") && p.url.port == 444 && !strcmp(p.name, "a"), "redirect 1: another host, name kept");
    CHECK(answer(&p, "HTTP/1.1 302 F\r\nLocation: /c\r\n\r\n", &ex) == WGET_FOLLOW && !strcmp(p.url.host, "k") &&
          !strcmp(p.url.path, "/c"), "redirect 2: a path");
    CHECK(answer(&p, "HTTP/1.1 308 P\r\nLocation: https://k/e\r\n\r\n", &ex) == WGET_FOLLOW, "redirect 3");
    CHECK(answer(&p, "HTTP/1.1 302 F\r\nLocation: https://k/f\r\n\r\n", &ex) == WGET_FAIL && ex == WGET_EXIT_PROTO,
          "redirect 4: more than 3 refused");
    wget_plan_start(&p, &o, 0, &why);
    CHECK(answer(&p, "HTTP/1.1 307 T\r\nLocation: http://k/d\r\n\r\n", &ex) == WGET_FAIL && ex == WGET_EXIT_PROTO,
          "redirect: https to http refused");
    wget_plan_start(&p, &o, 0, &why);
    args(&o, "https://h/d/e/a?q=1");
    wget_plan_start(&p, &o, 0, &why);
    CHECK(answer(&p, "HTTP/1.1 302 F\r\nLocation: x/y\r\n\r\n", &ex) == WGET_FOLLOW && !strcmp(p.url.path, "/d/e/x/y")
          && !strcmp(p.name, "a?q=1"), "redirect: a relative Location followed (%s), the name kept", p.url.path);
    CHECK(answer(&p, "HTTP/1.1 302 F\r\nLocation: //k:8443/y\r\n\r\n", &ex) == WGET_FOLLOW && p.url.tls &&
          !strcmp(p.url.host, "k") && p.url.port == 8443 && !strcmp(p.url.path, "/y"), "redirect: //host followed");
    wget_plan_start(&p, &o, 0, &why);
    CHECK(answer(&p, "HTTP/1.1 302 F\r\nLocation: ftp://k/y\r\n\r\n", &ex) == WGET_FAIL && ex == WGET_EXIT_PROTO,
          "redirect: another scheme refused");
    CHECK(answer(&p, "HTTP/1.1 302 F\r\n\r\n", &ex) == WGET_FAIL, "redirect: no Location");
    args(&o, "http://h/a");
    wget_plan_start(&p, &o, 0, &why);
    CHECK(answer(&p, "HTTP/1.1 302 F\r\nLocation: https://h/a\r\n\r\n", &ex) == WGET_FOLLOW && p.url.tls &&
          p.url.port == 443, "redirect: http to https followed");
    /* -c */
    args(&o, "-c http://h/f");
    wget_plan_start(&p, &o, 0, &why);
    CHECK(p.need_size && !p.need_free_name, "resume: the size is asked, no new name");
    wget_plan_have(&p, 0, 0);
    CHECK(p.from == 0, "resume: no file: from the start");
    wget_plan_have(&p, 1, 0);
    CHECK(p.from == 0, "resume: an empty file: from the start");
    wget_plan_have(&p, 1, 100);
    CHECK(p.from == 100, "resume: from 100");
    CHECK(answer(&p, "HTTP/1.1 206 P\r\nContent-Range: bytes 100-199/200\r\nContent-Length: 100\r\n\r\n", &ex) ==
          WGET_WRITE && p.append && p.total == 200, "resume: 206 appends");
    wget_plan_start(&p, &o, 0, &why);
    wget_plan_have(&p, 1, 100);
    CHECK(answer(&p, "HTTP/1.1 206 P\r\nContent-Range: bytes 99-199/200\r\n\r\n", &ex) == WGET_FAIL &&
          ex == WGET_EXIT_PROTO, "resume: a wrong first byte refused");
    CHECK(answer(&p, "HTTP/1.1 206 P\r\nContent-Range: bytes 100-150/200\r\n\r\n", &ex) == WGET_FAIL,
          "resume: not to the end refused");
    CHECK(answer(&p, "HTTP/1.1 206 P\r\nContent-Range: bytes 100-199/200\r\nContent-Length: 99\r\n\r\n", &ex) ==
          WGET_FAIL, "resume: a length that does not fit refused");
    CHECK(answer(&p, "HTTP/1.1 206 P\r\nContent-Length: 100\r\n\r\n", &ex) == WGET_FAIL, "resume: 206 without a range");
    CHECK(answer(&p, "HTTP/1.1 200 OK\r\nContent-Length: 300\r\n\r\n", &ex) == WGET_FAIL && ex == WGET_EXIT_GENERIC,
          "resume: a 200 is refused (never truncate with -c), exit 1");
    wget_plan_start(&p, &o, 0, &why);
    wget_plan_have(&p, 1, 200);
    CHECK(answer(&p, "HTTP/1.1 416 R\r\nContent-Range: bytes */200\r\nContent-Length: 0\r\n\r\n", &ex) == WGET_DONE,
          "resume: 416 with bytes */200: already complete");
    CHECK(answer(&p, "HTTP/1.1 416 R\r\nContent-Range: bytes */x\r\n\r\n", &ex) < 0, "416: a bad */total refused");
    CHECK(answer(&p, "HTTP/1.1 416 R\r\nContent-Length: 0\r\n\r\n", &ex) == WGET_DONE && ex == 0, "resume: 416 done");
    wget_plan_start(&p, &o, 0, &why);
    CHECK(answer(&p, "HTTP/1.1 416 R\r\nContent-Length: 0\r\n\r\n", &ex) == WGET_FAIL, "416 unasked is an error");
    /* -O */
    args(&o, "-O out http://h/a http://h/b");
    CHECK(wget_plan_start(&p, &o, 0, &why) == 0 && !strcmp(p.name, "out") && !p.append && !p.need_free_name &&
          !p.need_size, "-O: the first URL truncates");
    CHECK(wget_plan_start(&p, &o, 1, &why) == 0 && !strcmp(p.name, "out") && p.append, "-O: later ones append");
    args(&o, "-c -O out http://h/a http://h/b");
    CHECK(wget_plan_start(&p, &o, 0, &why) == 0 && p.need_size, "-c -O: the first resumes");
    CHECK(wget_plan_start(&p, &o, 1, &why) == 0 && !p.need_size && p.append, "-c -O: later ones append");
    args(&o, "-c -O - http://h/a");
    CHECK(wget_plan_start(&p, &o, 0, &why) == 0 && p.to_stdout && !p.need_size, "-O -: standard output never resumes");
    args(&o, "ftp://h/a");
    CHECK(wget_plan_start(&p, &o, 0, &why) == WGET_EXIT_USAGE, "plan: ftp refused");
    CHECK(wget_plan_start(&p, &o, 1, &why) == WGET_EXIT_USAGE, "plan: no such URL");
    /* servers and exit statuses */
    {
        struct http_url a, b;

        http_url_parse(&a, "https://Host.Example/x");
        http_url_parse(&b, "https://host.example:443/y");
        CHECK(wget_same_server(&a, &b), "same server: case and the default port");
        http_url_parse(&b, "http://host.example:443/y");
        CHECK(!wget_same_server(&a, &b), "same server: not across schemes");
        http_url_parse(&b, "https://host.example:444/");
        CHECK(!wget_same_server(&a, &b), "same server: not across ports");
    }
    CHECK(wget_exit_merge(0, 8) == 8 && wget_exit_merge(8, 4) == 4 && wget_exit_merge(1, 8) == 8 &&
          wget_exit_merge(5, 1) == 5 && wget_exit_merge(1, 0) == 1 && wget_exit_merge(0, 0) == 0, "exit: precedence");
}

/* ---- PEM ---- */
static void b64(const unsigned char *d, size_t n, char *out)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, k = 0, col = 0;

    for (i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);

        out[k++] = a[v >> 18 & 63];
        out[k++] = a[v >> 12 & 63];
        out[k++] = i + 1 < n ? a[v >> 6 & 63] : '=';
        out[k++] = i + 2 < n ? a[v & 63] : '=';
        if ((col += 4) == 64) {
            out[k++] = '\n';
            col = 0;
        }
    }
    out[k++] = '\n';
    out[k] = 0;
}

static void t_pem(void)
{
    static char pem[8192], enc[4096];
    unsigned char d1[300], d2[40], out[512];
    size_t off = 0, i;
    long k;

    for (i = 0; i < sizeof d1; i++)
        d1[i] = (unsigned char)(i * 7);
    for (i = 0; i < sizeof d2; i++)
        d2[i] = (unsigned char)(255 - i);
    strcpy(pem, "junk before\n-----BEGIN CERTIFICATE-----\n");
    b64(d1, sizeof d1, enc);
    strcat(pem, enc);
    strcat(pem, "-----END CERTIFICATE-----\n-----BEGIN " "PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n"
                "-----BEGIN CERTIFICATE-----\n");
    b64(d2, sizeof d2, enc);
    strcat(pem, enc);
    strcat(pem, "-----END CERTIFICATE-----\ntrailing\n");
    k = zc_pem_cert(pem, strlen(pem), &off, out, sizeof out);
    CHECK(k == (long)sizeof d1 && !memcmp(out, d1, sizeof d1), "pem: the first certificate (%ld)", k);
    k = zc_pem_cert(pem, strlen(pem), &off, out, sizeof out);
    CHECK(k == (long)sizeof d2 && !memcmp(out, d2, sizeof d2), "pem: the second, the key skipped (%ld)", k);
    CHECK(zc_pem_cert(pem, strlen(pem), &off, out, sizeof out) == 0, "pem: none left");
    off = 0;
    CHECK(zc_pem_cert(pem, strlen(pem), &off, out, 100) == -1, "pem: too large refused");
    off = 0;
    CHECK(zc_pem_cert(pem, 60, &off, out, sizeof out) == -1, "pem: cut in the middle");
    CHECK(strcmp(zc_tls_strerror(BR_ERR_RECV_FATAL_ALERT + 70), "this server needs TLS 1.3, which Zenix does not speak yet") == 0,
          "strerror: TLS 1.3");
}

int main(void)
{
    t_clock();
    t_sntp();
    t_wget();
    t_pem();
    printf("tls-core-test: %d checks passed, %d failed\n", pass, failed);
    return failed != 0;
}
