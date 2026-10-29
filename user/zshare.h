/* SPDX-License-Identifier: GPL-2.0-only */
/* The files libz and the C library share (DESIGN D27 section 6 step 1: one source each, compiled into both): the
 * allocator (zmalloc.c), the error texts (zerrtab.c), the buffered output (zout.c) and the environment (zenv.c).
 * Built into libz (user/libz.h's world) or, with ZENIX_LIBC, into the C library (picolibc's headers, libc/zenix). */
#pragma once
#ifdef ZENIX_LIBC
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "usys.h"
#define ZS_LIB "libc"
typedef size_t zs_size;                 /* malloc's argument */
typedef size_t zs_ulong;                /* calloc's and realloc's */
void zenv_load(void);
const char *zstrerror(int e);
const char *zwhy(const char *dflt);
void oputs(const char *s);
void oputn(const char *s, u32 n);
void oputc(int c);
void oflush(void);
#else
#include "libz.h"
#define ZS_LIB "libz"
typedef u32 zs_size;
typedef unsigned long zs_ulong;
void zenv_load(void);
#endif
extern void (*zout_at_exit)(void);
/* the allocator's report of a bad pointer (zmalloc.c): libz formats it; the C library writes it without the formatter,
 * so a program that only allocates does not link printf */
void zs_badptr(const char *who, const void *p);      /* zout.c: set by the first buffered write; the exit calls it */
