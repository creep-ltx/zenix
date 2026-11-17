/* SPDX-License-Identifier: GPL-2.0-only */
/* mail's core on the PC (DESIGN D59 section 8, tools/host/mail-test.sh "sessions"): the protocols over sockets, plain
 * or TLS (the crypto library's glue, as tools/host/irc-client.c; also after STARTTLS), driven by a script against
 * tools/host/mail-fake.py. Each step prints what came back as "!! ..." lines; the text of a message as it would be
 * shown. Exit 1 when a step's connection cannot be made.
 *
 *   mail-client [--anchors FILE] [--name HOST] SCRIPT
 *
 * The script, a step a line:
 *   open imap|pop PORT on|starttls|off         connect for reading (the next steps use it)
 *   login USER PASS [plain]                    "!! login R ERR"
 *   folders                                    "!! box NAME = SHOWN" each
 *   select NAME LIMIT                          "!! select R EXISTS NM" and "!! item I UID FLAGS DATE FROM | SUBJECT"
 *   fetch I                                    "!! fetch R cut C n N", the headers, "!! text" lines, "!! att ..."
 *   save I K FILE                              attachment K of message I, decoded, into FILE
 *   flag I seen|deleted|answered 0|1, copy I FOLDER, expunge, append FOLDER FILE, logout
 *   send PORT on|starttls|off USER PASS FILE   FILE (the editor's text) built and sent: "!! send R ERR"
 *   build FILE OUT                             FILE built into OUT (the bytes sent), "!! build R ERR" */
#include "../../net/mail/mail.h"
#include "../../crypto/zc/zc_tls.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char *name = "rsa.test";
static FILE *anchors;

struct hc { int fd, tls, port; struct zc_tls T; unsigned char iobuf[ZC_TLS_BUFSIZE], chain[ZC_X509_CHAINMAX], store[16384]; };

static int rd(void *u, unsigned long off, void *buf, unsigned long n)
{
    (void)u;
    return anchors && !fseek(anchors, (long)off, SEEK_SET) && fread(buf, 1, n, anchors) == n ? 0 : -1;
}
static int find(void *u, struct zc_x509 *x, const unsigned char hash[8], const unsigned char *dn, size_t dnlen)
{
    (void)u; (void)dn; (void)dnlen;
    return anchors ? zc_anchors_find(x, rd, 0, hash) : 0;
}
static long tsend(void *u, const unsigned char *p, size_t n) { struct hc *h = u; long k = send(h->fd, p, n, MSG_NOSIGNAL); return k < 0 ? -errno : k; }
static long trecv(void *u, unsigned char *p, size_t n)
{
    struct hc *h = u;
    struct pollfd q = { h->fd, POLLIN, 0 };
    if (poll(&q, 1, 10000) <= 0) return -ETIMEDOUT;
    long k = recv(h->fd, p, n, 0);
    return k < 0 ? -errno : k;
}

static int c_rd(void *u, char *b, int n)
{
    struct hc *h = u;
    if (h->tls) {
        long k = zc_tls_read(&h->T, b, (size_t)n);
        return k > 0 ? (int)k : k == 0 ? 0 : -1;
    }
    struct pollfd q = { h->fd, POLLIN, 0 };
    if (poll(&q, 1, 10000) <= 0) return -1;
    long k = recv(h->fd, b, (size_t)n, 0);
    return k > 0 ? (int)k : k == 0 ? 0 : -1;
}
static int c_wr(void *u, const char *b, int n)
{
    struct hc *h = u;
    if (h->tls) return zc_tls_write(&h->T, b, (size_t)n) < 0 ? -1 : 0;
    return send(h->fd, b, (size_t)n, MSG_NOSIGNAL) == n ? 0 : -1;
}
static int c_tls(void *u, char *err, int errn)
{
    struct hc *h = u;
    static struct zc_clock clk;
    unsigned char seed[32];
    struct zc_tls_io io = { h, tsend, trecv };
    for (int i = 0; i < 32; i++) seed[i] = (unsigned char)rand();
    clk.trusted = 1;
    clk.now = (u32)time(0);
    clk.floor = clk.now - 86400;
    zc_tls_init(&h->T, h->iobuf, h->chain, h->store, sizeof h->store, find, 0, 0);
    int r = zc_tls_connect(&h->T, &io, name, (unsigned)h->port, &clk, seed, 0);
    if (r < 0) {
        snprintf(err, (size_t)errn, "TLS: %s", zc_tls_strerror(r == ZC_TLS_EENGINE ? zc_tls_error(&h->T) : r));
        return -1;
    }
    h->tls = 1;
    printf("!! TLS ok\n");
    return 0;
}

static int tlsmode(const char *s) { return !strcmp(s, "on") ? TLS_ON : !strcmp(s, "starttls") ? TLS_STARTTLS : TLS_OFF; }

static int hopen(struct hc *h, struct mconn *m, int port, int tls)
{
    struct sockaddr_in a;
    memset(h, 0, sizeof *h);
    memset(m, 0, sizeof *m);
    m->u = h;
    m->rd = c_rd;
    m->wr = c_wr;
    m->starttls = c_tls;
    h->port = port;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    h->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (h->fd < 0 || connect(h->fd, (struct sockaddr *)&a, sizeof a) < 0) {
        printf("!! connect: %s\n", strerror(errno));
        return -1;
    }
    char err[200];
    if (tls == TLS_ON && c_tls(h, err, sizeof err) < 0) {
        printf("!! open %s\n", err);
        close(h->fd);
        return -1;
    }
    return 0;
}

static char *slurp(const char *p, unsigned *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char *b = malloc(4 << 20);
    *n = (unsigned)fread(b, 1, (4 << 20) - 1, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

static void lines(const char *pre, const char *t)
{
    while (*t) {
        const char *e = strchr(t, '\n');
        int n = e ? (int)(e - t) : (int)strlen(t);
        printf("%s%.*s\n", pre, n, t);
        t += n + (e ? 1 : 0);
    }
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        if (!strcmp(argv[i], "--anchors")) anchors = fopen(argv[++i], "rb");
        else if (!strcmp(argv[i], "--name")) name = argv[++i];
    }
    if (argc - i != 1) {
        fprintf(stderr, "usage: mail-client [--anchors FILE] [--name HOST] SCRIPT\n");
        return 2;
    }
    setvbuf(stdout, 0, _IOLBF, 0);
    FILE *sc = fopen(argv[i], "r");
    if (!sc) return 2;
    static struct hc H, O;
    struct mconn M, OM;
    struct msess S;
    memset(&S, 0, sizeof S);
    int tls = 0;
    char line[1200], a1[300], a2[300], a3[300], a4[300];
    char *raw = 0;
    struct mmsg pm;
    memset(&pm, 0, sizeof pm);
    while (fgets(line, sizeof line, sc)) {
        line[strcspn(line, "\n")] = 0;
        int x = 0, y = 0;
        if (sscanf(line, "open %299s %d %299s", a1, &x, a2) == 3) {
            tls = tlsmode(a2);
            if (hopen(&H, &M, x, tls) < 0) return 1;
            ms_free(&S);
            memset(&S, 0, sizeof S);
            S.c = &M;
            S.pop = !strcmp(a1, "pop");
            printf("!! open ok\n");
        } else if (sscanf(line, "login %299s %299s %299s", a1, a2, a3) >= 2) {
            int plain = strstr(line, " plain") != 0;
            int r = ms_login(&S, tls, a1, a2, plain);
            printf("!! login %d %s\n", r, r < 0 ? S.err : "");
        } else if (!strcmp(line, "folders")) {
            int r = ms_folders(&S);
            printf("!! folders %d %d %s\n", r, S.nbox, r < 0 ? S.err : "");
            for (int k = 0; k < S.nbox; k++) printf("!! box %s = %s\n", S.box[k].name, S.box[k].shown);
        } else if (sscanf(line, "select %299s %d", a1, &x) == 2) {
            int r = ms_select(&S, a1, x);
            printf("!! select %d %d %d %s\n", r, S.exists, S.nm, r < 0 ? S.err : "");
            for (int k = 0; k < S.nm; k++)
                printf("!! item %d %u %d %u %s | %s\n", k, S.m[k].uid, S.m[k].flags, S.m[k].date, S.m[k].from, S.m[k].subj);
        } else if (sscanf(line, "fetch %d", &x) == 1) {
            unsigned n;
            int cut;
            free(raw);
            mime_free(&pm);
            int r = ms_fetch(&S, x, &raw, &n, &cut);
            printf("!! fetch %d cut %d n %u %s\n", r, cut, n, r < 0 ? S.err : "");
            if (r < 0) continue;
            if (mime_parse(&pm, raw, n) < 0) { printf("!! parse out of memory\n"); continue; }
            printf("!! from %s\n!! to %s\n!! cc %s\n!! subject %s\n!! date %s\n!! replyto %s\n!! html %d\n", pm.from, pm.to,
                   pm.cc, pm.subject, pm.date, pm.replyto, pm.html);
            lines("!! text ", pm.text);
            for (int k = 0; k < pm.natt; k++)
                printf("!! att %d %s | %s | %u\n", k, pm.att[k].name, pm.att[k].type, pm.att[k].size);
        } else if (sscanf(line, "save %d %d %299s", &x, &y, a1) == 3) {
            unsigned n;
            if (y < 0 || y >= pm.natt) { printf("!! save no such\n"); continue; }
            char *d = mime_decode_part(&pm.att[y], &n);
            FILE *f = fopen(a1, "wb");
            if (f) { fwrite(d, 1, n, f); fclose(f); }
            free(d);
            printf("!! save %u\n", n);
        } else if (sscanf(line, "flag %d %299s %d", &x, a1, &y) == 3) {
            int f = !strcmp(a1, "seen") ? MF_SEEN : !strcmp(a1, "deleted") ? MF_DELETED : MF_ANSWERED;
            int r = ms_flag(&S, x, f, y);
            printf("!! flag %d %s\n", r, r < 0 ? S.err : "");
        } else if (sscanf(line, "copy %d %299s", &x, a1) == 2) {
            int r = ms_copy(&S, x, a1);
            printf("!! copy %d %s\n", r, r < 0 ? S.err : "");
        } else if (!strcmp(line, "expunge")) {
            int r = ms_expunge(&S);
            printf("!! expunge %d %s\n", r, r < 0 ? S.err : "");
        } else if (sscanf(line, "append %299s %299s", a1, a2) == 2) {
            unsigned n;
            char *t = slurp(a2, &n);
            int r = t ? ms_append(&S, a1, t, n) : -9;
            printf("!! append %d %s\n", r, r < 0 ? S.err : "");
            free(t);
        } else if (!strcmp(line, "logout")) {
            ms_logout(&S);
            printf("!! logout\n");
        } else if (sscanf(line, "send %d %299s %299s %299s %299s", &x, a1, a2, a3, a4) == 5 ||
                   sscanf(line, "build %299s %299s", a4, a3) == 2) {
            int build = !strncmp(line, "build", 5);
            unsigned n, mn;
            char *t = slurp(a4, &n), *rc[60], rb[3000], from[300], err[200];
            int nr;
            char *m = t ? mail_build(t, (u32)time(0), 0x5eed, &mn, rc, &nr, rb, sizeof rb, from, sizeof from, err, sizeof err) : 0;
            free(t);
            if (!m) { printf("!! %s -1 %s\n", build ? "build" : "send", err); continue; }
            if (build) {
                FILE *f = fopen(a3, "wb");
                fwrite(m, 1, mn, f);
                fclose(f);
                printf("!! build 0 %d recipients\n", nr);
                free(m);
                continue;
            }
            int st = tlsmode(a1);
            if (hopen(&O, &OM, x, st) < 0) { free(m); continue; }
            int r = smtp_send(&OM, st, strcmp(a2, "-") ? a2 : 0, a3, 0, from, rc, nr, m, mn, err, sizeof err);
            printf("!! send %d %s\n", r, r < 0 ? err : "");
            close(O.fd);
            free(m);
        } else if (line[0] && line[0] != '#')
            printf("!! unknown step: %s\n", line);
    }
    free(raw);
    mime_free(&pm);
    ms_free(&S);
    printf("!! end\n");
    return 0;
}
