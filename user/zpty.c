/* SPDX-License-Identifier: GPL-2.0-only */
/* Pseudo-terminals (DESIGN D20 7.2): posix_openpt, grantpt and unlockpt (nothing to do: the kernel's /dev/pts/N is
 * there from the master's open, and only root and the pty's owner may open it), ptsname (N from the master's st_ino,
 * PTY_MINO + N), openpty, and setsid (the caller its own process group and, when its descriptor 0 is a terminal
 * nobody leads, that terminal's session leader: SYS_ttyctl's TTYC_SCTTY). One source for libz and the C library
 * (user/zshare.h). */
#include "zshare.h"
#ifdef ZENIX_LIBC
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#endif
#define PTY_MINO_ 0x100                 /* kernel/kernel.h PTY_MINO (frozen with the step) */
#define NPTY_ 8

int posix_openpt(int flags)
{
    int fd = sys_open("/dev/ptmx", flags & (O_RDWR | O_NONBLOCK));
    return fd < 0 ? -1 : fd;
}
static int pty_num(int fd)
{
    struct ustat st;
    if (sys_fstat(fd, &st) < 0)
        return -1;
    if ((st.mode & 0170000) != 020000 || st.ino < PTY_MINO_ || st.ino >= PTY_MINO_ + NPTY_) {
        zerrno = EINVAL;                /* not a pty's master */
        return -1;
    }
    return (int)(st.ino - PTY_MINO_);
}
int grantpt(int fd) { return pty_num(fd) < 0 ? -1 : 0; }
int unlockpt(int fd) { return pty_num(fd) < 0 ? -1 : 0; }
int ptsname_r(int fd, char *buf, zs_size len)
{
    int n = pty_num(fd);
    if (n < 0)
        return zerrno;
    if (!buf || len < 11)
        return zerrno = ERANGE;
    memcpy(buf, "/dev/pts/", 9);
    buf[9] = (char)('0' + n);
    buf[10] = 0;
    return 0;
}
char *ptsname(int fd)
{
    static char name[11];
    return ptsname_r(fd, name, sizeof name) ? 0 : name;
}
/* BSD's and glibc's openpty: a master and its slave, both read and write; the name (at least 11 bytes) and a size
 * if asked; termp is ignored (Zenix's ttys have no termios state to set: the slave starts cooked) */
int openpty(int *amaster, int *aslave, char *name, const void *termp, const struct winsize *winp)
{
    (void)termp;
    char nm[11];
    int m = posix_openpt(O_RDWR);
    if (m < 0)
        return -1;
    if (ptsname_r(m, nm, sizeof nm)) {
        sys_close(m);
        return -1;
    }
    int s = sys_open(nm, O_RDWR);
    if (s < 0) {
        sys_close(m);
        return -1;
    }
    if (winp && sys_ttyctl(m, TTYC_SWINSZ, (u32)winp) < 0) {
        sys_close(s);
        sys_close(m);
        return -1;
    }
    if (name)
        memcpy(name, nm, sizeof nm);
    *amaster = m;
    *aslave = s;
    return 0;
}
int setsid(void)
{
    int me = sys_getpid();
    if (sys_setpgid(0, 0) < 0)
        return -1;
    sys_ttyctl(0, TTYC_SCTTY, 0);       /* not a terminal, or led by someone else: only the group, as D20 7.2 says */
    return me;
}
