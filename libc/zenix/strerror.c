/* SPDX-License-Identifier: GPL-2.0-only */
/* strerror: the texts of libz's table (user/zerrtab.c), so a program says the same words with either library;
 * picolibc's strerror_r and strerror_s call _strerror_r */
#include <errno.h>
#include <locale.h>
#include <string.h>
#include "zshare.h"

char *_strerror_r(int errnum, int internal, int *errptr)
{
    static char unknown[] = "unknown error -2147483648";
    (void)internal;
    if (errnum == 0) return (char *)"success";
    const char *t = zstrerror(errnum);
    if (t) return (char *)t;
    if (errptr) *errptr = EINVAL;
    char *p = unknown + 14;             /* after "unknown error " (no formatter pulled in for this) */
    unsigned v = errnum < 0 ? 0u - (unsigned)errnum : (unsigned)errnum;
    char d[11]; int n = 0;
    do d[n++] = (char)('0' + v % 10); while (v /= 10);
    if (errnum < 0) *p++ = '-';
    while (n) *p++ = d[--n];
    *p = 0;
    return unknown;
}
char *strerror(int errnum) { return _strerror_r(errnum, 0, 0); }
char *strerror_l(int errnum, locale_t l) { (void)l; return _strerror_r(errnum, 0, 0); }
