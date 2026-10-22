#pragma once
/* Environment for kernel/ext2.c when it is built into the filesystem server. */
#include "libz.h"
typedef unsigned char u8;
typedef unsigned short u16;
typedef int s32;
#include "../kernel/ext2.h"

static inline void *fs_alloc(u32 n) { return malloc(n); }
static inline u32 fs_ticks(void) { return sys_ticks(); }
int fs_blk_read(u32 lba, u32 count, void *buf);
int fs_blk_write(u32 lba, u32 count, const void *buf);
#define fs_log printf
