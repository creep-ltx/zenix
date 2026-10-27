/* SPDX-License-Identifier: GPL-2.0-only */
/* lwIP's compiler and C library glue for netd (DESIGN D19 2.2). Two builds use it: netd for Zenix (m68k, libz, the
 * stand-in C headers in net/port/shim) and the host build of the tests (NETD_HOST: the PC's C library). */
#ifndef ZENIX_LWIP_ARCH_CC_H
#define ZENIX_LWIP_ARCH_CC_H

#ifdef NETD_HOST
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define LWIP_NO_CTYPE_H 1
#else
/* Zenix: no C library headers but libz's; the types are defined here */
#define LWIP_NO_STDDEF_H 1
#define LWIP_NO_STDINT_H 1
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_LIMITS_H 1
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_CTYPE_H 1
#include "../../../user/libz.h"   /* size_t (unsigned long), memcpy, memset, memcmp, strlen, strncmp */
typedef unsigned char u8_t;
typedef signed char s8_t;
typedef unsigned short u16_t;
typedef signed short s16_t;
typedef unsigned long u32_t;
typedef signed long s32_t;
typedef unsigned long mem_ptr_t;
typedef long ptrdiff_t;
#define LWIP_HAVE_INT64 0
#ifndef NULL
#define NULL ((void *)0)
#endif
#define INT_MAX 0x7fffffff
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "lu"
#define S32_F "ld"
#define X32_F "lx"
#define SZT_F "lu"
#endif

/* m68k is big-endian; the PC is little-endian (lwIP's default byte swaps are used there) */
#ifndef BYTE_ORDER
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BYTE_ORDER LITTLE_ENDIAN
#else
#define BYTE_ORDER BIG_ENDIAN
#endif
#endif

/* Every check stays, but a failed one reports only its line number: no message strings (D19 2.2, 7 KB). netd logs
 * the line and ends; the service manager starts it again (D11). Debug output is never compiled in. */
void netd_lwip_assert(int line);
#define LWIP_PLATFORM_ASSERT(x) netd_lwip_assert(__LINE__)
#define LWIP_PLATFORM_DIAG(x) do { } while (0)

/* The random numbers for lwIP (DNS ids and ports, ephemeral ports): netd's ChaCha20 (D19 9.3, net/port/rand.c) */
#ifdef NETD_HOST
uint32_t netd_rand(void);
#else
u32_t netd_rand(void);
#endif
#define LWIP_RAND() netd_rand()

#endif
