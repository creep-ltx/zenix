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
int gr_edit_at(const char *who, const char *path, const char *group, const char *name, int add);   /* the same in a group file at PATH */
/* the one way /etc/passwd's lines are made and changed (passwd, useradd, zinstall's accounts for a new root: PATH names the file) */
int pw_name_ok(const char *n);          /* a login name: [a-z_][a-z0-9_-]*, 1-31 characters */
int pw_field_ok(const char *s);         /* no ':' and no newline */
void pw_new_hash(const char *password, char out[100]);      /* a fresh salt and pw_hash; "" for an empty password */
int pw_set_hash_at(const char *who, const char *path, const char *name, const char *hash);   /* NAME's hash field; 0, -1 (said) */
struct pw_add { const char *name, *group, *home, *shell, *comment, *extra, *hash; long uid; };   /* uid -1: the lowest free from 1000 */
/* a new line in the passwd file at PATH, its group from the group file at GPATH (a name or a number), -G's groups (EXTRA, a comma
 * list) checked before anything is written and joined after; *uid the number given. 0, 1 a refusal, 2 a bad argument (said) */
int pw_add_at(const char *who, const char *path, const char *gpath, const struct pw_add *a, u32 *uid, u32 *gid);
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
/* the one test of a login (login, dlogin: DESIGN D38 revision 3): NAME is a user and PASSWORD is theirs (or none is
 * needed) -> 1, their entry in *P. An unknown name takes as long as a wrong password of a new hash (PW_ROUNDS; a
 * locked, empty or older one answers sooner - /etc/passwd is readable anyway); after a failure the caller waits
 * PW_FAIL_DELAY ticks (the same in both) */
int pw_auth(const char *name, const char *password, struct pw *p);
#define PW_FAIL_DELAY 100
