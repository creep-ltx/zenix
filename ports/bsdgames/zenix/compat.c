/* SPDX-License-Identifier: GPL-2.0-only */
/* The BSD library calls the BSD games use that Zenix's C library does not have (ports/bsdgames/README):
 * err/warn and friends, getprogname, arc4random from the kernel's generator (/dev/urandom), tcflush's input half,
 * getlogin, setegid, and flock and ftruncate, which Zenix has not (ENOSYS; fortune ignores flock's answer). */
#include "zgames.h"
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include "err.h"

extern char **_zargv;                   /* the C library's start (libc/zenix/start.c) */
static const char *progname;

const char *getprogname(void)
{
    if (!progname) {
        const char *p = _zargv && _zargv[0] ? _zargv[0] : "game", *s = strrchr(p, '/');
        progname = s ? s + 1 : p;
    }
    return progname;
}
void setprogname(const char *p)
{
    const char *s = strrchr(p, '/');
    progname = s ? s + 1 : p;
}

void vwarnx(const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: ", getprogname());
    if (fmt)
        vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
void vwarn(const char *fmt, va_list ap)
{
    int e = errno;
    fprintf(stderr, "%s: ", getprogname());
    if (fmt) {
        vfprintf(stderr, fmt, ap);
        fputs(": ", stderr);
    }
    fprintf(stderr, "%s\n", strerror(e));
}
void warnx(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarnx(fmt, ap); va_end(ap); }
void warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarn(fmt, ap); va_end(ap); }
void verrx(int st, const char *fmt, va_list ap) { vwarnx(fmt, ap); exit(st); }
void verr(int st, const char *fmt, va_list ap) { vwarn(fmt, ap); exit(st); }
void errx(int st, const char *fmt, ...) { va_list ap; va_start(ap, fmt); verrx(st, fmt, ap); }
void err(int st, const char *fmt, ...) { va_list ap; va_start(ap, fmt); verr(st, fmt, ap); }

/* random numbers: 64 bytes at a time from /dev/urandom (never waits); without it (no device), a xorshift seeded
 * from the clock and the pid, which is good enough for a game */
static unsigned char pool[64];
static unsigned npool;
static uint32_t xs;
uint32_t arc4random(void)
{
    if (npool < 4) {
        int fd = open("/dev/urandom", O_RDONLY);
        int n = fd >= 0 ? (int)read(fd, pool, sizeof pool) : -1;
        if (fd >= 0)
            close(fd);
        if (n == (int)sizeof pool)
            npool = sizeof pool;
        else {
            if (!xs)
                xs = (uint32_t)time(0) ^ (uint32_t)getpid() << 16 ^ 0x9E3779B9u;
            xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5;
            return xs;
        }
    }
    npool -= 4;
    return (uint32_t)pool[npool] << 24 | (uint32_t)pool[npool + 1] << 16 | (uint32_t)pool[npool + 2] << 8 |
           pool[npool + 3];
}
uint32_t arc4random_uniform(uint32_t n)
{
    if (n < 2)
        return 0;
    uint32_t min = -n % n;              /* 2^32 mod n: below it the values would favour some results */
    for (;;) {
        uint32_t r = arc4random();
        if (r >= min)
            return r % n;
    }
}

int flock(int fd, int op)
{
    (void)fd; (void)op;
    errno = ENOSYS;
    return -1;
}

/* tcflush: Zenix's termios has none; the input already typed is read and dropped (robots after a move) */
int tcflush(int fd, int q)
{
    if (q == TCIFLUSH || q == TCIOFLUSH) {
        struct pollfd p = { fd, POLLIN, 0 };
        unsigned char b[32];
        while (poll(&p, 1, 0) > 0 && read(fd, b, sizeof b) > 0)
            ;
    }
    return 0;
}

/* getlogin: the user's name ($USER, else the password file's for the uid); setegid: only to the real or effective
 * group already held (no game is set-group-id on Zenix, so that is all a game asks); ftruncate: Zenix has no
 * truncating of an open file, ENOSYS (tetris truncates only a damaged score file, and then keeps no scores) */
char *getlogin(void)
{
    char *u = getenv("USER");
    if (u && *u)
        return u;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_name : NULL;
}
int setegid(gid_t g)
{
    if (g == getgid() || g == getegid())
        return 0;
    errno = EPERM;
    return -1;
}
int ftruncate(int fd, off_t len)
{
    (void)fd; (void)len;
    errno = ENOSYS;
    return -1;
}
