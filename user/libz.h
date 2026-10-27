/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "usys.h"
#include <stdarg.h>
typedef unsigned long size_t;
u32 strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, u32 n);
char *strcpy(char *d, const char *s);
char *strchr(const char *s, int c);
char *strstr(const char *h, const char *n);
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
void *malloc(u32 n);
void free(void *p);
void *xmalloc(u32 n);                     /* malloc, or "NAME: out of memory" and exit status 1 */
void *calloc(unsigned long n, unsigned long size);   /* zeroed; NULL on overflow or no memory */
void *realloc(void *p, unsigned long n);  /* in place if it fits; copies only the old block; NULL leaves p as it was */
int fprintf(int fd, const char *fmt, ...);
int printf(const char *fmt, ...);
int atoi(const char *s);
const char *zwhy(const char *dflt);   /* the text of the last failure's error number (zerrno: "permission denied" ...), else dflt */
const char *zstrerror(int e);         /* strerror: the text of an error number (either sign), 0 if unknown */

/* directories and paths: every directory walker uses these (no name arrays or path buffers on the stack) */
#define ZPATH_MAX 64                          /* system calls take paths of at most 63 bytes */
char *path_join(const char *dir, const char *name);   /* malloc'd "dir/name"; NULL: too long (zerrno 36) or no memory (12) */
int dir_list(const char *path, char ***names);  /* malloc'd, NULL-terminated names without . and ..; count or -1 */
void dir_free(char **names, int n);

/* more of the C library */
char *strdup(const char *s);
char *strcat(char *d, const char *s);
char *strncpy(char *d, const char *s, u32 n);
char *strrchr(const char *s, int c);
int memcmp(const void *a, const void *b, unsigned long n);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
void qsort(void *base, u32 n, u32 size, int (*cmp)(const void *, const void *));
int snprintf(char *buf, u32 max, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int vsnprintf(char *buf, u32 max, const char *fmt, va_list ap);
int vfprintf(int fd, const char *fmt, va_list ap);
int isdigit(int c); int isalpha(int c); int isalnum(int c); int isspace(int c);
int isupper(int c); int islower(int c); int toupper(int c); int tolower(int c);
char *getenv(const char *name);
int path_find(const char *name, char *out, u32 max);   /* a program by name through $PATH, as sh does: 1 = found (out) */
char *bre_to_ere(const char *bre);   /* a basic regular expression in the extended syntax of regex.c (malloc'd) */
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
/* colours (DESIGN D14): color_auto(fd): colour is wanted (fd a terminal, TERM set and not "dumb", NO_COLOR not set);
 * color_arg(arg, bare): --color / --colour [=WHEN]: 0 never, 1 always, 2 auto (bare: `bare`), -1 not such an option,
 * -2 an unknown WHEN */
int color_auto(int fd);
int color_rule(int fd, const char *term, const char *no_color);   /* the same rule with these values (sh: its variables) */
int color_arg(const char *arg, int bare);
extern char **environ;
int getpriority(int which, int who);            /* POSIX: the nice value (-20..19); -1 and zerrno set on failure */
int setpriority(int which, int who, int nice);  /* POSIX: 0, or -1 and zerrno (EACCES: only root may raise) */

/* buffered line reader */
struct rd { int fd; int pos, len, eof; char buf[2048]; };
void rd_init(struct rd *r, int fd);
/* next line into *line (malloc'd, grown as needed, without the newline); returns its length, or -1 at the end */
int rd_getline(struct rd *r, char **line, u32 *cap);
int rd_getc(struct rd *r);

/* buffered standard output: oputs/oprintf collect, oflush writes; _zexit (called when main returns) flushes */
void oputs(const char *s);
void oputn(const char *s, u32 n);
void oputc(int c);
int oprintf(const char *fmt, ...);
void oflush(void);
void _zexit(int status) __attribute__((noreturn));

/* signals and waiting for several things (DESIGN D16, docs/d16-events-and-signals.md); numbers in usys.h */
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
typedef u32 sigset_t;
struct sigaction { sighandler_t sa_handler; sigset_t sa_mask; int sa_flags; };
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
sighandler_t signal(int sig, sighandler_t h);  /* BSD style, as glibc: the handler stays, calls restart (SA_RESTART) */
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
#define sigemptyset(s)    (*(s) = 0, 0)
#define sigfillset(s)     (*(s) = ~1u, 0)
#define sigaddset(s, n)   (*(s) |= 1u << (n), 0)
#define sigdelset(s, n)   (*(s) &= ~(1u << (n)), 0)
#define sigismember(s, n) ((*(s) >> (n)) & 1)
int kill(int pid, int sig);                     /* pid 0: own group, -g: group g; sig 0: does it exist? */
int raise(int sig);
unsigned alarm(unsigned seconds);               /* the seconds the previous alarm had left */
int pause(void);                                /* until a handler ran: -1, zerrno EINTR */
int setpgid(int pid, int pgid);
int getpgid(int pid);                           /* 0: the caller */
int getpgrp(void);
int tcsetpgrp(int fd, int pgrp);                /* the foreground process group of the caller's terminal */
int poll(struct pollfd *fds, u32 n, int ms);    /* ms -1: no limit; also ports (POLLPORT) */
#define FD_SETSIZE 32
typedef struct { u32 bits; } fd_set;
#define FD_ZERO(s)     ((s)->bits = 0)
#define FD_SET(n, s)   ((s)->bits |= 1u << (n))
#define FD_CLR(n, s)   ((s)->bits &= ~(1u << (n)))
#define FD_ISSET(n, s) (((s)->bits >> (n)) & 1)
struct timeval { long tv_sec, tv_usec; };
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv);
int zsig_num(const char *name);                 /* "TERM", "SIGTERM" or "15" -> 15; -1 if unknown */
const char *zsig_name(int sig);                 /* 15 -> "TERM"; 0 if none */
