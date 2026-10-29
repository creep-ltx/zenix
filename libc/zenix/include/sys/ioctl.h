/* SPDX-License-Identifier: GPL-2.0-only */
/* ioctl (DESIGN D27 5.4, user/zsock.c): TIOCGWINSZ, the console's size (ENOTTY when the descriptor is not a terminal);
 * FIONREAD and FIONBIO on a socket (DESIGN D19 4.4), their numbers the kernel's (Linux's). Nothing else. */
#ifndef _ZENIX_SYS_IOCTL_H_
#define _ZENIX_SYS_IOCTL_H_
#include <sys/cdefs.h>
#include <zenix-consts.h>
_BEGIN_STD_C
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define TIOCGWINSZ 0x5413               /* Linux's number (the C library's own, not a system call's) */
#ifndef FIONREAD                        /* (the C library's own zsock.c has the kernel's netproto.h first) */
#define FIONREAD   ZX_FIONREAD
#define FIONBIO    ZX_FIONBIO
#endif
int ioctl(int fd, unsigned long req, ...);
_END_STD_C
#endif
