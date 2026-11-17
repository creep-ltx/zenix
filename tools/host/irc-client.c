/* SPDX-License-Identifier: GPL-2.0-only */
/* irc's core on the PC (DESIGN D58 section 8, tools/host/irc-test.sh): a client without a screen over net/irc/,
 * plain or TLS (the crypto library's glue, as tools/host/wget-client.c), driven by a script, against
 * tools/host/irc-fake.py. Every line a window gets is printed as "[WINDOW] TEXT" ("[status]" for the status
 * window), every line sent as ">> LINE". When the connection ends it connects again at once (the program's
 * reconnect, without its wait), up to three times.
 *
 *   irc-client [--tls --anchors FILE --name HOST] [--config FILE] PORT SCRIPT
 *
 * The script, a step a line: "type TEXT" (a line typed in the current window), "expect TEXT" (wait at most 10 s
 * until a window gets a line holding TEXT since the last "mark", else exit 1), "mark", "window N", "complete PREFIX N" (print the Nth nick
 * completion of PREFIX in the current window), "dump" (the current window's lines, oldest first), "lost" (wait for
 * the connection to end), "end". */
#include "../../net/irc/irc.h"
#include "../../crypto/zc/zc_tls.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static struct irc S;
static int fd = -1, tls, conns, lostflag;
static const char *name = "rsa.test";
static int port;
static FILE *anchors;
static struct zc_tls T;
static unsigned char iobuf[ZC_TLS_BUFSIZE], chainbuf[ZC_X509_CHAINMAX], store[16384];
static char seen[64][600];
static int nseen, mark;

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
static long tsend(void *u, const unsigned char *p, size_t n) { (void)u; long k = send(fd, p, n, MSG_NOSIGNAL); return k < 0 ? -errno : k; }
static long trecv(void *u, unsigned char *p, size_t n) { (void)u; long k = recv(fd, p, n, 0); return k < 0 ? -errno : k; }

static void out(void *u, const char *l, int n)
{
    (void)u;
    printf(">> %.*s\n", n - 2, l);
    if (tls) {
        int r;
        while ((r = zc_tls_write(&T, l, (size_t)n)) == ZC_TLS_ESTATE &&
               (br_ssl_engine_current_state(&T.cc.eng) & BR_SSL_RECVAPP)) {
            char b[1024];
            long k = zc_tls_read(&T, b, sizeof b);
            if (k > 0) irc_input(&S, b, (int)k);
        }
    } else if (send(fd, l, (size_t)n, MSG_NOSIGNAL) != n)
        printf("!! send failed\n");
}

static void shown(void *u, int w, const struct irc_line *l)
{
    (void)u;
    if (w < 0)
        return;
    printf("[%s] %s\n", w ? S.w[w]->name : "status", l->text);
    snprintf(seen[nseen++ % 64], sizeof seen[0], "%s", l->text);
    fflush(stdout);
}

static int net_connect(void)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        printf("!! connect: %s\n", strerror(errno));
        return -1;
    }
    if (tls) {
        static struct zc_clock clk;
        unsigned char seed[32];
        struct zc_tls_io io = { 0, tsend, trecv };
        for (int i = 0; i < 32; i++) seed[i] = (unsigned char)rand();
        clk.trusted = 1;
        clk.now = (u32)time(0);
        clk.floor = clk.now - 86400;
        zc_tls_init(&T, iobuf, chainbuf, store, sizeof store, find, 0, 0);
        int r = zc_tls_connect(&T, &io, name, (unsigned)port, &clk, seed, 0);
        if (r < 0) {
            printf("!! TLS: %s\n", zc_tls_strerror(r == ZC_TLS_EENGINE ? zc_tls_error(&T) : r));
            close(fd);
            fd = -1;
            return -1;
        }
        printf("!! TLS ok\n");
    }
    conns++;
    irc_login(&S);
    return 0;
}

/* the server's bytes for at most ms: 0, or -1 when the connection ended */
static int pump(int ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    char b[1024];
    long k;
    S.now = (u32)time(0);
    if (fd < 0)
        return -1;
    if (!(tls && (br_ssl_engine_current_state(&T.cc.eng) & BR_SSL_RECVAPP)) && poll(&p, 1, ms) <= 0)
        return 0;
    k = tls ? zc_tls_read(&T, b, sizeof b) : recv(fd, b, sizeof b, 0);
    if (k <= 0) {
        close(fd);
        fd = -1;
        lostflag = 1;
        irc_lost(&S, "the server closed the connection");
        if (conns < 4 && S.want != IRC_WANT_QUIT)
            net_connect();
        return -1;
    }
    irc_input(&S, b, (int)k);
    return 0;
}

static int seen_has(const char *t)
{
    for (int i = 0; i < 64 && i < nseen - mark; i++)
        if (strstr(seen[(nseen - 1 - i) % 64], t))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    const char *conf = 0;
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        if (!strcmp(argv[i], "--tls")) tls = 1;
        else if (!strcmp(argv[i], "--anchors")) anchors = fopen(argv[++i], "rb");
        else if (!strcmp(argv[i], "--name")) name = argv[++i];
        else if (!strcmp(argv[i], "--config")) conf = argv[++i];
    }
    if (argc - i != 2) {
        fprintf(stderr, "usage: irc-client [--tls --anchors FILE --name HOST] [--config FILE] PORT SCRIPT\n");
        return 2;
    }
    setvbuf(stdout, 0, _IOLBF, 0);
    port = atoi(argv[i]);
    FILE *sc = fopen(argv[i + 1], "r");
    char *text = calloc(1, 8193), err[160];
    if (conf) {
        FILE *f = fopen(conf, "r");
        if (!f || fread(text, 1, 8192, f) == 0) { perror(conf); return 2; }
        fclose(f);
    }
    if (irc_config(&S, text, err, sizeof err) < 0) {
        printf("!! config: %s\n", err);
        return 3;
    }
    S.send = out;
    S.shown = shown;
    S.now = (u32)time(0);
    irc_init(&S);
    if (!sc || net_connect() < 0)
        return 1;
    char line[1200];
    while (fgets(line, sizeof line, sc)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "type ", 5)) {
            S.now = (u32)time(0);
            irc_command(&S, line + 5);
            if (S.want == IRC_WANT_QUIT) {
                for (int k = 0; k < 20 && fd >= 0; k++) pump(100);
                printf("!! quit\n");
                break;
            }
        } else if (!strncmp(line, "expect ", 7)) {
            time_t t0 = time(0);
            while (!seen_has(line + 7)) {
                if (time(0) - t0 > 10) { printf("!! expected: %s\n", line + 7); return 1; }
                pump(200);
            }
        } else if (!strcmp(line, "mark"))
            mark = nseen;
        else if (!strncmp(line, "window ", 7))
            irc_select(&S, atoi(line + 7) - 1);
        else if (!strncmp(line, "complete ", 9)) {
            char pre[64];
            int nth = 0;
            sscanf(line + 9, "%63s %d", pre, &nth);
            const char *m = irc_nick_match(&S, S.cur, pre, (int)strlen(pre), nth);
            printf("!! complete %s %d: %s\n", pre, nth, m ? m : "(none)");
        } else if (!strcmp(line, "dump")) {
            struct irc_win *w = S.w[S.cur];
            for (int k = w->n - 1; k >= 0; k--)
                printf("!! dump [%s] %s\n", S.cur ? w->name : "status", w->ring[(w->head + w->cap - 1 - k) % w->cap]->text);
            printf("!! nicks %d:", w->nn);
            for (int k = 0; k < w->nn; k++) printf(" %s", w->nicks[k]);
            printf("\n!! windows %d me %s\n", S.nw, S.me);
        } else if (!strcmp(line, "lost")) {
            time_t t0 = time(0);
            lostflag = 0;
            while (!lostflag && time(0) - t0 < 10) pump(200);
            printf("!! lost %d\n", lostflag);
        } else if (!strcmp(line, "end"))
            break;
    }
    irc_free(&S);
    printf("!! end, %d connections\n", conns);
    return 0;
}
