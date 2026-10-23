/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Environment for zenixfs/zenixfs.c when it is built into a Zenix program (-DZENIXFS_FREESTANDING): the few C library
 * functions it needs, declared by hand because the library source must not see libz.h (whose u32 differs from uint32_t).
 * malloc/free come from user/libz.c. The library allocates only while mounting and in mkfs/fsck, never while a
 * server is running, so a running server needs no more memory. */
void *memcpy(void *, const void *, __SIZE_TYPE__);
void *memset(void *, int, __SIZE_TYPE__);
void *memmove(void *, const void *, __SIZE_TYPE__);
int memcmp(const void *, const void *, __SIZE_TYPE__);
__SIZE_TYPE__ strlen(const char *);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, __SIZE_TYPE__);
void *malloc(__SIZE_TYPE__);
void free(void *);
#define ZENIXFS_MALLOC(n) malloc(n)
#define ZENIXFS_FREE(p)   free(p)
