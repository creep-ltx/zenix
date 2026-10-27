/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * telnet-client - a PC test client over net/telnet/ and user/xfer/ (DESIGN D20 8.1): what Zenix's telnet will do
 * with the stream, without the console. It connects, negotiates, decodes, filters, translates keys from a script,
 * plays the console's answer to ESC[6n, and starts Zmodem when the board does (detected in the raw bytes, before the
 * decoder; -e when BINARY is off that way), with Zenix's download rules (user/xfer/rzrules.c). Run by
 * tools/host/telnet-sim-test.py against tools/bbs-sim.py.
 *   telnet-client HOST PORT MODE SCRIPT OUTDIR [UPLOAD...]      MODE: pc, amiga, unix, raw
 * SCRIPT, a step a line: "wait TEXT" (the screen since the last wait has TEXT; 20 s), "key TEXT", "line TEXT" (and
 * Return), "sleep MS", "quit", "eof" (wait for the board to hang up); TEXT takes \r \n \e \\ and \xHH.
 * OUTDIR gets screen.bin (all the filter gave the screen), events.txt (the filter's events, one a line), xfer.txt
 * (each transfer's result and what the download rules said) and down/ (the downloads).
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "../../net/telnet/telnet.h"
#include "../../user/xfer/xfer.h"
#include "../../user/xfer/rzrules.h"

static struct {
    int fd, closed;
    struct tn tn;
    struct tf tf;
    FILE *screen, *events, *xfer;
    unsigned char *shot;                /* the screen so far, for "wait" */
    long nshot, waitfrom, dsrfrom;
    unsigned char tail[SPLIT_TAIL + 1];
    char down[1024];
    struct rz_fs fs;
    struct rz_dl dl;
    const char *const *uploads;
    int nup;
} c;

static void die(const char *what)
{
    fprintf(stderr, "telnet-client: %s\n", what);
    exit(3);
}

static void net_write(const unsigned char *b, long n)
{
    while (n > 0) {
        long k = write(c.fd, b, n);
        if (k <= 0) {
            if (k < 0 && errno == EINTR)
                continue;
            c.closed = 1;
            return;
        }
        b += k;
        n -= k;
    }
}

static void tsend(void *u, const unsigned char *b, long n) { (void)u; net_write(b, n); }

/* data to the board: through the encoder */
static void to_board(const unsigned char *b, long n)
{
    unsigned char *e = malloc(2 * n + 1);
    if (!e)
        die("no memory");
    net_write(e, tn_encode(&c.tn, b, n, e));
    free(e);
}

static void answer(void *u, const unsigned char *b, long n) { (void)u; to_board(b, n); }

static void sput(void *u, const unsigned char *b, long n)
{
    long i;
    (void)u;
    fwrite(b, 1, n, c.screen);
    c.shot = realloc(c.shot, c.nshot + n + 1);
    if (!c.shot)
        die("no memory");
    memcpy(c.shot + c.nshot, b, n);
    c.nshot += n;
    /* the console's answer to ESC[6n, as Zenix's console gives it */
    for (i = c.dsrfrom; i + 4 <= c.nshot; i++)
        if (!memcmp(c.shot + i, "\x1b[6n", 4)) {
            to_board((const unsigned char *)"\x1b[12;34R", 8);
            i += 3;
            c.dsrfrom = i + 1;
        }
    if (c.dsrfrom < c.nshot - 3)
        c.dsrfrom = c.nshot - 3;
}

static void sevent(void *u, int ev, int arg)
{
    (void)u;
    if (ev == TF_EV_ICE)
        fprintf(c.events, "ice %d\n", arg);
    else if (ev == TF_EV_MODE) {
        fprintf(c.events, "mode %s\n", arg == TN_PC ? "pc" : "amiga");
        c.tn.mode = arg;                /* the keys and the terminal type follow the mode */
    }
    fflush(c.events);
}

static void screen_bytes(const unsigned char *raw, long n)
{
    unsigned char *d = malloc(n + 1);
    long k;
    if (!d)
        die("no memory");
    k = tn_decode(&c.tn, raw, n, d);
    tf_write(&c.tf, d, k);
    free(d);
}

/* ---- Zmodem: the engine's I/O over the telnet connection ---- */
static long xget(void *u, unsigned char *b, long max, long ms)
{
    unsigned char raw[16384];
    struct pollfd p;
    long k;
    (void)u;
    for (;;) {
        p.fd = c.fd;
        p.events = POLLIN;
        if (poll(&p, 1, (int)ms) <= 0)
            return 0;
        k = read(c.fd, raw, max < (long)sizeof raw ? max : (long)sizeof raw);
        if (k <= 0)
            return -1;
        k = tn_decode(&c.tn, raw, k, b);
        if (k > 0)
            return k;                   /* (only telnet commands came: read on) */
    }
}
static void xput(void *u, const unsigned char *b, long n) { (void)u; to_board(b, n); }
static void xshow(void *u, const char *name, unsigned long done, unsigned long size, int what)
{
    (void)u; (void)size;
    if (what)
        fprintf(c.xfer, "file %s %s %lu\n", name, what == 1 ? "done" : what == 2 ? "skipped" : "failed", done);
}
static void xrest(void *u, const unsigned char *b, long n) { (void)u; tf_write(&c.tf, b, n); }
static void *xropen(void *u, const char *path, unsigned long *size)
{
    FILE *f = fopen(path, "rb");
    (void)u;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = (unsigned long)ftell(f);
    fseek(f, 0, SEEK_SET);
    return f;
}
static long xrread(void *u, void *f, unsigned char *b, long n) { (void)u; return (long)fread(b, 1, n, f); }
static int xrseek(void *u, void *f, unsigned long pos) { (void)u; return fseek(f, (long)pos, SEEK_SET); }
static void xrclose(void *u, void *f) { (void)u; fclose(f); }

/* the download rules' file calls (stdio, under OUTDIR/down) */
static void in_down(const char *p, char *out, size_t n) { snprintf(out, n, "%s/%s", c.down, p); }
static int fs_look(void *u, const char *p, unsigned long *size)
{
    char f[2048];
    struct stat st;
    (void)u;
    in_down(p, f, sizeof f);
    if (lstat(f, &st))
        return 0;
    *size = (unsigned long)st.st_size;
    return 1;
}
static void *fs_open(void *u, const char *p, int how)
{
    char f[2048];
    (void)u;
    in_down(p, f, sizeof f);
    return fopen(f, how == RZ_OPEN_NEW ? "wb" : how == RZ_OPEN_APPEND ? "ab" : "rb");
}
static long fs_read(void *u, void *h, unsigned char *b, long n) { (void)u; return (long)fread(b, 1, n, h); }
static long fs_write(void *u, void *h, const unsigned char *b, long n) { (void)u; return (long)fwrite(b, 1, n, h); }
static void fs_close(void *u, void *h) { (void)u; fclose(h); }
static int fs_rename(void *u, const char *a, const char *b)
{
    char fa[2048], fb[2048];
    struct stat st;
    (void)u;
    in_down(a, fa, sizeof fa);
    in_down(b, fb, sizeof fb);
    return lstat(fb, &st) ? rename(fa, fb) : -1;
}
static void fs_remove(void *u, const char *p) { char f[2048]; (void)u; in_down(p, f, sizeof f); unlink(f); }
static void fs_say(void *u, int what, const char *name, const char *other)
{
    (void)u;
    fprintf(c.xfer, "say %d %s %s\n", what, name, other ? other : "-");
}
static void *xwopen(void *u, const char *name, unsigned long size, char *made, int madelen, unsigned long *have)
{
    (void)u;
    return rz_wopen(&c.dl, name, size, made, madelen, have);
}
static void *xwresume(void *u, void *f, int ans, unsigned long crc, char *made, int madelen, unsigned long *have)
{
    (void)u;
    return rz_wresume(&c.dl, f, ans, crc, made, madelen, have);
}
static long xwwrite(void *u, void *f, const unsigned char *b, long n) { (void)u; return rz_wwrite(&c.dl, f, b, n); }
static void xwclose(void *u, void *f, int ok) { (void)u; rz_wclose(&c.dl, f, ok); }

static void transfer(int kind, const unsigned char *pre, long npre)
{
    XferIO io;
    int r;
    memset(&io, 0, sizeof io);
    io.get = xget; io.put = xput; io.show = xshow; io.rest = xrest;
    io.wopen = xwopen; io.wwrite = xwwrite; io.wclose = xwclose; io.wresume = xwresume;
    io.ropen = xropen; io.rread = xrread; io.rseek = xrseek; io.rclose = xrclose;
    /* -e (D20 5): where BINARY is off in the direction the file goes */
    io.escctl = kind == 1 ? !tn_on(&c.tn, 1, TO_BINARY) : !tn_on(&c.tn, 0, TO_BINARY);
    if (kind == 1)
        r = zm_receive(&io, pre, npre);
    else
        r = zm_send(&io, c.uploads, c.nup, pre, npre);
    fprintf(c.xfer, "%s %d escctl %d\n", kind == 1 ? "recv" : "send", r, io.escctl);
    fflush(c.xfer);
}

static void from_net(const unsigned char *raw, long n)
{
    static const unsigned char head[] = "**\x18" "B0";
    unsigned char *pre;
    long at = 0, k = 0;
    int had = 0, kind = split_detect(c.tail, raw, n, &had);
    if (!kind)
        kind = zm_detect(raw, n, &at);
    if (!kind) {
        screen_bytes(raw, n);
        return;
    }
    if (had)
        at = 0;                         /* it began in the read before: those bytes are on the screen already */
    screen_bytes(raw, at);
    tf_flush(&c.tf);
    pre = malloc(n - at + 8);
    if (!pre)
        die("no memory");
    if (had) {
        memcpy(pre, head, had);         /* the start the read before had */
        k = had;
    }
    k += tn_decode(&c.tn, raw + at, n - at, pre + k);
    transfer(kind, pre, k);
    free(pre);
    memset(c.tail, 0, sizeof c.tail);
}

/* \r \n \e \\ \xHH */
static long unescape(const char *s, unsigned char *out)
{
    long k = 0;
    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            if (*s == 'r') out[k++] = '\r';
            else if (*s == 'n') out[k++] = '\n';
            else if (*s == 'e') out[k++] = 0x1b;
            else if (*s == 'x' && s[1] && s[2]) {
                char h[3] = { s[1], s[2], 0 };
                out[k++] = (unsigned char)strtoul(h, 0, 16);
                s += 2;
            } else out[k++] = (unsigned char)*s;
            s++;
        } else
            out[k++] = (unsigned char)*s++;
    }
    return k;
}

static void pump(int ms)
{
    unsigned char raw[16384];
    struct pollfd p = { c.fd, POLLIN, 0 };
    if (c.closed || poll(&p, 1, ms) <= 0)
        return;
    long k = read(c.fd, raw, sizeof raw);
    if (k <= 0) {
        c.closed = 1;
        return;
    }
    from_net(raw, k);
}

static void type_keys(const unsigned char *b, long n)
{
    unsigned char *t = malloc(2 * n + 1);
    int bell = 0;
    long k;
    if (!t)
        die("no memory");
    k = tk_keys(c.tn.mode, b, n, t, &bell);
    if (bell)
        fprintf(c.events, "bell %d\n", bell);
    to_board(t, k);
    free(t);
}

static int wait_for(const unsigned char *t, long n, int secs)
{
    time_t end = time(0) + secs;
    for (;;) {
        long i;
        for (i = c.waitfrom; i + n <= c.nshot; i++)
            if (!memcmp(c.shot + i, t, n)) {
                c.waitfrom = i + n;
                return 1;
            }
        if (c.closed || time(0) > end)
            return 0;
        pump(100);
    }
}

int main(int argc, char **argv)
{
    struct addrinfo hints, *ai;
    char path[2048], line[4096];
    unsigned char buf[4096];
    const char *m;
    FILE *script;
    int mode;
    if (argc < 6)
        die("usage: telnet-client HOST PORT MODE SCRIPT OUTDIR [UPLOAD...]");
    m = argv[3];
    mode = !strcmp(m, "pc") ? TN_PC : !strcmp(m, "unix") ? TN_UNIX : TN_AMIGA;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(argv[1], argv[2], &hints, &ai))
        die("no such host");
    c.fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (c.fd < 0 || connect(c.fd, ai->ai_addr, ai->ai_addrlen))
        die("cannot connect");
    freeaddrinfo(ai);
    snprintf(path, sizeof path, "%s/screen.bin", argv[5]);
    c.screen = fopen(path, "wb");
    snprintf(path, sizeof path, "%s/events.txt", argv[5]);
    c.events = fopen(path, "w");
    snprintf(path, sizeof path, "%s/xfer.txt", argv[5]);
    c.xfer = fopen(path, "w");
    snprintf(c.down, sizeof c.down, "%s/down", argv[5]);
    mkdir(c.down, 0755);
    if (!c.screen || !c.events || !c.xfer || !(script = fopen(argv[4], "r")))
        die("cannot open the files");
    c.uploads = (const char *const *)argv + 6;
    c.nup = argc - 6;
    c.fs.look = fs_look; c.fs.open = fs_open; c.fs.read = fs_read; c.fs.write = fs_write;
    c.fs.close = fs_close; c.fs.rename = fs_rename; c.fs.remove = fs_remove; c.fs.say = fs_say;
    c.dl.fs = &c.fs;
    c.tn.send = tsend;
    c.tn.mode = mode;
    c.tn.cols = 80;
    c.tn.rows = 32;
    c.tn.raw = !strcmp(m, "raw");
    tn_init(&c.tn, atoi(argv[2]));
    c.tf.put = sput;
    c.tf.answer = answer;
    c.tf.event = sevent;
    c.tf.cols = 80;
    c.tf.rows = 32;
    tf_init(&c.tf, mode);
    while (fgets(line, sizeof line, script)) {
        char *arg;
        long n;
        line[strcspn(line, "\n")] = 0;
        arg = strchr(line, ' ');
        arg = arg ? arg + 1 : line + strlen(line);
        n = unescape(arg, buf);
        if (!strncmp(line, "wait ", 5)) {
            if (!wait_for(buf, n, 20)) {
                fprintf(stderr, "telnet-client: waited in vain for \"%s\"\n", arg);
                return 4;
            }
        } else if (!strncmp(line, "key ", 4))
            type_keys(buf, n);
        else if (!strncmp(line, "line", 4)) {
            buf[n++] = '\r';
            type_keys(buf, n);
        } else if (!strncmp(line, "sleep ", 6)) {
            int ms = atoi(arg);
            while (ms > 0) {
                pump(ms < 100 ? ms : 100);
                ms -= 100;
            }
        } else if (!strcmp(line, "eof")) {
            time_t end = time(0) + 20;
            while (!c.closed && time(0) < end)
                pump(100);
        } else if (!strcmp(line, "quit"))
            break;
    }
    fclose(c.screen);
    fclose(c.events);
    fclose(c.xfer);
    close(c.fd);
    free(c.shot);
    return 0;
}
