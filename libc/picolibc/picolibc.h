/* SPDX-License-Identifier: BSD-3-Clause */
/* picolibc 1.8.10's configuration for Zenix: the header picolibc's meson build would write (DESIGN D27 section 4).
 * Written by hand from meson.build's conf_data and meson_options.txt, with Zenix's choices: tiny stdio, one thread,
 * no thread-local storage, a global errno, long long in printf, UTF-8 only, no picocrt, no semihosting, no POSIX
 * console (the port's own streams), no fortify, verbose assert. */
#pragma once
#define __FAST_STRCMP 1
#define __GLOBAL_ERRNO 1
#define __HAVE_BITFIELDS_IN_PACKED_STRUCTS 1
#define __HAVE_CC_INHIBIT_LOOP_TO_LIBCALL 1
#define __HAVE_COMPLEX 1
#define __IEEE_LIBM 1
#define __INIT_FINI_ARRAY 1
#define __IO_C99_FORMATS 1
#define __IO_DEFAULT 'd'
#define __IO_FLOAT_EXACT 1
#define __IO_LONG_LONG 1
#define __IO_SMALL_ULTOA 1
#define __MB_CAPABLE 1
#define __ASSERT_VERBOSE 1
#define __PICO_EXIT 1
#define __SINGLE_THREAD 1
#define __TINY_STDIO 1
#define _NEWLIB_VERSION "4.3.0"
#define __NEWLIB_VERSION__ "4.3.0"
#define __NEWLIB__ 4
#define __NEWLIB_MINOR__ 3
#define __NEWLIB_PATCHLEVEL__ 0
#define _PICOLIBC_VERSION "1.8.10"
#define _PICOLIBC__ 1
#define _PICOLIBC_MINOR__ 8
#define __PICOLIBC_VERSION__ "1.8.10"
#define __PICOLIBC__ 1
#define __PICOLIBC_MINOR__ 8
#define __PICOLIBC_PATCHLEVEL__ 10
/* Zenix (not a meson option): CLOCK_MONOTONIC exists, from the 100 Hz tick (DESIGN D27 section 5) */
#define _POSIX_MONOTONIC_CLOCK 200112L
