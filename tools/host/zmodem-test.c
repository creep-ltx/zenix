/* SPDX-License-Identifier: MIT */
/*
 * zmodem-test - user/xfer/ on the PC against lrzsz (the reference), DESIGN D20 8.1 "The Zmodem engine": lsz sends to
 * us, we send to lrz, over a socketpair, with small and big files, every byte value, 8 KB subpackets and escaped
 * controls, and a line that loses and garbles bytes; every file compared byte for byte. And Zenix's file rules
 * (user/xfer/rzrules.c, the downloads' side of the I/O table): .part files, the resume only after ZCRC (and a refused
 * splice), no overwrite (NAME.1, the same file skipped), leading dots refused, names cleaned, a sender that does not
 * answer ZCRC, five CANs, a full disk; our sender answering lrz's ZCRC. From CTerm's tests/zmodem_test.c (the
 * owner's, MIT; Copyright (c) 2026 Tobias Karlsson, the notice in user/xfer/zmodem.c).
 *   zmodem-test LSZ LRZ WORKDIR [quick]      (tools/host/telnet-test.sh builds and runs it; quick: no bad line)
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../../user/xfer/xfer.h"
#include "../../user/xfer/rzrules.h"
/* zmodem.c itself, for its frame writers (a host's script, below) */
#include "../../user/xfer/zmodem.c"

static char LSZ[1024], LRZ[1024], DIR[900];
static int fails, passes;
#define CHECK(c, what) do { if (c) { printf("PASS zmodem: %s\n", what); passes++; } \
    else { printf("FAIL zmodem: %s\n", what); fails++; } } while (0)

struct Line {
    int fd;
    unsigned noise;                     /* 1 in this many bytes spoiled */
    unsigned onoise;                    /* the same for what we send */
    unsigned seed;
    char down[1024];
    int together;                       /* reads wait for a quiet moment */
    unsigned long grow;                 /* ropen says the file is bigger */
    long diskfull;                      /* writes fail after this many bytes (0: never) */
    long opens, closes, okcloses, written;
    unsigned char rest[4096];           /* what came back after the end */
    long nrest;
    struct rz_fs fs;
    struct rz_dl dl;
    char said[4096];                    /* what the rules said, one line each */
};

static long get(void *u, unsigned char *b, long max, long ms)
{
    struct Line *l = u;
    struct pollfd p = { l->fd, POLLIN, 0 };
    long k;
    if (poll(&p, 1, (int)ms) <= 0)
        return 0;
    k = read(l->fd, b, max);
    if (k <= 0)
        return -1;
    while (l->together && k < max && poll(&p, 1, 100) > 0) {
        long j = read(l->fd, b + k, max - k);   /* the host's next bytes */
        if (j <= 0)                             /* in the same read */
            break;
        k += j;
    }
    if (l->noise) {                     /* a bad line: flip and drop */
        long i, j = 0;
        for (i = 0; i < k; i++) {
            l->seed = l->seed * 1103515245u + 12345u;
            if ((l->seed >> 8) % l->noise == 0) {
                if (l->seed & 0x10000)
                    continue;           /* lost */
                b[i] ^= 0x5a;           /* garbled */
            }
            b[j++] = b[i];
        }
        k = j;
    }
    return k;
}

static void put(void *u, const unsigned char *b0, long n0)
{
    struct Line *l = u;
    static unsigned char tmp[65536];
    const unsigned char *b = b0;
    long n = n0;
    if (l->onoise) {                    /* spoil what we send, too */
        long i, j = 0;
        for (i = 0; i < n0 && j < (long)sizeof tmp; i++) {
            l->seed = l->seed * 1103515245u + 12345u;
            if ((l->seed >> 8) % l->onoise == 0) {
                if (l->seed & 0x10000)
                    continue;
                tmp[j++] = b0[i] ^ 0x5a;
            } else
                tmp[j++] = b0[i];
        }
        b = tmp;
        n = j;
    }
    while (n > 0) {
        long k = write(l->fd, b, n);
        if (k <= 0)
            return;
        b += k;
        n -= k;
    }
}

static int stop(void *u) { (void)u; return 0; }
static void show(void *u, const char *name, unsigned long done, unsigned long size, int what)
{
    (void)u; (void)done; (void)size;
    if (what)
        printf("     %s %s\n", name, what == 1 ? "done" : what == 2 ? "skipped" : "failed");
}

/* ---- the rules' file calls, on the PC (paths under the download directory) ---- */
struct H { FILE *f; };
static void path_in(struct Line *l, const char *p, char *out, size_t n) { snprintf(out, n, "%s/%s", l->down, p); }
static int fs_look(void *u, const char *p, unsigned long *size)
{
    char full[2048];
    struct stat st;
    path_in(u, p, full, sizeof full);
    if (lstat(full, &st))
        return 0;
    *size = (unsigned long)st.st_size;
    return 1;
}
static void *fs_open(void *u, const char *p, int how)
{
    struct Line *l = u;
    char full[2048];
    path_in(l, p, full, sizeof full);
    l->opens += how != RZ_OPEN_READ;
    return fopen(full, how == RZ_OPEN_NEW ? "wb" : how == RZ_OPEN_APPEND ? "ab" : "rb");
}
static long fs_read(void *u, void *h, unsigned char *b, long n) { (void)u; return (long)fread(b, 1, n, h); }
static long fs_write(void *u, void *h, const unsigned char *b, long n)
{
    struct Line *l = u;
    if (l->diskfull && l->written + n > l->diskfull) {
        long k = l->diskfull - l->written;
        if (k > 0) {
            fwrite(b, 1, k, h);
            l->written += k;
        }
        errno = ENOSPC;
        return k > 0 ? k : -1;
    }
    l->written += n;
    return (long)fwrite(b, 1, n, h);
}
static void fs_close(void *u, void *h) { (void)u; fclose(h); }
static int fs_rename(void *u, const char *a, const char *b)
{
    char fa[2048], fb[2048];
    struct stat st;
    path_in(u, a, fa, sizeof fa);
    path_in(u, b, fb, sizeof fb);
    if (!lstat(fb, &st))
        return -1;                      /* never over a file */
    return rename(fa, fb);
}
static void fs_remove(void *u, const char *p) { char f[2048]; path_in(u, p, f, sizeof f); unlink(f); }
static void fs_say(void *u, int what, const char *name, const char *other)
{
    struct Line *l = u;
    size_t n = strlen(l->said);
    snprintf(l->said + n, sizeof l->said - n, "%d %s %s\n", what, name, other ? other : "-");
}

static void *wopen(void *u, const char *name, unsigned long size, char *made, int madelen, unsigned long *have)
{
    return rz_wopen(&((struct Line *)u)->dl, name, size, made, madelen, have);
}
static void *wresume(void *u, void *f, int ans, unsigned long crc, char *made, int madelen, unsigned long *have)
{
    return rz_wresume(&((struct Line *)u)->dl, f, ans, crc, made, madelen, have);
}
static long wwrite(void *u, void *f, const unsigned char *b, long n) { return rz_wwrite(&((struct Line *)u)->dl, f, b, n); }
static void wclose(void *u, void *f, int ok)
{
    struct Line *l = u;
    l->closes++;
    l->okcloses += ok != 0;
    rz_wclose(&l->dl, f, ok);
}

static void rest(void *u, const unsigned char *b, long n)
{
    struct Line *l = u;
    if (l->nrest + n <= (long)sizeof l->rest) {
        memcpy(l->rest + l->nrest, b, n);
        l->nrest += n;
    }
}

static void *ropen(void *u, const char *path, unsigned long *size)
{
    struct Line *l = u;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = (unsigned long)ftell(f) + l->grow;
    fseek(f, 0, SEEK_SET);
    return f;
}
static long rread(void *u, void *f, unsigned char *b, long n) { (void)u; return (long)fread(b, 1, n, f); }
static int rseek(void *u, void *f, unsigned long pos) { (void)u; return fseek(f, (long)pos, SEEK_SET); }
static void rclose(void *u, void *f) { (void)u; fclose(f); }

static void line_init(struct Line *l, const char *down)
{
    memset(l, 0, sizeof *l);
    l->seed = 1;
    snprintf(l->down, sizeof l->down, "%s", down);
    l->fs.u = l;
    l->fs.look = fs_look; l->fs.open = fs_open; l->fs.read = fs_read; l->fs.write = fs_write;
    l->fs.close = fs_close; l->fs.rename = fs_rename; l->fs.remove = fs_remove; l->fs.say = fs_say;
    l->dl.fs = &l->fs;
}

static XferIO io_for(struct Line *l)
{
    XferIO io;
    memset(&io, 0, sizeof io);
    io.u = l;
    io.get = get; io.put = put; io.stop = stop; io.show = show;
    io.wopen = wopen; io.wwrite = wwrite; io.wclose = wclose; io.wresume = wresume;
    io.ropen = ropen; io.rread = rread; io.rseek = rseek; io.rclose = rclose;
    io.rest = rest;
    return io;
}

/* lsz or lrz on the far end of a socketpair; the near end is ours */
static pid_t child(char *argv[], const char *cwd, int *fd)
{
    int sv[2];
    pid_t p;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
        return -1;
    p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_WRONLY);
        dup2(sv[1], 0);
        dup2(sv[1], 1);
        if (nul >= 0)
            dup2(nul, 2);               /* lrzsz's chatter */
        close(sv[0]);
        if (cwd && chdir(cwd)) _exit(99);
        execv(argv[0], argv);
        _exit(98);
    }
    close(sv[1]);
    *fd = sv[0];
    return p;
}

static int same(const char *a, const char *b)
{
    FILE *f = fopen(a, "rb"), *g = fopen(b, "rb");
    int c = 0, d = 0, ok = f && g;
    while (ok && (c = fgetc(f)) == (d = fgetc(g)))
        if (c == EOF)
            break;
    if (ok && c != d)
        ok = 0;
    if (f) fclose(f);
    if (g) fclose(g);
    return ok;
}

static int exists(const char *fmt, const char *a)
{
    char p[2048];
    struct stat st;
    snprintf(p, sizeof p, fmt, DIR, a);
    return !lstat(p, &st);
}

static long fsize(const char *p)
{
    struct stat st;
    return stat(p, &st) ? -1 : (long)st.st_size;
}

static void sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sh(const char *fmt, ...)
{
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (system(cmd))
        printf("     (command failed: %s)\n", cmd);
}

static void make(const char *name, long n, int kind)
{
    char path[2048];
    FILE *f;
    long i;
    unsigned s = 7;
    snprintf(path, sizeof path, "%s/src/%s", DIR, name);
    f = fopen(path, "wb");
    for (i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        fputc(kind == 0 ? "Zmodem test line\r\n"[i % 18]
              : kind == 1 ? (int)(i & 255) : (int)((s >> 16) & 255), f);
    }
    fclose(f);
}

static const char *names[] = { "empty", "text.txt", "bytes.bin", "random.bin" };
static const char *noisy[] = { "text.txt", "bytes.bin", "random-200k.bin" };     /* a bad line: 10 s per lost header */
#define download(w, f, n, o) download_(w, f, n, o, (n) ? noisy : names, (n) ? 3 : 4)
#define upload(w, f, n, o) upload_(w, f, n, o, (n) ? noisy : names, (n) ? 3 : 4)

/* wait for lsz's start header, then receive: the files, or -2 when it never began */
static int receive_from(struct Line *l, XferIO *io)
{
    unsigned char buf[4096];
    long n = 0, at = 0;
    int k;
    for (k = 0; k < 50 && !zm_detect(buf, n, &at); k++) {
        long g = get(l, buf + n, sizeof buf - n, 200);
        if (g > 0) n += g;
    }
    return zm_detect(buf, n, &at) == 1 ? zm_receive(io, buf + at, n - at) : -2;
}

/* lsz [flags] files -> zm_receive; the lsz exit status into *st */
static int lsz_to_us(struct Line *l, const char *const *flags, const char *const *files, int nfiles, int *st)
{
    char *argv[32];
    char paths[16][1200];
    XferIO io;
    int a = 0, i, got;
    pid_t p;
    argv[a++] = LSZ;
    argv[a++] = "-q";
    for (i = 0; flags && flags[i]; i++)
        argv[a++] = (char *)flags[i];
    for (i = 0; i < nfiles; i++) {
        snprintf(paths[i], sizeof paths[i], "%s/src/%s", DIR, files[i]);
        argv[a++] = paths[i];
    }
    argv[a] = NULL;
    p = child(argv, NULL, &l->fd);
    io = io_for(l);
    got = receive_from(l, &io);
    waitpid(p, st, 0);
    close(l->fd);
    return got;
}

static void fresh_down(void) { sh("rm -rf '%s/down' && mkdir -p '%s/down'", DIR, DIR); }

static void download_(const char *what, const char *flag, unsigned noise, unsigned onoise, const char *const *names, int nn)
{
    struct Line l;
    const char *flags[3] = { "-b", flag, NULL };
    char label[300], x[2048], y[2048];
    int i, st, got;
    fresh_down();
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    l.noise = noise;
    l.onoise = onoise;
    got = lsz_to_us(&l, flags, names, nn, &st);
    snprintf(label, sizeof label, "%s: %d files received", what, nn);
    CHECK(got == nn, label);
    for (i = 0; i < nn; i++) {
        snprintf(x, sizeof x, "%s/src/%s", DIR, names[i]);
        snprintf(y, sizeof y, "%s/down/%s", DIR, names[i]);
        snprintf(label, sizeof label, "%s: %s the same, no part left", what, names[i]);
        CHECK(same(x, y) && !exists("%s/down/%s.part", names[i]), label);
    }
    snprintf(label, sizeof label, "%s: lsz ended well", what);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, label);
}

/* zm_send -> lrz [flag]; up: the receiver's directory, made by the caller */
static int us_to_lrz(struct Line *l, const char *flag, const char *const *files, int n, int *st)
{
    char *argv[8];
    const char *paths[8];
    static char pbuf[8][1200];
    char up[1100];
    int a = 0, i, got;
    pid_t p;
    snprintf(up, sizeof up, "%s/up", DIR);
    argv[a++] = LRZ;
    argv[a++] = "-b";
    argv[a++] = "-q";
    if (flag) argv[a++] = (char *)flag;
    argv[a] = NULL;
    p = child(argv, up, &l->fd);
    XferIO io = io_for(l);
    for (i = 0; i < n; i++) {
        snprintf(pbuf[i], sizeof pbuf[i], "%s/src/%s", DIR, files[i]);
        paths[i] = pbuf[i];
    }
    got = zm_send(&io, paths, n, NULL, 0);
    waitpid(p, st, 0);
    close(l->fd);
    return got;
}

static void upload_(const char *what, const char *flag, unsigned noise, unsigned onoise, const char *const *names, int nn)
{
    struct Line l;
    char label[300], x[2048], y[2048];
    int i, st, got;
    sh("rm -rf '%s/up' && mkdir -p '%s/up'", DIR, DIR);
    line_init(&l, "");
    l.noise = noise;
    l.onoise = onoise;
    l.seed = 3;
    got = us_to_lrz(&l, flag, names, nn, &st);
    snprintf(label, sizeof label, "%s: %d files sent", what, nn);
    CHECK(got == nn, label);
    for (i = 0; i < nn; i++) {
        snprintf(x, sizeof x, "%s/src/%s", DIR, names[i]);
        snprintf(y, sizeof y, "%s/up/%s", DIR, names[i]);
        snprintf(label, sizeof label, "%s: %s the same", what, names[i]);
        CHECK(same(x, y), label);
    }
    snprintf(label, sizeof label, "%s: lrz ended well", what);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, label);
}

/* ---- Zenix's file rules (rzrules.c) ---- */

/* lsz sends random.bin while a part of it (300000 bytes) is here from an earlier try: ZCRC says it is the same
 * file, so only the rest comes; with lsz -r (it asks for a resume) the same */
static void resume(const char *what, const char *flag)
{
    struct Line l;
    const char *flags[3] = { "-b", flag, NULL }, *files[1] = { "random.bin" };
    char label[300], x[2048], y[2048];
    int st, got;
    fresh_down();
    sh("head -c 300000 '%s/src/random.bin' > '%s/down/random.bin.part'", DIR, DIR);
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    got = lsz_to_us(&l, flags, files, 1, &st);
    snprintf(x, sizeof x, "%s/src/random.bin", DIR);
    snprintf(y, sizeof y, "%s/down/random.bin", DIR);
    snprintf(label, sizeof label, "%s: 1 file received", what); CHECK(got == 1, label);
    snprintf(label, sizeof label, "%s: only the 700000 missing bytes came", what);
    CHECK(l.written == 1000000 - 300000, label);
    snprintf(label, sizeof label, "%s: random.bin the same, no part left", what);
    CHECK(same(x, y) && !exists("%s/down/%s", "random.bin.part"), label);
    snprintf(label, sizeof label, "%s: said: gone on from the part", what);
    CHECK(strstr(l.said, "4 random.bin random.bin.part\n") != NULL, label);
    snprintf(label, sizeof label, "%s: lsz ended well", what);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, label);
}

/* a part of ANOTHER file of the same name: ZCRC differs, so no splice - the part is kept untouched and the file
 * comes whole as random.bin.1 */
static void splice_refused(void)
{
    struct Line l;
    const char *flags[2] = { "-b", NULL }, *files[1] = { "random.bin" };
    char x[2048], y[2048], part[2048];
    int st, got;
    fresh_down();
    snprintf(part, sizeof part, "%s/down/random.bin.part", DIR);
    sh("head -c 300000 '%s/src/bytes.bin' '%s/src/text.txt' '%s/src/bytes.bin' > '%s'", DIR, DIR, DIR, part);
    sh("cp '%s' '%s/part.copy'", part, DIR);
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    got = lsz_to_us(&l, flags, files, 1, &st);
    snprintf(x, sizeof x, "%s/src/random.bin", DIR);
    snprintf(y, sizeof y, "%s/down/random.bin.1", DIR);
    CHECK(got == 1, "splice refused: 1 file received");
    CHECK(l.written == 1000000, "splice refused: the whole file came");
    CHECK(same(x, y), "splice refused: random.bin.1 is the file");
    snprintf(x, sizeof x, "%s/part.copy", DIR);
    CHECK(same(x, part), "splice refused: the other file's part is untouched");
    CHECK(!exists("%s/down/%s", "random.bin"), "splice refused: no random.bin made from two files");
    CHECK(strstr(l.said, "5 random.bin random.bin.part\n") && strstr(l.said, "3 random.bin random.bin.1\n"),
          "splice refused: said why, and the new name");
}

/* NAME exists: another size gives NAME.1 (NAME untouched), the same size is skipped (ZSKIP) and lsz goes on */
static void no_overwrite(void)
{
    struct Line l;
    const char *flags[2] = { "-b", NULL }, *files[3] = { "text.txt", "bytes.bin", "empty" };
    char x[2048], y[2048];
    int st, got;
    fresh_down();
    sh("printf 'mine' > '%s/down/text.txt' && cp '%s/src/bytes.bin' '%s/down/bytes.bin'", DIR, DIR, DIR);
    sh("touch -d 2000-01-01 '%s/down/bytes.bin'", DIR);
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    got = lsz_to_us(&l, flags, files, 3, &st);
    snprintf(x, sizeof x, "%s/src/text.txt", DIR);
    snprintf(y, sizeof y, "%s/down/text.txt.1", DIR);
    CHECK(got == 2, "no overwrite: 2 files received, 1 skipped");
    CHECK(same(x, y), "no overwrite: another text.txt was here: saved as text.txt.1");
    snprintf(y, sizeof y, "%s/down/text.txt", DIR);
    CHECK(fsize(y) == 4, "no overwrite: the text.txt that was here is untouched");
    CHECK(strstr(l.said, "2 bytes.bin bytes.bin\n") != NULL, "no overwrite: the same bytes.bin skipped, said");
    {
        struct stat s1;
        snprintf(y, sizeof y, "%s/down/bytes.bin", DIR);
        CHECK(!stat(y, &s1) && s1.st_mtime < 1000000000, "no overwrite: the skipped file is not written");
    }
    CHECK(exists("%s/down/%s", "empty"), "no overwrite: lsz went on to the next file");
    CHECK(WIFEXITED(st), "no overwrite: lsz ended");
}

/* a name beginning with '.' is refused (ZSKIP), the rest of the batch comes */
static void leading_dot(void)
{
    struct Line l;
    const char *flags[2] = { "-b", NULL }, *files[2] = { ".profile", "text.txt" };
    int st, got;
    fresh_down();
    sh("printf 'echo owned\\n' > '%s/src/.profile'", DIR);
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    got = lsz_to_us(&l, flags, files, 2, &st);
    CHECK(got == 1, "leading dot: 1 file received");
    CHECK(!exists("%s/down/%s", ".profile") && !exists("%s/down/%s", ".profile.part"),
          "leading dot: no .profile written");
    CHECK(strstr(l.said, "1 .profile -\n") != NULL, "leading dot: refused, said");
    CHECK(exists("%s/down/%s", "text.txt"), "leading dot: the next file came");
}

/* audit 2026-10-28 S14: a sender that sends more than its ZFILE said (a hostile board filling the disk): made here
 * with the engine's own framing, written at once into the line, the receiver's answers read and dropped. The
 * download ends at the size said, the part is kept, the session is cancelled */
static struct { unsigned char b[65536]; long n; } forged;
static void forge_put(void *u, const unsigned char *b, long n)
{
    (void)u;
    if (forged.n + n <= (long)sizeof forged.b) { memcpy(forged.b + forged.n, b, (size_t)n); forged.n += n; }
}
static void sends_more(void)
{
    static struct Z z;
    static unsigned char data[20000];
    XferIO fio;
    struct Line l;
    int sv[2], i, got;
    char p[2048];
    pid_t c;
    memset(&fio, 0, sizeof fio);
    fio.put = forge_put;
    memset(&z, 0, sizeof z);
    z.io = &fio;
    z.use32 = 1;
    forged.n = 0;
    crc_init();
    for (i = 0; i < (int)sizeof data; i++) data[i] = (unsigned char)(i * 7 + 3);
    pos_hdr(&z, 0);
    send_hex(&z, ZRQINIT);
    pos_hdr(&z, 0);
    send_bin(&z, ZFILE);
    send_data(&z, (const unsigned char *)"liar.bin\0" "15000 0 100644", 23, ZCRCW);
    pos_hdr(&z, 0);
    send_bin(&z, ZDATA);
    for (i = 0; i < (int)sizeof data; i += 1000) send_data(&z, data + i, 1000, i + 1000 < (int)sizeof data ? ZCRCG : ZCRCE);
    pos_hdr(&z, sizeof data);
    send_bin(&z, ZEOF);
    pos_hdr(&z, 0);
    send_hex(&z, ZFIN);
    flush(&z);
    fresh_down();
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { CHECK(0, "sends more: socketpair"); return; }
    c = fork();
    if (c == 0) {
        unsigned char junk[4096];
        long off = 0, w;
        close(sv[0]);
        fcntl(sv[1], F_SETFL, O_NONBLOCK);
        while (off < forged.n) {        /* the stream, and the answers read and dropped meanwhile */
            struct pollfd q = { sv[1], POLLOUT | POLLIN, 0 };
            poll(&q, 1, 1000);
            while (read(sv[1], junk, sizeof junk) > 0) {}
            if ((w = write(sv[1], forged.b + off, (size_t)(forged.n - off))) > 0) off += w;
        }
        fcntl(sv[1], F_SETFL, 0);
        while (read(sv[1], junk, sizeof junk) > 0) {}
        _exit(0);
    }
    close(sv[1]);
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    l.fd = sv[0];
    {
        XferIO io = io_for(&l);
        alarm(60);
        got = receive_from(&l, &io);
        alarm(0);
    }
    close(sv[0]);
    waitpid(c, NULL, 0);
    snprintf(p, sizeof p, "%s/down/liar.bin.part", DIR);
    CHECK(got == -1 && !exists("%s/down/%s", "liar.bin") && fsize(p) >= 0 && fsize(p) <= 15000,
          "sends more than its ZFILE said: ended at the size said (the part kept), never written past it");
}

/* audit 2026-10-28 S13: a refused name is said with its control characters made harmless: rz's -T lines to telnet
 * are one a line, and ".\nR 9999\nM evil.lha: done" forged two of them */
static void refused_name_said_clean(void)
{
    struct Line l;
    char made[64];
    unsigned long have;
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    CHECK(rz_wopen(&l.dl, ".\nR 9999\nM evil.lha: 51234 bytes, done\x1b[2J", 10, made, sizeof made, &have) == NULL,
          "a hostile name beginning with '.': refused");
    CHECK(!strchr(l.said, '\x1b') && strchr(l.said, '\n') == l.said + strlen(l.said) - 1 &&
          strstr(l.said, ".?R 9999?M evil.lha") != NULL, "... said on one line, its controls shown as '?'");
}

/* our sender answers lrz's ZCRC (lrz -r with a part of the same file): only the rest is sent */
static void lrz_resume(void)
{
    struct Line l;
    const char *files[1] = { "random.bin" };
    char x[2048], y[2048];
    int st, got;
    sh("rm -rf '%s/up' && mkdir -p '%s/up' && head -c 300000 '%s/src/random.bin' > '%s/up/random.bin'",
       DIR, DIR, DIR, DIR);
    line_init(&l, "");
    got = us_to_lrz(&l, "-r", files, 1, &st);
    snprintf(x, sizeof x, "%s/src/random.bin", DIR);
    snprintf(y, sizeof y, "%s/up/random.bin", DIR);
    CHECK(got == 1, "lrz -r: 1 file sent");
    CHECK(same(x, y), "lrz -r: random.bin the same (our sender answered its ZCRC)");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "lrz -r: lrz ended well");
}

/* The host's prompt right after the session (in the same read as "OO") comes back for the screen */
static void prompt_after(void)
{
    struct Line l;
    XferIO io;
    char *argv[4], cmd[3000], x[2048], y[2048];
    int st, got, k;
    pid_t p;
    fresh_down();
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    l.together = 1;
    snprintf(cmd, sizeof cmd, "'%s' -b -q '%s/src/text.txt'; printf 'BBS prompt> '", LSZ, DIR);
    argv[0] = "/bin/sh"; argv[1] = "-c"; argv[2] = cmd; argv[3] = NULL;
    p = child(argv, NULL, &l.fd);
    io = io_for(&l);
    got = receive_from(&l, &io);
    while ((k = (int)get(&l, l.rest + l.nrest, sizeof l.rest - l.nrest, 2000)) > 0)
        l.nrest += k;
    waitpid(p, &st, 0);
    close(l.fd);
    snprintf(x, sizeof x, "%s/src/text.txt", DIR);
    snprintf(y, sizeof y, "%s/down/text.txt", DIR);
    CHECK(got == 1 && same(x, y), "prompt after Zmodem: text.txt received");
    CHECK(l.nrest == 12 && !memcmp(l.rest, "BBS prompt> ", 12), "prompt after Zmodem: the prompt reaches the screen");
}

/* ---- a host's own script of frames, written with zmodem.c's writers ---- */
static unsigned char script[65536];
static long nscript;
static struct Z zs;
static void sput(void *u, const unsigned char *b, long n)
{
    (void)u;
    if (nscript + n <= (long)sizeof script) {
        memcpy(script + nscript, b, n);
        nscript += n;
    }
}
static long gone_(void *u, unsigned char *b, long max, long ms) { (void)u; (void)b; (void)max; (void)ms; return -1; }
static void drop(void *u, const unsigned char *b, long n) { (void)u; (void)b; (void)n; }

static XferIO sio;
static void s_begin(void)
{
    memset(&sio, 0, sizeof sio);
    sio.put = sput;
    nscript = 0;
    z_begin(&zs, &sio, NULL, 0);
    zs.use32 = 1;
}
static void s_file(const char *info, long n)
{
    pos_hdr(&zs, 0);
    zs.hdr[3] = ZCBIN;
    send_bin(&zs, ZFILE);
    send_data(&zs, (const unsigned char *)info, n, ZCRCW);
}
static void s_data(unsigned long at, const unsigned char *d, long n)
{
    pos_hdr(&zs, at);
    send_bin(&zs, ZDATA);
    send_data(&zs, d, n, ZCRCE);
}
static void s_eof(unsigned long at) { pos_hdr(&zs, at); send_bin(&zs, ZEOF); }
static void s_fin(const char *after)
{
    pos_hdr(&zs, 0);
    send_hex(&zs, ZFIN);
    ostr(&zs, after);
    flush(&zs);
}
static int run_script(struct Line *l)
{
    XferIO io = io_for(l);
    io.get = gone_;
    io.put = drop;
    return zm_receive(&io, script, nscript);
}

/* ZFILE, maybe some data, then ZFIN: the file is closed (a part), the session ends well, what follows "OO" comes back */
static void ends_inside(const char *what, long data)
{
    struct Line l;
    unsigned char d[300];
    char label[300], p[2048];
    long i;
    s_begin();
    s_file("cut.bin\0" "5000 0 100644 0 1", 25);
    for (i = 0; i < data; i++)
        d[i] = (unsigned char)i;
    if (data)
        s_data(0, d, data);
    s_fin("OOafter\r\n");
    fresh_down();
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    snprintf(label, sizeof label, "%s: no file, no failure", what);
    CHECK(run_script(&l) == 0, label);
    snprintf(label, sizeof label, "%s: the file was closed, as a part", what);
    CHECK(l.opens == 1 && l.closes == 1 && l.okcloses == 0, label);
    snprintf(p, sizeof p, "%s/down/cut.bin.part", DIR);
    snprintf(label, sizeof label, data ? "%s: the part holds what came" : "%s: an empty part is not kept", what);
    CHECK(data ? fsize(p) == data : fsize(p) < 0, label);
    snprintf(label, sizeof label, "%s: what followed \"OO\" comes back", what);
    CHECK(l.nrest == 7 && !memcmp(l.rest, "after\r\n", 7), label);
}

/* names from a host: drawers taken off, controls made '_', a leading dot refused, a long name cut */
static void names_cleaned(void)
{
    struct Line l;
    static const struct { const char *offered, *stored; } t[] = {
        { "../../etc/passwd", "passwd" },
        { "C:\\dos\\file.zip", "file.zip" },
        { "work:s/.profile", NULL },
        { "..", NULL },
        { "ctl\001\177\233name", "ctl___name" },
    };
    char info[400], label[400], longname[301];
    unsigned char d[3] = { 'a', 'b', 'c' };
    size_t i;
    int got, n;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        s_begin();
        n = snprintf(info, sizeof info, "%s", t[i].offered);
        n += 1 + snprintf(info + n + 1, sizeof info - n - 1, "3 0 100644 0 1");
        s_file(info, n + 1);
        if (t[i].stored) {
            s_data(0, d, 3);
            s_eof(3);
        }
        s_fin("OO");
        fresh_down();
        line_init(&l, "");
        snprintf(l.down, sizeof l.down, "%s/down", DIR);
        got = run_script(&l);
        {
            char shown[200];
            const char *q;
            int k = 0;
            for (q = t[i].offered; *q && k < 190; q++)      /* a label of printable bytes */
                k += snprintf(shown + k, sizeof shown - k, (*q & 0x80) || *q < 0x20 || *q == 0x7f ? "\\%03o" : "%c",
                              (unsigned char)*q);
            snprintf(label, sizeof label, "names: \"%s\" -> %s", shown, t[i].stored ? t[i].stored : "refused");
        }
        if (t[i].stored) {
            char p[2048];
            snprintf(p, sizeof p, "%s/down/%s", DIR, t[i].stored);
            CHECK(got == 1 && fsize(p) == 3, label);
        } else
            CHECK(got == 0 && l.opens == 0 && strstr(l.said, "1 ") == l.said, label);
    }
    memset(longname, 'x', 300);
    longname[300] = 0;
    s_begin();
    n = snprintf(info, sizeof info, "%s", longname);
    n += 1 + snprintf(info + n + 1, sizeof info - n - 1, "3 0 100644 0 1");
    s_file(info, n + 1);
    s_data(0, d, 3);
    s_eof(3);
    s_fin("OO");
    fresh_down();
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    got = run_script(&l);
    longname[RZ_NAMEMAX] = 0;
    CHECK(got == 1 && exists("%s/down/%s", longname), "names: a 300-byte name is cut to 246 (room for .999.part)");
}

/* a sender that does not answer ZCRC (it offers the file again): the part is kept, the file comes as NAME.1 */
static void zcrc_unanswered(void)
{
    struct Line l;
    unsigned char d[100];
    char p[2048];
    int i;
    for (i = 0; i < 100; i++)
        d[i] = (unsigned char)(i * 7);
    s_begin();
    s_file("old.bin\0" "100 0 100644 0 1", 24);
    s_file("old.bin\0" "100 0 100644 0 1", 24);     /* again: it did not understand our ZCRC */
    s_data(0, d, 100);
    s_eof(100);
    s_fin("OO");
    fresh_down();
    sh("printf 'xyz' > '%s/down/old.bin.part'", DIR);
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    CHECK(run_script(&l) == 1, "ZCRC unanswered: 1 file received");
    snprintf(p, sizeof p, "%s/down/old.bin.1", DIR);
    CHECK(fsize(p) == 100, "ZCRC unanswered: it came whole as old.bin.1");
    snprintf(p, sizeof p, "%s/down/old.bin.part", DIR);
    CHECK(fsize(p) == 3, "ZCRC unanswered: the part is kept as it was");
}

/* five CANs from the host mid-file: the session ends (-1), what came is kept as a part */
static void five_cans(void)
{
    struct Line l;
    unsigned char d[200];
    char p[2048];
    int i;
    for (i = 0; i < 200; i++)
        d[i] = (unsigned char)i;
    s_begin();
    s_file("can.bin\0" "5000 0 100644 0 1", 25);
    pos_hdr(&zs, 0);
    send_bin(&zs, ZDATA);
    send_data(&zs, d, 200, ZCRCG);
    for (i = 0; i < 5; i++)
        obyte(&zs, 0x18);
    flush(&zs);
    fresh_down();
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    CHECK(run_script(&l) == -1, "five CANs: the transfer ends as cancelled");
    snprintf(p, sizeof p, "%s/down/can.bin.part", DIR);
    CHECK(fsize(p) == 200, "five CANs: the 200 bytes that came are kept as can.bin.part");
}

/* the disk fills up: the transfer ends, the part keeps what was written */
static void disk_full(void)
{
    struct Line l;
    const char *flags[2] = { "-b", NULL }, *files[1] = { "random.bin" };
    char p[2048];
    int st, got;
    fresh_down();
    line_init(&l, "");
    snprintf(l.down, sizeof l.down, "%s/down", DIR);
    l.diskfull = 100000;
    got = lsz_to_us(&l, flags, files, 1, &st);
    snprintf(p, sizeof p, "%s/down/random.bin.part", DIR);
    CHECK(got == -1, "disk full: the transfer ends as failed");
    CHECK(fsize(p) == 100000 && !exists("%s/down/%s", "random.bin"), "disk full: the part is kept, no whole file");
}

/* The file is 5000 bytes shorter than its size said (it shrank while it went): it ends where it ends */
static void shrinks(void)
{
    struct Line l;
    const char *files[1] = { "text.txt" };
    char x[2048], y[2048];
    int st, got;
    sh("rm -rf '%s/up' && mkdir -p '%s/up'", DIR, DIR);
    line_init(&l, "");
    l.grow = 5000;
    alarm(60);
    got = us_to_lrz(&l, NULL, files, 1, &st);
    alarm(0);
    snprintf(x, sizeof x, "%s/src/text.txt", DIR);
    snprintf(y, sizeof y, "%s/up/text.txt", DIR);
    CHECK(got == 1 && same(x, y), "shrinks while sent: text.txt sent whole");
}

/* rz_choose alone, against a table of files */
static const char *fake[16][2];
static int fake_look(void *u, const char *p, unsigned long *size)
{
    int i;
    (void)u;
    for (i = 0; fake[i][0]; i++)
        if (!strcmp(fake[i][0], p)) {
            *size = strtoul(fake[i][1], 0, 10);
            return 1;
        }
    return 0;
}
static void choose_rules(void)
{
    char t[RZ_NAMEMAX + 8];
    unsigned long pl;
    int ix, r;
    memset(fake, 0, sizeof fake);
    r = rz_choose("a", 10, 0, 0, fake_look, 0, t, sizeof t, &pl, &ix);
    CHECK(r == RZ_FRESH && !strcmp(t, "a") && ix == 0, "choose: a free name");
    fake[0][0] = "a"; fake[0][1] = "10";
    r = rz_choose("a", 10, 0, 0, fake_look, 0, t, sizeof t, &pl, &ix);
    CHECK(r == RZ_SKIP && !strcmp(t, "a"), "choose: the same size is the same file: skip");
    r = rz_choose("a", 11, 0, 0, fake_look, 0, t, sizeof t, &pl, &ix);
    CHECK(r == RZ_FRESH && !strcmp(t, "a.1") && ix == 1, "choose: another size: a.1");
    fake[1][0] = "a.1.part"; fake[1][1] = "4";
    r = rz_choose("a", 11, 0, 0, fake_look, 0, t, sizeof t, &pl, &ix);
    CHECK(r == RZ_RESUME && !strcmp(t, "a.1") && pl == 4, "choose: a shorter part: resume after ZCRC");
    r = rz_choose("a", 11, ix + 1, RZF_NORESUME, fake_look, 0, t, sizeof t, &pl, &ix);
    CHECK(r == RZ_FRESH && !strcmp(t, "a.2"), "choose: after a ZCRC that said no: the next free name");
    r = rz_choose("a", 3, 0, 0, fake_look, 0, t, sizeof t, &pl, &ix);
    CHECK(r == RZ_FRESH && !strcmp(t, "a.2"), "choose: a part longer than the file is not this file's");
    fake[2][0] = "a.2.part"; fake[2][1] = "0";
    r = rz_choose("a", 3, 0, 0, fake_look, 0, t, sizeof t, &pl, &ix);
    CHECK(r == RZ_FRESH && !strcmp(t, "a.2"), "choose: an empty part is free");
    r = rz_choose("a", 10, 0, RZF_NOSKIP | RZF_NORESUME, fake_look, 0, t, sizeof t, &pl, &ix);
    CHECK(r == RZ_FRESH && !strcmp(t, "a.2"), "choose: NOSKIP never takes an existing file as the same");
    r = rz_choose("a", 10, 1000, 0, fake_look, 0, t, sizeof t, &pl, &ix);
    CHECK(r == RZ_FULL, "choose: past a.999 there is no name");
    {
        char o[RZ_NAMEMAX + 1];
        CHECK(rz_clean(".x", o, sizeof o) == -1 && rz_clean("", o, sizeof o) == -1 && rz_clean(0, o, sizeof o) == -1,
              "clean: empty and leading-dot names refused");
        CHECK(rz_clean("a b-c.ZIP", o, sizeof o) == 0 && !strcmp(o, "a b-c.ZIP"), "clean: an ordinary name kept");
    }
}

/* zmodem-test seeds DIR: sessions written by the engine's own writers, as seeds for tools/host/zmodem-fuzz.c (its
 * first byte: 0 receive, 4 receive with a part there; 1 send, after a ZRINIT and ZRPOS script) */
static void seeds(const char *dir)
{
    static const unsigned char d[100] = { 1, 2, 3, 0x18, 0x7f, 0xff, '\r', 0x11 };
    char p[2048];
    FILE *f;
    int i;
    for (i = 0; i < 3; i++) {
        s_begin();
        if (i == 2) {                   /* what a receiver says: ZRINIT, ZRPOS 0, ZCRC, ZRINIT, ZFIN */
            pos_hdr(&zs, 0);
            zs.hdr[3] = CANFDX | CANOVIO | CANFC32;
            send_hex(&zs, ZRINIT);
            pos_hdr(&zs, 0);
            send_hex(&zs, ZRPOS);
            pos_hdr(&zs, 0);
            send_hex(&zs, ZRINIT);
            pos_hdr(&zs, 0);
            send_hex(&zs, ZFIN);
            flush(&zs);
        } else {
            s_file("seed.bin\0" "100 0 100644 0 1", 25);
            if (i == 1) {
                pos_hdr(&zs, 0x12345678);
                send_bin(&zs, ZCRC);
            }
            s_data(0, d, 100);
            s_eof(100);
            s_fin("OOprompt");
        }
        snprintf(p, sizeof p, "%s/zseed%d", dir, i);
        f = fopen(p, "wb");
        if (!f)
            continue;
        fputc(i == 0 ? 0 : i == 1 ? 4 : 1, f);
        fwrite(script, 1, nscript, f);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    int quick = argc > 4 && !strcmp(argv[4], "quick");
    if (argc == 3 && !strcmp(argv[1], "seeds")) {
        seeds(argv[2]);
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: zmodem-test LSZ LRZ WORKDIR [quick]\n");
        return 2;
    }
    if (!realpath(argv[1], LSZ) || !realpath(argv[2], LRZ) || access(LSZ, X_OK) || access(LRZ, X_OK)) {
        printf("FAIL zmodem: lsz/lrz not found (%s, %s)\n", argv[1], argv[2]);
        return 2;
    }
    snprintf(DIR, sizeof DIR, "%s", argv[3]);
    sh("rm -rf '%s' && mkdir -p '%s/src'", DIR, DIR);
    make("empty", 0, 0);
    make("text.txt", 3000, 0);
    make("bytes.bin", 256 * 4, 1);
    make("random.bin", 1000000, 2);
    make("random-200k.bin", 200000, 2);
    choose_rules();
    download("lsz", NULL, 0, 0);
    download("lsz -8 (8 KB subpackets)", "-8", 0, 0);
    download("lsz -e (controls escaped)", "-e", 0, 0);
    upload("lrz", NULL, 0, 0);
    upload("lrz -e (asks for controls escaped)", "-e", 0, 0);
    if (!quick) {
        download("lsz, a bad line both ways", NULL, 3000, 500);
        upload("lrz, a bad line both ways", NULL, 500, 3000);
    }
    resume("resume after ZCRC", NULL);
    resume("resume after ZCRC, lsz -r", "-r");
    splice_refused();
    no_overwrite();
    leading_dot();
    refused_name_said_clean();
    sends_more();
    lrz_resume();
    prompt_after();
    ends_inside("ZFILE, data, ZFIN", 200);
    ends_inside("ZFILE, ZFIN", 0);
    names_cleaned();
    zcrc_unanswered();
    five_cans();
    disk_full();
    shrinks();
    sh("rm -rf '%s'", DIR);
    printf("zmodem: %d passed, %d failed\n", passes, fails);
    return fails != 0;
}
