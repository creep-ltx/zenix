/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* What ZINE's plain C cores share (docs/d29-zine.md, step 1): fixed-size types and big-endian access. The cores
 * (hunk.c, zvol.c, zplan.c, lvo.c) use no C library at all, so the same file builds into /bin/zine, into the blob
 * (freestanding, no libz) and for the PC (the host tests, tools/host/zine-test.sh). */
#include <stdint.h>

typedef uint8_t zu8;
typedef uint16_t zu16;
typedef uint32_t zu32;

static inline zu32 zbe32(const zu8 *p)
{
    return (zu32)p[0] << 24 | (zu32)p[1] << 16 | (zu32)p[2] << 8 | p[3];
}

static inline zu32 zbe16(const zu8 *p)
{
    return (zu32)p[0] << 8 | p[1];
}

static inline void zput32(zu8 *p, zu32 v)
{
    p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

/* exec's memory attributes (exec/memory.h, RKRM Includes and Autodocs) */
#define ZMEMF_PUBLIC 1u
#define ZMEMF_CHIP   2u
#define ZMEMF_FAST   4u

/* errno numbers as Zenix's (kernel/errno.h; negative in the cores' answers) */
#define ZE_NOENT  2
#define ZE_NOEXEC 8
#define ZE_NOMEM  12
#define ZE_NODEV  19
#define ZE_NOTDIR 20
#define ZE_INVAL  22
#define ZE_NOSPC  28
#define ZE_NAMETOOLONG 36
