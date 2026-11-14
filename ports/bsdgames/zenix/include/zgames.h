/* SPDX-License-Identifier: GPL-2.0-only */
/* Included before every file of the BSD games (the Makefile's -include; ports/bsdgames/README): the NetBSD
 * <sys/cdefs.h> macros picolibc's does not have, as NetBSD defines them (the RCS ids and copyright strings dropped:
 * they would only take room), and the BSD library calls Zenix's C library lacks (ports/bsdgames/zenix/compat.c). */
#ifndef ZGAMES_H
#define ZGAMES_H
#include <sys/cdefs.h>
#include <sys/types.h>          /* NetBSD's <stdlib.h> and <unistd.h> bring these with them */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#define __RCSID(s)              struct __rcsid_unused
#define __COPYRIGHT(s)          struct __copyright_unused
#ifndef __dead
#define __dead                  __attribute__((__noreturn__))
#endif
#ifndef __printflike
#define __printflike(a, b)      __attribute__((__format__(__printf__, a, b)))
#endif
#ifndef __unused
#define __unused                __attribute__((__unused__))
#endif
#ifndef __arraycount
#define __arraycount(a)         (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef __UNCONST
#define __UNCONST(a)            ((void *)(uintptr_t)(const void *)(a))
#endif
#include <machine/endian.h>     /* tetris: NetBSD's <sys/types.h> brings bswap32 and bswap64 (picolibc's words) */
#ifndef bswap32
#define bswap32(x)              __bswap32(x)
#define bswap64(x)              __bswap64(x)
#endif
#ifndef BE64TOH                 /* NetBSD's in-place byte swaps */
#define BE32TOH(x)              ((x) = be32toh(x))
#define BE64TOH(x)              ((x) = be64toh(x))
#define HTOBE32(x)              ((x) = htobe32(x))
#define HTOBE64(x)              ((x) = htobe64(x))
#endif
#ifndef OXTABS                  /* tetris: NetBSD termios's tabs-to-spaces output flag (Zenix has none) */
#define OXTABS                  0
#endif
#ifndef INFTIM                  /* tetris: poll's "no time-out" */
#define INFTIM                  (-1)
#endif
#ifndef TCIFLUSH                /* robots: tcflush (compat.c: the input waiting is read and dropped) */
#define TCIFLUSH                0
#define TCOFLUSH                1
#define TCIOFLUSH               2
#endif
int tcflush(int, int);
#define NAMLEN(d)               strlen((d)->d_name)     /* fortune: BSD's dirent has d_namlen */
const char *getprogname(void);
void setprogname(const char *);
uint32_t arc4random(void);
uint32_t arc4random_uniform(uint32_t);
int flock(int, int);
char *getlogin(void);
int setegid(gid_t);
int ftruncate(int, off_t);
#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#endif
#endif
