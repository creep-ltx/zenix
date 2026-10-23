/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Environment for kernel/ext2.c when it is built into the filesystem server. */
#ifdef ZHOST
#include "../tools/host/zhost.h"     /* host build for the tests (tools/ext2-host-test.sh) */
#else
#include "libz.h"
typedef unsigned char u8;
typedef unsigned short u16;
typedef int s32;
#endif
#include "../kernel/ext2.h"

static inline void *fs_alloc(u32 n) { return malloc(n); }
static inline void fs_free(void *p) { free(p); }
static inline u32 fs_ticks(void) { return sys_ticks(); }
static inline u32 fs_time(void) { return (u32)sys_time(); }
int fs_blk_read(u32 lba, u32 count, void *buf);
int fs_blk_write(u32 lba, u32 count, const void *buf);
#define fs_log printf
