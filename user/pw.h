// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "libz.h"
#include "zpw.h"                        /* struct pw, struct gr and the parser (zpw.c) */
const char *pw_name(u32 uid, char *buf);                /* name, or the number as text when unknown */
const char *gr_name(u32 gid, char *buf);
int pw_replace(const char *who, const char *path, const char *data, u32 len, u32 mode);   /* PATH.new, then rename over: 0 or -1 (said) */
int gr_has(const char *list, const char *name);         /* NAME in a comma-separated member list */
void gr_drop(char *list, const char *name);             /* take NAME out of such a list, in place */
int gr_edit(const char *who, const char *group, const char *name, int add);   /* /etc/group: member in/out; 0 or -1 (said) */
/* supplementary groups (SYS_groups): set the process's from /etc/group for user P (as initgroups: its gid and every
 * group listing it; more than NGROUPS: said, the first ones kept), before setuid; 0 or the kernel's -errno */
int pw_initgroups(const char *who, const struct pw *p);
int in_group(u32 gid);                  /* the real gid, or one of the process's supplementary groups */
int in_group_named(const char *name);   /* the same, for a group of /etc/group by name (0 if it has none) */
static inline void pw_fputs(const char *s, int fd) { sys_write(fd, s, strlen(s)); }
static inline void *memchr(const void *s, int c, u32 n) { const unsigned char *p = s; while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; } return 0; }
/* sha.c */
#include "sha256.h"
void pw_hash(const char *password, const char *salt, char *out);      /* out: "ROUNDS$salt$hex" (at least 100 bytes) */
int pw_check(const char *password, const char *stored);                /* 1 = right password (or none needed) */
