/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* /etc/passwd and /etc/group read (zpw.c, one copy for libz's programs and the C library's getpwnam ...; DESIGN D27
 * section 5). The formats (frozen):  /etc/passwd  name:hash:uid:gid:comment:home:shell   /etc/group  name::gid:members
 * hash: empty = no password, "*" = account locked, else "ROUNDS$salt$hex", or the older "salt$hex" = 100 rounds (sha.c).
 * The uid and gid fields must be decimal numbers (0-65535): a line where they are not is ignored, never read as 0. */
#include "usys.h"
struct pw { char name[32], hash[100], comment[48], home[64], shell[32]; u32 uid, gid; };
struct gr { char name[32]; u32 gid; char members[128]; };
int pw_byname(const char *name, struct pw *out);       /* 0 found, -1 not */
int pw_byuid(u32 uid, struct pw *out);
int gr_byname(const char *name, struct gr *out);
int gr_bygid(u32 gid, struct gr *out);
int read_secret(const char *prompt, char *buf, int max);   /* a line read without echo (raw): 0, or -1 on ^C or the end */
char *pw_slurp(const char *path, int *len);              /* the whole file, malloc'd (free it), NUL-terminated; 0 if unreadable */
