/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "usys.h"
#include <stdarg.h>
typedef __SIZE_TYPE__ size_t;            /* the compiler's, as the C library's (DESIGN D27 6 step 2) */
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strchr(const char *s, int c);
char *strstr(const char *h, const char *n);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *malloc(size_t n);
void free(void *p);
void *rheap_alloc(u32 n);                 /* the region heap: small first-fit blocks, for servers (libz.c) */
void rheap_free(void *p);
u32 rheap_size(const void *p);             /* the bytes a block holds (with its header) */
void *xmalloc(u32 n);                     /* malloc, or "NAME: out of memory" and exit status 1 */
void *calloc(size_t n, size_t size);   /* zeroed; NULL on overflow or no memory */
void *realloc(void *p, size_t n);  /* in place if it fits; copies only the old block; NULL leaves p as it was */
int dprintf(int fd, const char *fmt, ...);   /* POSIX's: unbuffered, straight to fd (DESIGN D27 6 step 2) */
int atoi(const char *s);
const char *zwhy(const char *dflt);   /* the text of the last failure's error number (zerrno: "permission denied" ...), else dflt */
const char *zstrerror(int e);         /* strerror: the text of an error number (either sign), 0 if unknown */

/* directories and paths: every directory walker uses these (no name arrays or path buffers on the stack) */
#define ZPATH_MAX 256                         /* system calls take paths of at most 255 bytes (FS_PATHMAX) */
char *path_join(const char *dir, const char *name);   /* malloc'd "dir/name"; NULL: too long (zerrno 36) or no memory (12) */
int dir_list(const char *path, char ***names);  /* malloc'd, NULL-terminated names without . and ..; count or -1 */
int dir_list_some(const char *path, char ***names, int *err);  /* the same, but a failure part way: the names so far, and *err */
/* DESIGN D49: and each name's lstat where the server gave it (*stats one a name, malloc'd or NULL; mode 0: not given) */
int dir_list_stat(const char *path, char ***names, struct ustat **stats, int *err);
/* a directory read in batches (D49): zdir_next gives the next record (kernel/udirx.h), NULL at the end or on an
 * error (zdir_err: 0 or the errno); the old one-a-call path by itself where the kernel or the server lacks the batch */
struct zdir;
struct zdir *zdir_open(const char *path);     /* NULL and zerrno: cannot open */
const struct udirx *zdir_next(struct zdir *d);
int zdir_err(const struct zdir *d);
int zdir_fd(const struct zdir *d);
void zdir_rewind(struct zdir *d);             /* forget the buffered records (the caller put the descriptor back at 0) */
void zdir_close(struct zdir *d);              /* also the descriptor */
/* D49 8: is PATH (links followed) the file open on FD, the same device and inode? (cp, mv: never copy onto itself) */
int same_file(int fd, const char *path);
void dir_free(char **names, int n);
/* the absolute name of a path with ., .. and symbolic links resolved as the kernel follows them (DESIGN D26: at most
 * 40 links; the path need not exist): 0 and out (cap bytes, at most ZPATH_MAX used), -1 when it cannot be resolved (a loop,
 * a link that cannot be read, a name too long). Its buffers are on the stack (about 1.5 KB, no recursion) */
int zrealpath(const char *in, char *out, size_t cap);
/* one line of /etc/svctab or /etc/daemons (DESIGN D11 4.4, D24 7): "NAME NICE PROGRAM [ARGUMENTS]", cut in place (tabs
 * made spaces: the kernel splits a program's words at spaces). 1: *name, *nice and *prog set; 0: blank or a # comment;
 * -EINVAL: a bad line (*name its first word, for the message). One parser for boot and init */
int svc_line(char *l, char **name, int *nice, char **prog);

/* more of the C library */
char *strdup(const char *s);
char *strcat(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strrchr(const char *s, int c);
int memcmp(const void *a, const void *b, size_t n);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
int snprintf(char *buf, size_t max, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int vsnprintf(char *buf, size_t max, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int isdigit(int c); int isalpha(int c); int isalnum(int c); int isspace(int c);
int isupper(int c); int islower(int c); int toupper(int c); int tolower(int c);
char *getenv(const char *name);
int path_find(const char *name, char *out, size_t max);   /* a program by name through $PATH, as sh does: 1 = found (out) */
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
/* DESIGN D28: UTF-8 as the console decodes it, and its widths (kernel/cwidth.h: the same function and table), so a
 * program lays out its columns as the console draws them. A byte that is not part of a character is one cell of one
 * column (the console's Latin-1 fallback); a C1 control written as UTF-8 and a zero-width character take none. */
int zu8next(const char *s, u32 n, int *cols);   /* bytes of the character at s (1-4; n >= 1), *cols its columns */
u32 zu8prev(const char *s, u32 pos);            /* where the character before pos starts (pos > 0) */
int zu8width(u32 c);                            /* a code point's columns: -1, 0, 1 or 2 */
int zu8cols(const char *s, u32 n);              /* the columns of n bytes */
u32 zu8fit(const char *s, u32 n, int cols);     /* the bytes of whole characters that fit in cols columns */

/* pseudo-terminals (DESIGN D20 7.2, user/zpty.c): /dev/ptmx and /dev/pts/N; the window size through ioctl (zsock.c) */
#ifndef TIOCGWINSZ
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };   /* Linux's, the kernel's struct uwinsize */
#define TIOCSCTTY  0x540E               /* Linux's numbers (libz's own, not a system call's) */
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#endif
int ioctl(int fd, unsigned long req, ...);      /* TIOCGWINSZ, TIOCSWINSZ, TIOCSCTTY; FIONREAD, FIONBIO on a socket */
int posix_openpt(int flags);                    /* a new pty's master (O_RDWR, O_NONBLOCK) */
int grantpt(int fd);                            /* nothing to do: 0 on a master */
int unlockpt(int fd);
char *ptsname(int fd);                          /* "/dev/pts/N" (a static buffer), 0 if fd is no master */
int ptsname_r(int fd, char *buf, size_t len);   /* 0, or the error number */
int openpty(int *amaster, int *aslave, char *name, const void *termp, const struct winsize *winp);
int setsid(void);                               /* own group; leader of fd 0's terminal if nobody leads it */

/* DESIGN D60 (docs/d60-full-protection.md 5.5, 11): a server asking for the bytes of its client it is about to read
 * (peer_rd) or write (peer_wr): 1 if it may, 0 if not. With full memory protection the kernel lends the range for the
 * rest of the request (a system service, or a server that could debug the client, may ask for any range; any other
 * server only for what the client declared); otherwise no system call is made: 1 */
int prot_full(void);                            /* full memory protection is on (SYS_meminfo's fifth word, read once) */
int peer_rd(struct umsg *m, u32 addr, u32 len);
int peer_wr(struct umsg *m, u32 addr, u32 len);
int peer_str(struct umsg *m, u32 addr, u32 max);    /* a string there: peer_rd of the longest range of at most max
                                                 * bytes that is the client's; 1 if it holds the string's 0 (or is max
                                                 * bytes: the caller stops there) */
