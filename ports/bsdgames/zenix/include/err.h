/* SPDX-License-Identifier: GPL-2.0-only */
/* BSD's <err.h> for the BSD games (ports/bsdgames/zenix/compat.c): the program's name, the message, and for err and
 * warn the error's text, on standard error; err and errx then exit with the status given. */
#ifndef ZGAMES_ERR_H
#define ZGAMES_ERR_H
#include <stdarg.h>
void err(int, const char *, ...) __attribute__((__noreturn__, __format__(__printf__, 2, 3)));
void errx(int, const char *, ...) __attribute__((__noreturn__, __format__(__printf__, 2, 3)));
void verr(int, const char *, va_list) __attribute__((__noreturn__));
void verrx(int, const char *, va_list) __attribute__((__noreturn__));
void warn(const char *, ...) __attribute__((__format__(__printf__, 1, 2)));
void warnx(const char *, ...) __attribute__((__format__(__printf__, 1, 2)));
void vwarn(const char *, va_list);
void vwarnx(const char *, va_list);
#endif
