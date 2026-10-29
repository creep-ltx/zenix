/* SPDX-License-Identifier: GPL-2.0-only */
/* picolibc's regex.h (FreeBSD's) uses size_t without including its header: this one does, then that one
 * (libc/zenix/regcomp.c: the C library's regcomp over user/regex.c) */
#include <stddef.h>
#include_next <regex.h>
