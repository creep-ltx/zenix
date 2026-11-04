/* SPDX-License-Identifier: GPL-2.0-only */
/* openpty (glibc's <pty.h>, user/zpty.c): a pseudo-terminal's master and slave (DESIGN D20 7.2) */
#ifndef _ZENIX_PTY_H_
#define _ZENIX_PTY_H_
#include <sys/cdefs.h>
#include <sys/ioctl.h>
_BEGIN_STD_C
int openpty(int *amaster, int *aslave, char *name, const void *termp, const struct winsize *winp);
_END_STD_C
#endif
