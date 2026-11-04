/* SPDX-License-Identifier: GPL-2.0-only */
/* The files libz and the C library share (DESIGN D27 section 6 step 1: one source each, compiled into both): the
 * allocator (zmalloc.c), the error texts (zerrtab.c), the buffered output (zout.c), the environment (zenv.c), the
 * signals (zsig.c), waiting for several things (zpoll.c), the sockets (zsock.c), the helpers for paths, directories
 * and colours (zhelp.c), the user and group parser (zpw.c) and the pseudo-terminals (zpty.c). Built into libz (user/libz.h's world) or, with
 * ZENIX_LIBC, into the C library (picolibc's headers, libc/zenix; the helpers' prototypes are libzenix's, zenix.h). */
#pragma once
#ifdef ZENIX_LIBC
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>
#include "usys.h"
#include <zenix.h>
#define ZS_LIB "libc"
typedef size_t zs_size;                 /* malloc's argument, a length */
typedef size_t zs_ulong;                /* calloc's and realloc's */
typedef nfds_t zs_nfds;                 /* poll's count */
void zenv_load(void);
#else
#include "libz.h"
#define ZS_LIB "libz"
typedef size_t zs_size;
typedef size_t zs_ulong;
typedef u32 zs_nfds;
void zenv_load(void);
#endif
extern void (*zout_at_exit)(void);      /* zout.c: set by the first buffered write; the exit calls it */
/* the allocator's report of a bad pointer (zmalloc.c): libz formats it; the C library writes it without the formatter,
 * so a program that only allocates does not link printf */
void zs_badptr(const char *who, const void *p);
/* a basic regular expression in the extended syntax regex.c speaks (zhelp.c): malloc'd, 0 when out of memory (libz's
 * bre_to_ere ends the program then; the C library's regcomp answers REG_ESPACE) */
char *zs_bre_to_ere(const char *bre);
/* the user and group parser (zpw.c): one copy for libz's pw_* and the C library's getpw* */
#include "zpw.h"
