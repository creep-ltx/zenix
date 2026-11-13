/* SPDX-License-Identifier: GPL-2.0-only */
/* The C library's system layer (DESIGN D27 section 5): the POSIX names picolibc and programs call, each over one of
 * Zenix's system calls (user/usys.h). A failed call returns -1 with errno set (usys.h sets it: zerrno is errno in
 * the C library). What Zenix lacks is not faked: there is no fork, exec, mmap or sbrk (section 5.5). */
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "usys.h"

static int ret(int r) { return r < 0 ? -1 : r; }        /* errno is already set */

/* ---- files */
/* SYS_open takes no mode: a file it creates gets the kernel's 0666 less the umask, and another mode is set with
 * chmod right after (not atomic: docs/limits.md) */
int open(const char *path, int flags, ...)
{
    struct ustat st;
    int existed = (flags & O_CREAT) ? sys_stat(path, &st) >= 0 : 1, fd;
    if ((fd = sys_open(path, flags)) < 0) return -1;
    if (!existed) {
        va_list ap;
        va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        u32 mask = (u32)sys_umask(0);
        sys_umask(mask);
        if ((mode & 0777) != 0666) sys_chmod(path, mode & 07777 & ~mask);
    }
    return fd;
}
int creat(const char *path, mode_t mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }
int close(int fd) { return ret(sys_close(fd)); }
/* the terminal's line editing in raw mode (termios.c, DESIGN D27 5.4) when a program asked for ICANON without ECHO or
 * ISIG: a pointer, so a program that never sets the terminal links none of it */
ssize_t (*__zx_tty_read)(int fd, void *buf, size_t n);
ssize_t read(int fd, void *buf, size_t n)
{
    if (__zx_tty_read) return __zx_tty_read(fd, buf, n);
    return ret(sys_read(fd, buf, (u32)n));
}
ssize_t write(int fd, const void *buf, size_t n) { return ret(sys_write(fd, buf, (u32)n)); }
off_t lseek(int fd, off_t off, int whence)
{
    if (off != (off_t)(int)off) { errno = EOVERFLOW; return -1; }     /* files are at most 4 GB, offsets 32 bits */
    return ret(sys_lseek(fd, (int)off, whence));
}
int dup(int fd) { return ret(sys_dup(fd)); }
int dup2(int from, int to) { return ret(sys_dup2(from, to)); }
int pipe(int fds[2]) { return ret(sys_pipe(fds)); }
int pipe2(int fds[2], int flags) { return ret(sys_pipe2(fds, flags)); }      /* O_NONBLOCK (both ends) */
int isatty(int fd)
{
    int r = sys_isatty(fd);
    if (r == 1) return 1;
    if (r >= 0) errno = ENOTTY;
    return 0;
}

/* ---- names */
static void fill_stat(struct stat *o, const struct ustat2 *s)
{
    memset(o, 0, sizeof *o);
    o->st_dev = s->dev;                     /* DESIGN D49 8: the mount's instance (0 from an older kernel) */
    o->st_ino = s->ino;
    o->st_mode = s->mode;
    o->st_nlink = s->links;
    o->st_uid = s->uid;
    o->st_gid = s->gid;
    o->st_size = s->size;
    o->st_atim.tv_sec = o->st_mtim.tv_sec = o->st_ctim.tv_sec = s->mtime;   /* one time: the modification's */
    o->st_blksize = 1024;
    o->st_blocks = (s->size + 511) / 512;
}
int stat(const char *path, struct stat *o)
{
    struct ustat2 s;
    if (sys_stat2(path, &s) < 0) return -1;
    fill_stat(o, &s);
    return 0;
}
int lstat(const char *path, struct stat *o)
{
    struct ustat2 s;
    if (sys_lstat2(path, &s) < 0) return -1;
    fill_stat(o, &s);
    return 0;
}
int fstat(int fd, struct stat *o)
{
    struct ustat2 s;
    if (sys_fstat2(fd, &s) < 0) return -1;
    fill_stat(o, &s);
    return 0;
}
int unlink(const char *p) { return ret(sys_unlink(p)); }
int rmdir(const char *p) { return ret(sys_rmdir(p)); }
int rename(const char *a, const char *b) { return ret(sys_rename(a, b)); }
int link(const char *a, const char *b) { return ret(sys_link(a, b)); }
int symlink(const char *t, const char *p) { return ret(sys_symlink(t, p)); }
ssize_t readlink(const char *p, char *buf, size_t n) { return ret(sys_readlink(p, buf, (u32)n)); }
int chmod(const char *p, mode_t m) { return ret(sys_chmod(p, m & 07777)); }
int chown(const char *p, uid_t u, gid_t g) { return ret(sys_chown(p, u, g)); }
int chdir(const char *p) { return ret(sys_chdir(p)); }
char *getcwd(char *buf, size_t n)
{
    if (!buf || !n) { errno = EINVAL; return 0; }       /* no buffer taken for the caller */
    return sys_getcwd(buf, (u32)n) < 0 ? 0 : buf;
}
mode_t umask(mode_t m) { return (mode_t)sys_umask(m & 0777); }
/* SYS_mkdir takes no mode: it is set by chmod after (not atomic: docs/limits.md) */
int mkdir(const char *p, mode_t mode)
{
    if (sys_mkdir(p) < 0) return -1;
    u32 mask = (u32)sys_umask(0);
    sys_umask(mask);
    if ((mode & 07777) != 0777) sys_chmod(p, mode & 07777 & ~mask);
    return 0;
}
/* from the file's mode and the caller's ids, its supplementary groups too (root may do anything but execute a file
 * nobody may execute) */
int access(const char *p, int how)
{
    struct ustat s;
    if (sys_stat(p, &s) < 0) return -1;
    if (how == F_OK) return 0;
    u32 uid = (u32)sys_geteuid(), gid = (u32)sys_getgid(), bits;
    if (uid == 0) bits = (how & X_OK) && !(s.mode & 0111) ? 0 : 7;
    else {
        unsigned short g[NGROUPS];
        int n = sys_getgroups(g, NGROUPS), in = gid == s.gid;
        for (int i = 0; i < n && !in; i++) in = g[i] == s.gid;
        bits = (s.mode >> (uid == s.uid ? 6 : in ? 3 : 0)) & 7;
    }
    if ((how & bits) != (u32)how) { errno = EACCES; return -1; }
    return 0;
}

/* ---- processes */
pid_t getpid(void) { return sys_getpid(); }
uid_t getuid(void) { return (uid_t)sys_getuid(); }
uid_t geteuid(void) { return (uid_t)sys_geteuid(); }
gid_t getgid(void) { return (gid_t)sys_getgid(); }
gid_t getegid(void) { return (gid_t)sys_getgid(); }         /* there is no effective group id */
int setuid(uid_t u) { return ret(sys_setuid(u)); }
int setgid(gid_t g) { return ret(sys_setgid(g)); }
/* the supplementary groups (SYS_groups): at most NGROUPS; size 0 only counts, a smaller room is EINVAL */
int getgroups(int size, gid_t list[])
{
    unsigned short g[NGROUPS];
    int n = sys_getgroups(g, NGROUPS);
    if (n < 0) return -1;
    if (size == 0) return n;
    if (size < n) { errno = EINVAL; return -1; }
    for (int i = 0; i < n; i++) list[i] = g[i];
    return n;
}
int setgroups(int n, const gid_t *list)
{
    unsigned short g[NGROUPS];
    if (n < 0 || n > NGROUPS) { errno = EINVAL; return -1; }
    for (int i = 0; i < n; i++) g[i] = (unsigned short)list[i];
    return ret(sys_setgroups(g, (u32)n));
}
void _exit(int status) { sys_exit(status); __builtin_unreachable(); }

/* ---- time: the wall clock in seconds (SYS_time), the 100 Hz tick since boot (SYS_ticks: never goes back, wraps
 * after 497 days) */
int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) { tv->tv_sec = sys_time(); tv->tv_usec = 0; }     /* the clock has whole seconds */
    return 0;
}
int clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id == CLOCK_REALTIME) { ts->tv_sec = sys_time(); ts->tv_nsec = 0; return 0; }
    if (id == CLOCK_MONOTONIC) { u32 t = sys_ticks(); ts->tv_sec = t / 100; ts->tv_nsec = (long)(t % 100) * 10000000; return 0; }
    errno = EINVAL;
    return -1;
}
u32 __zstart_tick;                      /* _zstart: the tick the program began at */
clock_t clock(void) { return (clock_t)((sys_ticks() - __zstart_tick) * (CLOCKS_PER_SEC / 100)); }    /* wall time */
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000) { errno = EINVAL; return -1; }
    u32 t = (u32)req->tv_sec * 100 + (u32)((req->tv_nsec + 9999999) / 10000000);
    sys_sleep(t);
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}
int usleep(useconds_t us) { sys_sleep((us + 9999) / 10000); return 0; }
unsigned sleep(unsigned s) { sys_sleep(s * 100); return 0; }
