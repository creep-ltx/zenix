/* SPDX-License-Identifier: GPL-2.0-only */
/* poll (DESIGN D16, user/zpoll.c): struct pollfd is the kernel's (SYS_evwait reads it: user/usys.h, frozen; zpoll.c
 * checks the layout), the bits from the kernel's headers; POLLPORT (Zenix): fd is a port this process owns. The
 * timeout counts in the system's 10 ms ticks, rounded up. */
#ifndef _ZENIX_POLL_H_
#define _ZENIX_POLL_H_
#include <sys/cdefs.h>
#include <zenix-consts.h>
_BEGIN_STD_C
struct pollfd { int fd; short events, revents; };
typedef unsigned long nfds_t;
#define POLLIN   ZX_POLLIN
#define POLLPRI  ZX_POLLPRI
#define POLLOUT  ZX_POLLOUT
#define POLLERR  ZX_POLLERR
#define POLLHUP  ZX_POLLHUP
#define POLLNVAL ZX_POLLNVAL
#define POLLPORT ZX_POLLPORT
int poll(struct pollfd *fds, nfds_t n, int ms);
_END_STD_C
#endif
