/* SPDX-License-Identifier: GPL-2.0-only */
/* picolibc's sys/stat.h, and the special times of utimensat (POSIX 2008), which picolibc defines only for Cygwin and
 * RTEMS (their values: tv_nsec of the second element picks "now" or "leave alone"; libc/zenix/sys.c utimensat) */
#include_next <sys/stat.h>
#ifndef UTIME_NOW
#define UTIME_NOW  -2L
#define UTIME_OMIT -1L
#endif
