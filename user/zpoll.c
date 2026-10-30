/* SPDX-License-Identifier: GPL-2.0-only */
/* Waiting for several things (DESIGN D16): poll over SYS_evwait (descriptors and, with POLLPORT, ports; the timeout
 * in 10 ms ticks, rounded up) and select over poll. One source for libz and the C library (user/zshare.h). */
#include "zshare.h"
_Static_assert(sizeof(struct pollfd) == 8 && __builtin_offsetof(struct pollfd, revents) == 6, "struct pollfd is the kernel's");

int poll(struct pollfd *f, zs_nfds n, int ms)
{
    int r = sys_evwait(f, n, ms < 0 ? -1 : (ms + 9) / 10);
    return r < 0 ? -1 : r;
}
/* a process has at most NFD descriptors: the C library's FD_SETSIZE (64) would put 512 bytes on the stack (audit B3-16) */
#ifdef ZX_NFD
#define ZS_NFD ZX_NFD
#else
#define ZS_NFD FD_SETSIZE
#endif
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    struct pollfd p[ZS_NFD];
    u32 n = 0;
    (void)ex;
    for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
        short ev = (short)((rd && FD_ISSET(fd, rd) ? POLLIN : 0) | (wr && FD_ISSET(fd, wr) ? POLLOUT : 0));
        if (!ev) continue;
        if (fd >= ZS_NFD) { zerrno = EBADF; return -1; }    /* (no such descriptor can be open) */
        p[n].fd = fd; p[n].events = ev; p[n].revents = 0; n++;
    }
    if (tv && (tv->tv_sec < 0 || tv->tv_usec < 0)) { zerrno = EINVAL; return -1; }
    /* the timeout in ms, at most about 24 days (a larger one is that, not a wrap into "for ever": B3-16) */
    int ms = !tv ? -1 : tv->tv_sec >= 2000000 ? 2000000000 : (int)tv->tv_sec * 1000 + (int)(tv->tv_usec / 1000);
    int r = poll(p, n, ms);
    if (r < 0) return -1;
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);
    int cnt = 0;
    for (u32 i = 0; i < n; i++) {
        int hit = 0;
        if (rd && (p[i].revents & (POLLIN | POLLHUP | POLLERR))) { FD_SET(p[i].fd, rd); hit = 1; }
        if (wr && (p[i].revents & (POLLOUT | POLLERR))) { FD_SET(p[i].fd, wr); hit = 1; }
        if (p[i].revents & POLLNVAL) { zerrno = EBADF; return -1; }
        cnt += hit;
    }
    return cnt;
}
