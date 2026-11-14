/* SPDX-License-Identifier: GPL-2.0-only */
/* What libdisplay (display/lib/libdisplay.c) and the sound library (user/sndlib.c) take from libz, for their build
 * inside /bin/lua, a C library program (ports/lua/zenix/lzenix.c; the Makefile's Lua section): the system calls of
 * user/usys.h with the C library's errno and constants, the string calls, and zstrerror over strerror. Their source
 * is compiled unchanged; only this header stands in for user/libz.h. */
#ifndef ZENIX_LUA_LIBZ_SHIM_H
#define ZENIX_LUA_LIBZ_SHIM_H
#define ZENIX_LIBC 1
#include <string.h>
#include <stdlib.h>
#include "usys.h"
#ifndef POLLRDBAND                      /* the C library's <poll.h> has not this one (usys.h's, D38: a post arrived) */
#define POLLRDBAND 0x0080
#endif
static inline const char *zstrerror(int e) { return strerror(e < 0 ? -e : e); }
#endif
