/* SPDX-License-Identifier: GPL-2.0-only */
/* The C library's error numbers are Zenix's (DESIGN D27 section 5): kernel/errno.h, the one table, ahead of
 * picolibc's own sys/errno.h (newlib's numbers, some of which differ). errno is a plain int (one thread). */
#ifndef _SYS_ERRNO_H_
#define _SYS_ERRNO_H_
#include <sys/cdefs.h>
#include "../../../../kernel/errno.h"
_BEGIN_STD_C
#if __GNU_VISIBLE
char *_user_strerror(int errnum, int internal, int *errptr);
#endif
#define __THREAD_LOCAL_ERRNO
extern int errno;
#define errno errno
#define __ELASTERROR 2000
_END_STD_C
#endif
