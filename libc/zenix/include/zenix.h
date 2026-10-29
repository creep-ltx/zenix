/* SPDX-License-Identifier: GPL-2.0-only */
/* Zenix's own helpers for C library programs (DESIGN D27 section 6, libzenix): the buffered standard output (a
 * 4 KB buffer for descriptor 1, written when full and at exit: one system call per screen) and the error texts.
 * A program uses oputs or stdout's FILE functions, never both (two buffers would mix their order). */
#ifndef _ZENIX_H_
#define _ZENIX_H_
void oputs(const char *s);
void oputn(const char *s, unsigned n);
void oputc(int c);
void oflush(void);
const char *zstrerror(int e);           /* an error number's text (either sign), 0 if none */
#endif
