/* SPDX-License-Identifier: GPL-2.0-only */
/* tcc's build settings for Zenix (ports/tcc/README): both builds, the PC's cross compiler and /bin/tcc. Upstream's
 * configure writes this file; Zenix's Makefile copies this one beside the sources instead. */
#define TCC_VERSION "0.9.28rc"
#define CONFIG_TCC_PREDEFS 1
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCCDIR "/usr/lib/tcc"
/* tcc's own headers, then the C library's: the port's in /usr/include, picolibc's under /usr/include/pico (the
   order of the Makefile's LIBC_INC, for #include_next) */
#define ZX_INC "{B}/../../include"
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include:" ZX_INC ":" ZX_INC "/pico/gen:" ZX_INC "/pico/m68k:" \
    ZX_INC "/pico/stdio:" ZX_INC "/pico/locale:" ZX_INC "/pico:" ZX_INC "/pico/include:" ZX_INC "/pico/libm"
#define CONFIG_TCC_LIBPATHS "{B}/.."
#define CONFIG_TCC_CRTPREFIX "{B}/.."
/* no dlopen (Zenix has no shared libraries) */
#define CONFIG_TCC_STATIC 1
/* memory (the minimum is 4 MB): the tokens' allocators grow 32 KB at a time and the identifiers' hash table has 4096
   slots (upstream: 256 KB each up front, 16384 slots) */
#define TOKSYM_TAL_SIZE (32 * 1024)
#define TOKSTR_TAL_SIZE (32 * 1024)
#define TOK_HASH_SIZE 4096
