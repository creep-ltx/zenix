// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "libz.h"
/* /etc/passwd:  name:hash:uid:gid:comment:home:shell      /etc/group:  name::gid:member,member
 * hash: empty = no password, "*" = account locked, else "ROUNDS$salt$hex", or the older "salt$hex" = 100 rounds (sha.c).
 * The uid and gid fields must be decimal numbers (0-65535): a line where they are not is ignored, never read as 0. */
struct pw { char name[32], hash[100], comment[48], home[64], shell[32]; u32 uid, gid; };
struct gr { char name[32]; u32 gid; char members[128]; };
int pw_byname(const char *name, struct pw *out);       /* 0 found, -1 not */
int pw_byuid(u32 uid, struct pw *out);
int gr_byname(const char *name, struct gr *out);
int gr_bygid(u32 gid, struct gr *out);
const char *pw_name(u32 uid, char *buf);                /* name, or the number as text when unknown */
const char *gr_name(u32 gid, char *buf);
int read_secret(const char *prompt, char *buf, int max);
char *pw_slurp(const char *path, int *len);             /* the whole file, malloc'd (free it), NUL-terminated; 0 if unreadable */
static inline void fputs(const char *s, int fd) { sys_write(fd, s, strlen(s)); }
static inline void *memchr(const void *s, int c, u32 n) { const unsigned char *p = s; while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; } return 0; }
/* sha.c */
void sha256(const void *data, u32 len, unsigned char out[32]);
void pw_hash(const char *password, const char *salt, char *out);      /* out: "ROUNDS$salt$hex" (at least 100 bytes) */
int pw_check(const char *password, const char *stored);                /* 1 = right password (or none needed) */
