/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Host build of Zenix filesystem code (kernel/ext2.c, user/fsd.c, user/fsck_ext2.c, user/mkfs_ext2.c): the few libz
 * and system-call pieces they use, served from an image file (tools/host/hostdisk.c). Build with -m32 -DZHOST: the
 * Zenix code passes pointers in 32-bit message words. Used only by the host tests (tools/ext2-host-test.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/errno.h"        /* the Zenix error numbers (the same values as Linux) */
/* Zenix's dprintf(fd, ...) (DESIGN D27 6 step 2) through the host's streams, so a test's lines keep their order with
 * the harness's own printf: descriptor 2 is stderr, any other stdout */
#include <stdarg.h>
static inline int zhost_dprintf(int fd, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(fd == 2 ? stderr : stdout, fmt, ap);
    va_end(ap);
    return n;
}
#define dprintf zhost_dprintf

typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef int s32;

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400

struct umsg { struct umsg *next; int from, server; u32 state; int result; u32 type; u32 a[4]; unsigned short uid, gid, umask, kflags; u32 client_; };
#define UMSG_KERNEL 1

int sys_call(int port, struct umsg *m);          /* hostdisk.c: the disk server's messages on an image file */
int sys_port_find(const char *name);
int sys_port_create(const char *name);
struct umsg *sys_recv(int port, int wait);
struct umsg *sys_replyrecv(struct umsg *m, int r, int port);
int sys_reply(struct umsg *m, int r);
void sys_sleep(u32 ticks);
u32 sys_ticks(void);
int sys_meminfo(u32 out[4]);                      /* hostdisk.c: fast RAM free 0 with HOSTDISK_NOFAST (fsd's cache, D18) */
long sys_time(void);
void *xmalloc(u32 n);
/* the disk server's privileged calls (tools/host/diskd-host.c provides them) */
struct uproc { u32 pid, ppid, state, mem; char name[16]; int nice; u32 pri; };
int sys_blk_read(u32 lba, u32 n, void *buf);
int sys_blk_write(u32 lba, u32 n, const void *buf);
int sys_blk_size(void);
int sys_blk_ram(u32 *r);
int sys_blk_flush(void);
int sys_fd_info(u32 drive);
int sys_fd_rw(u32 drive, u32 lba, u32 n, void *buf, int write);
int sys_procinfo(u32 i, struct uproc *p);
/* the service manager's calls (DESIGN D11): diskd-host.c keeps the saved table in a buffer of its own */
int sys_svc_self(void);
int sys_svc_save(const void *buf, u32 n);
int sys_svc_load(void *buf, u32 n);
int sys_svc_opens(u32 *ino, u32 bytes);
#define SVC_MIDREQ 0x10000
#define SVC_EXIT_DAMAGED 86

/* the date conversion: libz's own source (user/zdate.h) */
struct zdt { int year, mon, day, hour, min, sec, wday, yday; };
static long zdays(int y, int m, int d);
#define ZDATE_LINK static __attribute__((unused))
#include "../../user/zdate.h"
