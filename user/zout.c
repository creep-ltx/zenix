/* SPDX-License-Identifier: GPL-2.0-only */
/* Zenix's buffered standard output (oputs, oputn, oputc, oflush), one source for libz and the C library
 * (user/zshare.h). A program uses it or the C library's stdout, never both (DESIGN D27 section 5.2). */
#include "zshare.h"
const char *zstrerror(int e);           /* zerrtab.c */

/* The 4 KB buffer is linked only into programs that use it (audit O12): the exit flushes it through zout_at_exit, a
 * pointer the first oputn sets, so a program that never buffers (diskd, wasabid, nfscd, any printf-only one) names
 * neither the buffer nor oflush. */
static char obuf[4096]; static u32 olen;
void (*zout_at_exit)(void);
/* all of it written, or the program ends: EPIPE (the reader is gone) with status 141, as SIGPIPE would; any other
 * failure (a full disk) says so on standard error and ends with status 1, never a silent cut or a silent 141 */
static void owrite(const char *s, u32 n)
{
    while (n) {
        int r = sys_write(1, s, n);
        if (r == -EINTR)
            continue;
        if (r == -EPIPE)
            sys_exit(141);
        if (r <= 0) {
            const char *t = r < 0 ? zstrerror(r) : 0;
            if (!t) t = "nothing written";
            sys_write(2, "write error on standard output: ", 32); sys_write(2, t, strlen(t)); sys_write(2, "\n", 1);
            sys_exit(1);
        }
        s += r; n -= (u32)r;
    }
}
void oflush(void) { if (olen) { u32 n = olen; olen = 0; owrite(obuf, n); } }
void oputn(const char *s, u32 n)
{
    zout_at_exit = oflush;
    if (n >= sizeof obuf) { oflush(); owrite(s, n); return; }
    if (olen + n > sizeof obuf) oflush();
    memcpy(obuf + olen, s, n); olen += n;
}
void oputs(const char *s) { oputn(s, strlen(s)); }
void oputc(int c) { char b = (char)c; oputn(&b, 1); }
