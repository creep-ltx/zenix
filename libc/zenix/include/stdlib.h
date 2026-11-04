/* SPDX-License-Identifier: GPL-2.0-only */
/* picolibc's stdlib.h, and the pseudo-terminal calls it does not declare (POSIX's, user/zpty.c, DESIGN D20 7.2);
 * grantpt is picolibc's own declaration */
#include_next <stdlib.h>
#ifndef _ZENIX_STDLIB_H_
#define _ZENIX_STDLIB_H_
_BEGIN_STD_C
int posix_openpt(int flags);
int unlockpt(int fd);
char *ptsname(int fd);
int ptsname_r(int fd, char *buf, size_t len);
_END_STD_C
#endif
