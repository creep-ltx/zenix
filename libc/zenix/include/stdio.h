/* SPDX-License-Identifier: GPL-2.0-only */
/* picolibc's stdio.h, and what tinystdio does not have: POSIX's dprintf and vdprintf (libc/zenix/dprintf.c), popen and
 * pclose (libc/zenix/spawn.c) */
#include_next <stdio.h>
#ifndef _ZENIX_STDIO_H_
#define _ZENIX_STDIO_H_
_BEGIN_STD_C
int dprintf(int fd, const char *__restrict fmt, ...) __PRINTF_ATTRIBUTE__(2, 3);
int vdprintf(int fd, const char *__restrict fmt, __gnuc_va_list ap) __PRINTF_ATTRIBUTE__(2, 0);
FILE *popen(const char *cmd, const char *mode);
int pclose(FILE *f);
_END_STD_C
#endif
