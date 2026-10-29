/* SPDX-License-Identifier: GPL-2.0-only */
/* picolibc's stdio.h, and POSIX's dprintf and vdprintf, which tinystdio does not have (libc/zenix/dprintf.c) */
#include_next <stdio.h>
#ifndef _ZENIX_STDIO_H_
#define _ZENIX_STDIO_H_
_BEGIN_STD_C
int dprintf(int fd, const char *__restrict fmt, ...) __PRINTF_ATTRIBUTE__(2, 3);
int vdprintf(int fd, const char *__restrict fmt, __gnuc_va_list ap) __PRINTF_ATTRIBUTE__(2, 0);
_END_STD_C
#endif
