/* SPDX-License-Identifier: GPL-2.0-only */
/* Zenix's limits (DESIGN D27 section 5, docs/limits.md) over picolibc's sys/syslimits.h: paths of at most 63 bytes,
 * names of 63, 16 descriptors per process, 8 KB of spawn arguments. */
#ifndef _ZENIX_SYSLIMITS_H_
#define _ZENIX_SYSLIMITS_H_
#include_next <sys/syslimits.h>
#include <zenix-consts.h>
#undef ARG_MAX
#define ARG_MAX   8192
#undef OPEN_MAX
#define OPEN_MAX  ZX_NFD
#undef NAME_MAX
#define NAME_MAX  63
#undef PATH_MAX
#define PATH_MAX  256
#endif
