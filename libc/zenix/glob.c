/* SPDX-License-Identifier: GPL-2.0-only */
/* glob and globfree (DESIGN D27 section 5: the port's, picolibc has none) over opendir/readdir and picolibc's fnmatch:
 * the Unix wildcards * ? [...] in any part of the path, a backslash takes the next character as it is, a name that
 * starts with a dot is matched only by a pattern part that starts with one. Flags: GLOB_APPEND, GLOB_DOOFFS, GLOB_ERR,
 * GLOB_MARK, GLOB_NOCHECK, GLOB_NOSORT, GLOB_NOMAGIC, GLOB_QUOTE (backslashes are always taken); the others (braces,
 * ~, other directory functions, a limit) answer GLOB_ABEND with errno EINVAL. No path buffer on the stack: every
 * path is malloc'd (at most PATH_MAX, 256 bytes with its end, as every system call takes; a longer one cannot name a file). */
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define GL_KNOWN (GLOB_APPEND | GLOB_DOOFFS | GLOB_ERR | GLOB_MARK | GLOB_NOCHECK | GLOB_NOSORT | GLOB_NOMAGIC | GLOB_QUOTE)

struct gl { glob_t *g; int flags, (*errf)(const char *, int), cap, err; };

static int magic(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) { i++; continue; }
        if (s[i] == '*' || s[i] == '?' || s[i] == '[') return 1;
    }
    return 0;
}
/* one more name in the list (with a / when GLOB_MARK and it is a directory) */
static int add(struct gl *x, const char *path, int dir)
{
    glob_t *g = x->g;
    size_t at = (size_t)g->gl_offs + (size_t)g->gl_pathc;
    if (at + 2 > (size_t)x->cap) {
        int nc = x->cap ? x->cap * 2 : 16;
        char **nv = realloc(g->gl_pathv, (size_t)nc * sizeof *nv);
        if (!nv) return GLOB_NOSPACE;
        g->gl_pathv = nv;
        x->cap = nc;
    }
    size_t l = strlen(path);
    int mark = dir && (x->flags & GLOB_MARK) && (!l || path[l - 1] != '/');
    char *p = malloc(l + 1 + (size_t)mark);
    if (!p) return GLOB_NOSPACE;
    memcpy(p, path, l);
    if (mark) p[l++] = '/';
    p[l] = 0;
    g->gl_pathv[at] = p;
    g->gl_pathv[at + 1] = 0;
    g->gl_pathc++;
    g->gl_matchc++;
    return 0;
}
/* base and one more part ("" is the current directory), malloc'd; 0 when longer than a path may be */
static char *join(const char *base, const char *part, size_t n)
{
    size_t bl = strlen(base), sl = bl && base[bl - 1] != '/';
    if (bl + sl + n >= PATH_MAX) return 0;
    char *p = malloc(bl + sl + n + 2);         /* (room for a trailing /, see walk) */
    if (!p) return 0;
    memcpy(p, base, bl);
    if (sl) p[bl] = '/';
    memcpy(p + bl + sl, part, n);
    p[bl + sl + n] = 0;
    return p;
}
static int isdir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }

/* the rest of the pattern under the directory base */
static int walk(struct gl *x, const char *base, const char *rest)
{
    while (*rest == '/') rest++;
    size_t n = 0;
    while (rest[n] && rest[n] != '/') n++;
    const char *tail = rest + n;
    int slash = *tail == '/';                       /* a pattern that ends in /: only directories, named with it */
    while (*tail == '/') tail++;
    int last = !*tail, r = 0;
    if (!magic(rest, n)) {                          /* a plain part: no directory read */
        char *lit = malloc(n + 1), *o = lit;
        if (!lit) return GLOB_NOSPACE;
        for (size_t i = 0; i < n; i++) { if (rest[i] == '\\' && i + 1 < n) i++; *o++ = rest[i]; }
        *o = 0;
        char *p = join(base, lit, (size_t)(o - lit));
        free(lit);
        if (!p) return 0;
        struct stat st;
        if (last) {
            if (!lstat(p, &st) && (!slash || isdir(p))) {
                int dir = isdir(p);
                if (slash) strcat(p, "/");
                r = add(x, p, dir);
            }
        } else
            r = walk(x, p, tail);
        free(p);
        return r;
    }
    char *pat = malloc(n + 1);
    if (!pat) return GLOB_NOSPACE;
    memcpy(pat, rest, n);
    pat[n] = 0;
    DIR *d = opendir(*base ? base : ".");
    if (!d) {
        free(pat);
        if (errno == ENOENT || errno == ENOTDIR) return 0;
        if ((x->errf && x->errf(*base ? base : ".", errno)) || (x->flags & GLOB_ERR)) return GLOB_ABEND;
        return 0;
    }
    struct dirent *e;
    while (!r && (e = readdir(d))) {
        if (fnmatch(pat, e->d_name, FNM_PERIOD)) continue;
        char *p = join(base, e->d_name, strlen(e->d_name));
        if (!p) continue;                           /* longer than any path: no such file */
        if (last) {
            int dir = (slash || (x->flags & GLOB_MARK)) && isdir(p);
            if (!slash || dir) {
                if (slash) strcat(p, "/");
                r = add(x, p, dir);
            }
        } else if (isdir(p))
            r = walk(x, p, tail);
        free(p);
    }
    closedir(d);
    free(pat);
    return r;
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int glob(const char *pattern, int flags, int (*errf)(const char *, int), glob_t *g)
{
    struct gl x = { g, flags, errf, 0, 0 };
    if (flags & ~GL_KNOWN) { errno = EINVAL; return GLOB_ABEND; }
    if (!(flags & GLOB_APPEND)) {
        g->gl_pathc = g->gl_matchc = 0;
        g->gl_pathv = 0;
        if (!(flags & GLOB_DOOFFS)) g->gl_offs = 0;
        size_t n0 = (size_t)g->gl_offs + 1;
        if (!(g->gl_pathv = malloc(n0 * sizeof *g->gl_pathv))) return GLOB_NOSPACE;
        for (size_t i = 0; i < n0; i++) g->gl_pathv[i] = 0;
        x.cap = (int)n0;
    } else {
        x.cap = g->gl_offs + g->gl_pathc + 1;       /* the list as it is (realloc grows it) */
        g->gl_matchc = 0;
    }
    g->gl_flags = flags;
    int old = g->gl_pathc;
    int r = walk(&x, *pattern == '/' ? "/" : "", pattern);
    if (!r && g->gl_pathc == old) {
        if ((flags & GLOB_NOCHECK) || ((flags & GLOB_NOMAGIC) && !magic(pattern, strlen(pattern))))
            r = add(&x, pattern, 0);
        else
            return GLOB_NOMATCH;
    }
    if (r) return r;
    if (!(flags & GLOB_NOSORT))
        qsort(g->gl_pathv + g->gl_offs + old, (size_t)(g->gl_pathc - old), sizeof *g->gl_pathv, cmp);
    return 0;
}
void globfree(glob_t *g)
{
    if (!g->gl_pathv) return;
    for (int i = 0; i < g->gl_pathc; i++) free(g->gl_pathv[g->gl_offs + i]);
    free(g->gl_pathv);
    g->gl_pathv = 0;
    g->gl_pathc = 0;
}
