/* SPDX-License-Identifier: GPL-2.0-only */
/* Included before the BSD games' files when the PC builds them (the Makefile's -include: adventure's mkdata,
 * fortune's strfile; tools/games-host-test.sh: adventure, banner, number): the NetBSD macros and getprogname the PC's
 * C library (glibc) lacks, and the headers NetBSD's bring with them. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <time.h>
#define __RCSID(s)              struct __rcsid_unused
#define __COPYRIGHT(s)          struct __copyright_unused
#define __dead                  __attribute__((__noreturn__))
#define __printflike(a, b)      __attribute__((__format__(__printf__, a, b)))
#define __noinline              __attribute__((__noinline__))
#define __unused                __attribute__((__unused__))
#define __arraycount(a)         (sizeof(a) / sizeof((a)[0]))
#define getprogname()           program_invocation_short_name
