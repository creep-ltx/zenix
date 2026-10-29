/* SPDX-License-Identifier: GPL-2.0-only */
/* rz and sz: Zmodem downloads and uploads over standard input and output (DESIGN D20 5, docs/man/rz.txt), one program
 * with two names (sz is a link to rz, as ifup is ifconfig). The protocol is the engine in user/xfer/ (CTerm's
 * zmodem.c, MIT); where a download goes is user/xfer/rzrules.c (no system calls, shared with the PC tests); this file
 * gives both Zenix's file calls. The contract is lrzsz's: the other side's Zmodem bytes on standard input, ours on
 * standard output, so the same program serves telnet (which starts it when a board starts a transfer), cu later, and
 * a session into Zenix.
 *   rz [-e] [-q] [-T] [-d DIR]       receive into the current directory (or DIR)
 *   sz [-e] [-q] [-T] FILE...        send
 * -e escapes every control character (a telnet line without BINARY), -q says nothing but errors, -T is telnet's form
 * of standard error: "P text" progress lines, "M text" messages, and "R n" followed by n bytes that came after the
 * session (the board's prompt after "OO", for the screen). Exit status 0 when every file went well, 1 otherwise
 * (cancelled, failed, refused, skipped), 2 for wrong arguments. */
#include "libz.h"
#include "xfer/xfer.h"
#include "xfer/rzrules.h"

static int quiet, tform, stopped, gone, bad, files_ok, said;   /* said: the rules told why a file is not taken */
static u32 shown_at;

static void on_stop(int sig) { (void)sig; stopped = 1; }

/* ---- standard error: what the person sees (telnet reads it with -T) ---- */
static void tell(char kind, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (tform)
        dprintf(2, "%c %s\n", kind, line);
    else if (kind == 'P')
        dprintf(2, "\r%s\033[K", line);
    else
        dprintf(2, "\r%s\033[K\r\n", line);
}

/* ---- the host: standard input and output ---- */
static long x_get(void *u, unsigned char *b, long max, long ms)
{
    (void)u;
    u32 end = sys_ticks() + (u32)(ms + 9) / 10;
    for (;;) {
        struct pollfd p = { 0, POLLIN, 0 };
        int left = (int)(end - sys_ticks());
        int r = sys_evwait(&p, 1, left > 0 ? left : 0);
        if (r < 0 && r != -EINTR)
            return -1;
        if (stopped)
            return 0;
        if (r > 0) {
            int n = sys_read(0, b, (u32)(max > 4096 ? 4096 : max));
            if (n > 0)
                return n;
            if (n == 0 || (n != -EAGAIN && n != -EINTR))
                return -1;              /* the end of the other side (or a broken line) */
        }
        if (left <= 0)
            return 0;
    }
}

static void x_put(void *u, const unsigned char *b, long n)
{
    (void)u;
    while (n > 0 && !gone) {
        int w = sys_write(1, b, (u32)n);
        if (w == -EAGAIN || w == -EINTR) {
            struct pollfd p = { 1, POLLOUT, 0 };
            sys_evwait(&p, 1, 100);
            continue;
        }
        if (w <= 0) {
            gone = 1;                   /* the other side is gone: the engine's next read ends it */
            return;
        }
        b += w;
        n -= w;
    }
}

static int x_stop(void *u) { (void)u; return stopped; }

static void x_show(void *u, const char *name, unsigned long done, unsigned long size, int what)
{
    (void)u;
    if (what == 1) {
        files_ok++;
        if (!quiet)
            tell('M', "%s: %lu bytes, done", name, done);
        return;
    }
    if (what == 2 || what == 3) {
        if (said) {                     /* not taken, and the rules said why (the same file, a dot name ...) */
            said = 0;
            return;
        }
        bad = 1;
        if (!quiet || what == 3)
            tell('M', "%s: %s", name, what == 2 ? "skipped" : "failed");
        return;
    }
    u32 now = sys_ticks();
    if (quiet || (now - shown_at < 50 && done != 0))
        return;                         /* twice a second is enough */
    shown_at = now;
    if (size)
        tell('P', "%s: %lu of %lu bytes (%lu%%)", name, done, size,
             size >= 0x1000000 ? done / (size / 100 ? size / 100 : 1) : done * 100 / size);
    else
        tell('P', "%s: %lu bytes", name, done);
}

static void x_rest(void *u, const unsigned char *b, long n)
{
    (void)u;
    if (n <= 0)
        return;
    if (tform)
        dprintf(2, "R %ld\n", n);
    while (n > 0) {
        int w = sys_write(2, b, (u32)n);
        if (w <= 0)
            return;
        b += w;
        n -= w;
    }
}

/* ---- files: the download rules' calls ---- */
static int f_look(void *u, const char *path, unsigned long *size)
{
    (void)u;
    struct ustat st;
    if (sys_lstat(path, &st) < 0)
        return 0;
    *size = st.size;
    return 1;
}

static void *f_open(void *u, const char *path, int how)
{
    (void)u;
    int fd = sys_open(path, how == RZ_OPEN_NEW ? O_WRONLY | O_CREAT | O_TRUNC :
                            how == RZ_OPEN_APPEND ? O_WRONLY | O_APPEND : O_RDONLY);
    return fd < 0 ? 0 : (void *)(long)(fd + 1);
}

static long f_read(void *u, void *h, unsigned char *b, long n) { (void)u; return sys_read((int)(long)h - 1, b, (u32)n); }

static long f_write(void *u, void *h, const unsigned char *b, long n)
{
    (void)u;
    long done = 0;
    while (done < n) {
        int w = sys_write((int)(long)h - 1, b + done, (u32)(n - done));
        if (w <= 0) {
            tell('M', "cannot write: %s", zstrerror(w) ? zstrerror(w) : "failed");
            return w < 0 ? w : -1;      /* a full disk ends the transfer; the part is kept */
        }
        done += w;
    }
    return done;
}

static void f_close(void *u, void *h) { (void)u; sys_close((int)(long)h - 1); }

static int f_rename(void *u, const char *from, const char *to)
{
    unsigned long sz;
    if (f_look(u, to, &sz))
        return -1;                      /* never over an existing file */
    return sys_rename(from, to);
}

static void f_remove(void *u, const char *path) { (void)u; sys_unlink(path); }

static void f_say(void *u, int what, const char *name, const char *other)
{
    (void)u;
    said = what == RZ_SAY_REFUSED || what == RZ_SAY_SAME || what == RZ_SAY_FULL || what == RZ_SAY_NOOPEN;
    switch (what) {
    case RZ_SAY_REFUSED: bad = 1; tell('M', "%s: refused (an empty name or one that begins with '.')", name); break;
    case RZ_SAY_SAME:    tell('M', "%s: the same file is here already (%s): skipped", name, other); break;
    case RZ_SAY_OTHER:   tell('M', "%s: there is another file of that name: saved as %s", name, other); break;
    case RZ_SAY_RESUME:  tell('M', "%s: going on from %s (the same file: CRC checked)", name, other); break;
    case RZ_SAY_NOTSAME: tell('M', "%s: %s is not the same file: kept, the file comes whole", name, other); break;
    case RZ_SAY_FULL:    bad = 1; tell('M', "%s: no free name up to .999: refused", name); break;
    case RZ_SAY_KEPT:    bad = 1; tell('M', "%s: ended early: what came is kept as %s", name, other); break;
    case RZ_SAY_RENAMED: tell('M', "%s appeared meanwhile: the file is %s", name, other); break;
    case RZ_SAY_NOOPEN:  bad = 1; tell('M', "%s: cannot open %s", name, other); break;
    }
}

static const struct rz_fs fs = { 0, f_look, f_open, f_read, f_write, f_close, f_rename, f_remove, f_say };
static struct rz_dl dl;

/* paths are at most 63 bytes (ZPATH_MAX): a long name is shortened in front of its extension, so NAME.999.part fits */
#define NAMEFIT (ZPATH_MAX - 1 - 9)
static const char *fit(const char *name, char *buf)
{
    u32 n = strlen(name);
    if (n <= NAMEFIT)
        return name;
    const char *dot = strrchr(name, '.');
    u32 ext = dot && dot != name ? strlen(dot) : 0;
    if (ext > 12)
        ext = 0;
    memcpy(buf, name, NAMEFIT - ext);
    memcpy(buf + NAMEFIT - ext, dot ? dot : "", ext);
    buf[NAMEFIT] = 0;
    return buf;
}

static void *x_wopen(void *u, const char *name, unsigned long size, char *made, int madelen, unsigned long *have)
{
    (void)u;
    char buf[NAMEFIT + 1];
    return rz_wopen(&dl, fit(name, buf), size, made, madelen, have);
}

static void *x_wresume(void *u, void *f, int ans, unsigned long crc, char *made, int madelen, unsigned long *have)
{
    (void)u;
    return rz_wresume(&dl, f, ans, crc, made, madelen, have);
}

static long x_wwrite(void *u, void *f, const unsigned char *b, long n) { (void)u; return rz_wwrite(&dl, f, b, n); }
static void x_wclose(void *u, void *f, int ok) { (void)u; rz_wclose(&dl, f, ok); }

/* uploads */
static void *x_ropen(void *u, const char *path, unsigned long *size)
{
    (void)u;
    struct ustat st;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        tell('M', "%s: %s", path, zstrerror(fd) ? zstrerror(fd) : "cannot open");
        return 0;
    }
    if (sys_fstat(fd, &st) < 0 || (st.mode & S_IFMT) == S_IFDIR) {
        tell('M', "%s: not a file", path);
        sys_close(fd);
        return 0;
    }
    *size = st.size;
    return (void *)(long)(fd + 1);
}

static long x_rread(void *u, void *f, unsigned char *b, long n) { (void)u; return sys_read((int)(long)f - 1, b, (u32)n); }

static int x_rseek(void *u, void *f, unsigned long pos)
{
    (void)u;
    return sys_lseek((int)(long)f - 1, (int)pos, 0) == (int)pos ? 0 : -1;
}

static void x_rclose(void *u, void *f) { (void)u; sys_close((int)(long)f - 1); }

static int usage(int send)
{
    dprintf(2, send ? "usage: sz [-e] [-q] [-T] FILE...\n" : "usage: rz [-e] [-q] [-T] [-d DIR]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    int send = !strcmp(me, "sz"), escctl = 0, i = 1;
    const char *dir = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-e")) escctl = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-T")) tform = 1;
        else if (!send && !strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else return usage(send);
    }
    if (send ? i >= argc : i != argc)
        return usage(send);
    if (dir && sys_chdir(dir) < 0) {
        dprintf(2, "%s: %s: %s\n", me, dir, zwhy("cannot go there"));
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;            /* no SA_RESTART: a wait ends, and the engine cancels (ten CANs) */
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGHUP, &sa, 0);
    int raw = sys_isatty(0);
    if (raw)
        sys_ttymode(TTY_RAW | TTY_NOSIG);       /* a terminal as the line: every byte as it is */

    static XferIO io;
    io.get = x_get; io.put = x_put; io.stop = x_stop; io.show = x_show; io.rest = x_rest;
    io.wopen = x_wopen; io.wresume = x_wresume; io.wwrite = x_wwrite; io.wclose = x_wclose;
    io.ropen = x_ropen; io.rread = x_rread; io.rseek = x_rseek; io.rclose = x_rclose;
    io.escctl = escctl;
    dl.fs = &fs;
    int r = send ? zm_send(&io, (const char *const *)argv + i, argc - i, 0, 0) : zm_receive(&io, 0, 0);
    if (raw)
        sys_ttymode(0);
    if (r < 0)
        tell('M', "%s", stopped ? "cancelled" : gone ? "the other side is gone" : "the transfer failed");
    else if (!quiet)
        tell('M', "%d file%s %s", r, r == 1 ? "" : "s", send ? "sent" : "received");
    return r < 0 || bad ? 1 : 0;
}
