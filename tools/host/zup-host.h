/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Host build of zup (user/zup.c) and SHA-256 (user/sha.c) for tools/zup-host-test.sh: the Zenix system calls zup uses,
 * served from a directory on the PC (tools/host/zup-host.c). Build with -m32 -DZHOST. The environment:
 *   ZUP_ROOT    the directory that is "/"
 *   ZUP_BUNDLE  a bundle file SYS_update INFO reports as in memory (unset: none); ZUP_FLAGS its flags
 *   ZUP_KVNUM   the running kernel's version (unset: a kernel without SYS_update, -ENOSYS)
 *   ZUP_CUT=N   the power goes before the Nth change (write, rename, link, unlink, mkdir, chmod, chown, sync, mark)
 *   ZUP_TRACE   a file: each change, one line ("rename /a /b"); ZUP_SELF: this program, for /sbin/zup's spawn
 * The processes (SYS_procinfo, for zup's lock) are the PC's own, from /proc: a program called zup or zpkg there is
 * a zup or zpkg to it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <signal.h>                     /* zup ignores SIGPIPE (DESIGN D16 rule 11): the host's own call here */
#include "../../kernel/errno.h"

typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef int s32;

#ifndef O_CREAT                         /* the same values as Linux's: the PC's own when its headers came first */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_EXCL   0x80
#define O_TRUNC  0x200
#define O_APPEND 0x400
#endif
#ifndef S_IFMT
#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#endif

struct ustat { u32 ino, mode, size, mtime, uid, gid, links; };
struct ustatfs { unsigned bsize, blocks, bfree, files, ffree; };
struct umsg { struct umsg *next; int from, server; u32 state; int result; u32 type; u32 a[4]; unsigned short uid, gid, umask, kflags; u32 client_; };
struct uupdate { u32 addr, size, flags, kvnum; char kver[16]; };
struct umountinfo { char path[48], port[16], dev[16], type[8]; };
int sys_mountinfo(u32 i, struct umountinfo *m);   /* the root only (-1 past it) */
struct uproc { u32 pid, ppid, state, mem; char name[16]; int nice; u32 pri; u32 shared; u32 pgid; };
#define UPD_INFO      1
#define UPD_RELEASE   2
#define UPD_NOTE      3
#define UPD_F_ROLLBACK 1u
#define UPD_F_TRIAL    2u
#define UPDNOTE_DONE      1u
#define UPDNOTE_FAILED    2u
#define UPDNOTE_CONFIRMED 3u
#define FS_MARK 25

int sys_open(const char *p, int fl);
int sys_close(int fd);
int sys_read(int fd, void *b, u32 n);
int sys_write(int fd, const void *b, u32 n);
int sys_stat(const char *p, struct ustat *s);
int sys_lstat(const char *p, struct ustat *s);
int sys_rename(const char *a, const char *b);
int sys_link(const char *a, const char *b);
int sys_unlink(const char *p);
int sys_mkdir(const char *p);
int sys_rmdir(const char *p);
int sys_chmod(const char *p, u32 mode);
int sys_chown(const char *p, u32 uid, u32 gid);
int sys_symlink(const char *t, const char *p);
int sys_readlink(const char *p, char *b, u32 max);
int sys_sync(void);
int sys_statfs(struct ustatfs *f);
int sys_getuid(void);
int sys_getpid(void);
int sys_procinfo(u32 i, struct uproc *p);   /* the PC's processes (/proc), slot i from 1 */
int sys_lseek(int fd, int off, int whence);
int sys_fstat(int fd, struct ustat *s);
int sys_spawn(const char *path, char *const *argv, const int *stdio);
int sys_wait(int pid);
int sys_port_find(const char *n);
int sys_call(int port, struct umsg *m);
int sys_update(u32 op, u32 a, u32 b);
int dir_list(const char *path, char ***names);
void dir_free(char **names, int n);
#include "../../user/sha256.h"
