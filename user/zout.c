/* SPDX-License-Identifier: GPL-2.0-only */
/* Zenix's buffered standard output (oputs, oputn, oputc, oflush), one source for libz and the C library
 * (user/zshare.h). A program uses it or the C library's stdout, never both (DESIGN D27 section 5.2). */
#include "zshare.h"

/* The 4 KB buffer is linked only into programs that use it (audit O12): the exit flushes it through zout_at_exit, a
 * pointer the first oputn sets, so a program that never buffers (diskd, wasabid, nfscd, any printf-only one) names
 * neither the buffer nor oflush. */
static char obuf[4096]; static u32 olen;
void (*zout_at_exit)(void);
void oflush(void) { if (olen) { int r = sys_write(1, obuf, olen); olen = 0; if (r < 0) sys_exit(141); } }   /* the reader is gone: end, as SIGPIPE would */
void oputn(const char *s, u32 n)
{
    zout_at_exit = oflush;
    if (n >= sizeof obuf) { oflush(); if (sys_write(1, s, n) < 0) sys_exit(141); return; }
    if (olen + n > sizeof obuf) oflush();
    memcpy(obuf + olen, s, n); olen += n;
}
void oputs(const char *s) { oputn(s, strlen(s)); }
void oputc(int c) { char b = (char)c; oputn(&b, 1); }
