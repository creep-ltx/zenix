/* SPDX-License-Identifier: GPL-2.0-only */
/* Zenix's own helpers for C library programs (DESIGN D27 section 6, libzenix): the buffered standard output (a
 * 4 KB buffer for descriptor 1, written when full and at exit: one system call per screen), the error texts, the
 * helpers for paths, directories and colours, and the signals' names. The same code as libz's (user/zshare.h).
 * A program uses oputs or stdout's FILE functions, never both (two buffers would mix their order). */
#ifndef _ZENIX_H_
#define _ZENIX_H_
#include <stddef.h>
void oputs(const char *s);
void oputn(const char *s, unsigned n);
void oputc(int c);
void oflush(void);
const char *zstrerror(int e);           /* an error number's text (either sign), 0 if none */
const char *zwhy(const char *dflt);     /* the text of errno (the last failure), else dflt */
/* paths and directories: no name arrays or path buffers on a 16 KB stack */
#define ZPATH_MAX 256                   /* system calls take paths of at most 255 bytes */
char *path_join(const char *dir, const char *name);  /* malloc'd "dir/name"; 0: too long (ENAMETOOLONG) or ENOMEM */
int dir_list(const char *path, char ***names);  /* malloc'd, 0-terminated names without . and ..: their count or -1 */
void dir_free(char **names, int n);
/* a directory read in batches (DESIGN D49, user/zhelp.c; the records are kernel/udirx.h's) */
struct zdir;
struct udirx;
struct zdir *zdir_open(const char *path);
const struct udirx *zdir_next(struct zdir *d);
int zdir_err(const struct zdir *d);
int zdir_fd(const struct zdir *d);
void zdir_rewind(struct zdir *d);
void zdir_close(struct zdir *d);
int path_find(const char *name, char *out, size_t max);   /* a program through $PATH, as sh finds it: 1 found */
int zrealpath(const char *in, char *out, size_t cap);     /* ., .. and links resolved: 0, or -1 */
/* colours (DESIGN D14): auto means fd is a terminal, TERM set and not "dumb", NO_COLOR not set */
int color_auto(int fd);
int color_rule(int fd, const char *term, const char *no_color);
int color_arg(const char *arg, int bare);   /* --color[=WHEN]: 0 never, 1 always, 2 auto, -1 not it, -2 bad WHEN */
const char *zsig_name(int sig);         /* 15 -> "TERM"; 0 if none */
int zsig_num(const char *name);         /* "TERM", "SIGTERM" or "15" -> 15; -1 if unknown */
#endif
