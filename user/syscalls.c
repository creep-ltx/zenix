/* newlib glue: the m68k port expects plain POSIX-named syscalls
 * (its reent layer calls close/read/write/..., not _close/...). */
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include "usys.h"

ssize_t write(int fd, const void *buf, size_t n)
{
    long r = sys_write(fd, buf, (long)n);
    if (r < 0) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)r;
}

ssize_t read(int fd, void *buf, size_t n)
{
    long r = sys_read(fd, buf, (long)n);
    if (r < 0) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)r;
}

int open(const char *path, int flags, ...)
{
    long mode = (flags & (O_WRONLY | O_RDWR)) ? 1 : 0;
    long r = sys_open(path, mode);
    if (r < 0) {
        errno = ENOENT;
        return -1;
    }
    return (int)r;
}

int close(int fd)
{
    return (int)sys_close(fd);
}

off_t lseek(int fd, off_t off, int whence)
{
    long r = __sys(SYS_lseek, fd, (long)off, whence);
    if (r < 0) {
        errno = EINVAL;
        return -1;
    }
    return (off_t)r;
}

int fstat(int fd, struct stat *st)
{
    st->st_mode = fd < 3 ? S_IFCHR : S_IFREG;
    st->st_blksize = 512;
    return 0;
}

int stat(const char *path, struct stat *st)
{
    (void)path;
    st->st_mode = S_IFREG;
    return 0;
}

int isatty(int fd)
{
    return fd < 3;
}

void *sbrk(ptrdiff_t incr)
{
    static char *cur;
    if (!cur)
        cur = (char *)sys_brk(0);
    char *old = cur;
    if (sys_brk((long)(cur + incr)) < 0) {
        errno = ENOMEM;
        return (void *)-1;
    }
    cur += incr;
    return old;
}

void _exit(int code)
{
    sys_exit(code);
    for (;;)
        ;
}

int getpid(void)
{
    return (int)sys_getpid();
}

int kill(int pid, int sig)
{
    if (sys_kill(pid, sig) < 0) {
        errno = ESRCH;
        return -1;
    }
    return 0;
}

/* kernel-backed signal()/raise(): replaces newlib's software-only
 * versions (all symbols of its signal.o must be covered so the archive
 * member never gets pulled in) */
typedef void (*__sig_t)(int);
__sig_t signal(int sig, __sig_t handler)
{
    long old = sys_signal(sig, handler);
    if (old < 0) {
        errno = EINVAL;
        return (__sig_t)-1;
    }
    return (__sig_t)old;
}

int raise(int sig)
{
    return kill(getpid(), sig);
}

struct _reent;
__sig_t _signal_r(struct _reent *re, int sig, __sig_t handler)
{
    (void)re;
    return signal(sig, handler);
}

int _raise_r(struct _reent *re, int sig)
{
    (void)re;
    return raise(sig);
}

int _init_signal_r(struct _reent *re)
{
    (void)re;
    return 0;
}

int __sigtramp_r(struct _reent *re, int sig)
{
    (void)re;
    (void)sig;
    return -1;
}

int unlink(const char *path)
{
    (void)path;
    errno = ENOENT;
    return -1;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        tv->tv_sec = 0;
        tv->tv_usec = 0;
    }
    return 0;
}
