/* SPDX-License-Identifier: GPL-2.0-only */
/* Zenix's helpers, one source for libz and the C library's libzenix (user/zshare.h, zenix.h): paths and directories
 * (every directory walker uses these, so that no walker keeps a name array or a path buffer on its 16 KB stack: a
 * recursive walk takes one level per directory, and there is no guard below the stack), the resolver of realpath, the
 * colour rule (DESIGN D14) and the translation of basic regular expressions. */
#include "zshare.h"

/* realpath's resolver (realpath(1), wasabid): one pass at a time, no recursion; no printf (wasabid has none) */
int zrealpath(const char *in, char *out, zs_size cap)
{
    char path[512], acc[ZPATH_MAX], tgt[ZPATH_MAX + 1], next[512];  /* a target of 255 bytes plus a path of 255 */
    int links = 0;
    acc[0] = 0;
    if (in[0] != '/' && sys_getcwd(acc, sizeof acc) < 0) strcpy(acc, "/");
    if (!strcmp(acc, "/")) acc[0] = 0;
    u32 il = strlen(in);
    if (il >= sizeof path) return -1;
    memcpy(path, in, il + 1);
    char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *e = p; while (*e && *e != '/') e++;
        char save = *e; *e = 0;
        if (!strcmp(p, ".")) { }
        else if (!strcmp(p, "..")) { char *sl = strrchr(acc, '/'); if (sl) *sl = 0; }
        else {
            u32 l = strlen(acc); if (l + strlen(p) + 2 >= sizeof acc) return -1;
            acc[l] = '/'; strcpy(acc + l + 1, p);
            struct ustat st;
            if (sys_lstat(acc[0] ? acc : "/", &st) == 0 && (st.mode & S_IFMT) == S_IFLNK) {
                if (++links > 40) return -1;
                int n = sys_readlink(acc, tgt, sizeof tgt - 1); if (n < 0) return -1;
                if (n == (int)sizeof tgt - 1) return -1;    /* a longer target (F15): "cannot resolve", never a prefix */
                tgt[n] = 0;
                *strrchr(acc, '/') = 0;                     /* the directory part, then the link text relative to it */
                *e = save;
                u32 el = strlen(e);
                if ((u32)n + el >= sizeof next) return -1;
                memcpy(next, tgt, (u32)n);
                memcpy(next + n, e, el + 1);
                strcpy(path, next);
                if (tgt[0] == '/') acc[0] = 0;              /* an absolute target starts again from / */
                p = path;
                continue;
            }
        }
        *e = save; p = e;
    }
    const char *r = acc[0] ? acc : "/";
    if (strlen(r) >= cap) return -1;
    strcpy(out, r);
    return 0;
}

/* "dir/name" in a new malloc'd string (no double slash). NULL when it would be longer than a system call takes
 * (zerrno 36, "path too long") or there is no memory (zerrno 12): the caller reports it, nothing is cut. */
char *path_join(const char *dir, const char *name)
{
    u32 dl = strlen(dir), nl = strlen(name);
    u32 slash = dl && dir[dl - 1] != '/';
    if (dl + slash + nl > ZPATH_MAX - 1) { zerrno = ENAMETOOLONG; return 0; }
    char *p = malloc(dl + slash + nl + 1);
    if (!p) { zerrno = ENOMEM; return 0; }
    memcpy(p, dir, dl);
    if (slash) p[dl] = '/';
    memcpy(p + dl + slash, name, nl + 1);
    return p;
}

/* DESIGN D49 8: is PATH the file open on FD (the same device and inode, links followed as an open follows them)?
 * Opening it with O_TRUNC would empty that file (cp a a, mv across a link onto its own mount). One lookup: the open
 * file is asked by its descriptor. A kernel before D49 gives no device (0 in both): then the inode, size, time and
 * mode together (two empty files of two new volumes may look alike: refused, nothing lost) */
int same_file(int fd, const char *path)
{
    struct ustat2 x, y;
    if (sys_fstat2(fd, &x) < 0 || sys_stat2(path, &y) < 0 || x.ino != y.ino || x.dev != y.dev) return 0;
    return x.dev || (x.size == y.size && x.mtime == y.mtime && x.mode == y.mode);
}

#pragma GCC push_options
#pragma GCC optimize("Os")              /* (cold enough: about 40 records a call) */
/* DESIGN D49: a directory read in batches. One malloc: the descriptor, the state, a 2 KB buffer of records (about 42
 * short names a call). The first SYS_readdirs answering -ENOSYS (an older kernel, a server without FS_READDIRS) puts the
 * directory in the old mode for good: one SYS_readdir a record, made into a udirx (no UDX_STAT, next 0) */
#define ZDIR_BUF 2048
#define ZDIR_MOUNTS 7                   /* NMOUNT 8 (kernel/kernel.h) less the root */
struct zdir { int fd; unsigned char old, end, mnts, nmn; unsigned short len, pos; int err; char mn[ZDIR_MOUNTS][48]; char buf[ZDIR_BUF]; };
/* D49 4.2: the last names of the mount points (read once a listing, when a record first has a stat): such a name's
 * record keeps the stat of the directory underneath, its lstat is the mounted root's. By text, as the kernel picks
 * mounts; a name that only looks like one loses its stat too, which costs that name one lstat */
static __attribute__((noinline)) void zdir_unmount(struct zdir *d, struct udirx *x)
{
    if (!d->mnts) {
        struct umountinfo u;
        d->mnts = 1;
        for (u32 i = 1; d->nmn < ZDIR_MOUNTS && sys_mountinfo(i, &u) >= 0; i++) {
            u.path[sizeof u.path - 1] = 0;
            const char *l = strrchr(u.path, '/');
            strcpy(d->mn[d->nmn++], l ? l + 1 : u.path);
        }
    }
    for (int i = 0; i < d->nmn; i++)
        if (!strcmp(d->mn[i], x->name)) { x->flags = 0; memset(&x->st, 0, sizeof x->st); }
}
struct zdir *zdir_open(const char *path)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct zdir *d = malloc(sizeof *d);
    if (!d) { sys_close(fd); zerrno = ENOMEM; return 0; }
    d->fd = fd; d->old = d->end = d->mnts = d->nmn = 0; d->len = d->pos = 0; d->err = 0;
    return d;
}
static __attribute__((noinline)) int zdir_fill(struct zdir *d)
{
    int r = -ENOSYS;
    d->pos = d->len = 0;
    if (!d->old && (r = sys_readdirs(d->fd, d->buf, sizeof d->buf)) == -ENOSYS) d->old = 1;
    if (d->old) {
        struct udirent u;
        if ((r = sys_readdir(d->fd, &u)) > 0) {
            u.name[sizeof u.name - 1] = 0;
            u32 used = 0;
            udirx_put(d->buf, sizeof d->buf, &used, u.name, u.ino, u.type, 0);   /* (an empty name: skipped by zdir_next) */
            r = (int)used;
        }
    }
    if (r > (int)sizeof d->buf) r = -EIO;
    if (r <= 0) { d->end = 1; d->err = r < 0 ? -r : 0; return 0; }
    d->len = (unsigned short)r;
    return 1;
}
/* the next record, NULL at the end or on an error (zdir_err); a record the server got wrong ends the reading, -EIO */
const struct udirx *zdir_next(struct zdir *d)
{
    for (;;) {
        if (d->end || (d->pos >= d->len && !zdir_fill(d))) return 0;
        struct udirx *x = (struct udirx *)(d->buf + d->pos);
        u32 st = udirx_step(x, (u32)(d->len - d->pos));
        if (!st) { d->end = 1; d->err = EIO; return 0; }   /* cannot step over it: the rest of this answer is lost */
        d->pos = (unsigned short)(d->pos + st);
        if (!udirx_named(x)) continue;                  /* a name no program can use: skipped, the rest still listed */
        if (x->flags & UDX_STAT) zdir_unmount(d, x);
        return x;
    }
}
int zdir_err(const struct zdir *d) { return d->err; }
int zdir_fd(const struct zdir *d) { return d->fd; }
void zdir_rewind(struct zdir *d) { d->end = 0; d->len = d->pos = 0; d->err = 0; }   /* after the descriptor is back at 0 */
void zdir_close(struct zdir *d) { if (d) { sys_close(d->fd); free(d); } }

/* The names in a directory, without "." and "..", in the order the filesystem gives them: *names is a malloc'd
 * array (as long as needed, NULL-terminated) of malloc'd strings, given back with dir_free. Returns the count, or
 * -1 when the directory cannot be opened (zerrno from the system call) or memory runs out (zerrno 12); *names is
 * then NULL and nothing is left allocated. If reading fails part way (a damaged disk), *err is that error and the
 * names read so far are returned: what can be seen is seen. Without a failure *err is 0. */
static int dir_list_x(const char *path, char ***names, struct ustat **stats, int *err)
{
    *names = 0;
    *err = 0;
    if (stats) *stats = 0;
    struct zdir *d = zdir_open(path);
    if (!d) return -1;
    char **v = 0; struct ustat *sv = 0; int n = 0, cap = 0; const struct udirx *x;
    while ((x = zdir_next(d))) {
        if (x->name[0] == '.' && (!x->name[1] || (x->name[1] == '.' && !x->name[2]))) continue;
        if (n + 1 >= cap) {
            int nc = cap ? cap * 2 : 32;
            char **nv = realloc(v, (u32)nc * sizeof *nv);
            if (!nv) goto oom;
            v = nv;
            if (stats) {
                struct ustat *ns = realloc(sv, (u32)nc * sizeof *ns);
                if (!ns) goto oom;
                sv = ns;
            }
            cap = nc;
        }
        if (!(v[n] = strdup(x->name))) goto oom;
        if (stats) {
            if (x->flags & UDX_STAT) memcpy(&sv[n], &x->st, sizeof sv[n]);
            else memset(&sv[n], 0, sizeof sv[n]);         /* mode 0: not given (a real file's mode never is 0) */
        }
        n++;
    }
    int e = zdir_err(d);
    if (e && e != ENOTDIR) *err = e;                  /* (not a directory: no names, as it always was) */
    zdir_close(d);
    if (!v && !(v = malloc(sizeof *v))) { free(sv); zerrno = ENOMEM; return -1; }
    v[n] = 0;
    *names = v;
    if (stats) *stats = sv;
    return n;
oom:
    zdir_close(d);
    dir_free(v, n);
    free(sv);
    zerrno = ENOMEM;
    return -1;
}
int dir_list_some(const char *path, char ***names, int *err) { return dir_list_x(path, names, 0, err); }
/* DESIGN D49: dir_list_some and each name's lstat where the directory's server gave it (*stats malloc'd, one a name,
 * NULL when there are none; freed by the caller; mode 0 where it was not given: ask lstat) */
int dir_list_stat(const char *path, char ***names, struct ustat **stats, int *err) { return dir_list_x(path, names, stats, err); }
#pragma GCC pop_options
/* dir_list_some for those who go on to act on the names (copy, remove, archive): all of them, or -1 */
int dir_list(const char *path, char ***names)
{
    int e, n = dir_list_some(path, names, &e);
    if (n >= 0 && e) {
        dir_free(*names, n);
        *names = 0;
        zerrno = e;
        return -1;
    }
    return n;
}
void dir_free(char **names, int n)
{
    if (!names) return;
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* A program by name, as the shell finds it: a name with a slash as it is, else the first PATH directory (default /bin)
 * that has a regular file of that name. 1 and the path in out, or 0 (not found, or longer than max). */
int path_find(const char *name, char *out, zs_size max)
{
    struct ustat st;
    if (strchr(name, '/')) {
        if (strlen(name) >= max) return 0;
        strcpy(out, name);
        return sys_stat(out, &st) == 0 && (st.mode & S_IFMT) == S_IFREG;
    }
    const char *p = getenv("PATH");
    if (!p) p = "/bin";
    u32 nl = strlen(name);
    while (*p) {
        const char *e = p; while (*e && *e != ':') e++;
        u32 l = (u32)(e - p);
        if (l + nl + 2 <= max) {
            memcpy(out, p, l);
            if (!l) out[l++] = '.';
            if (out[l - 1] != '/') out[l++] = '/';
            memcpy(out + l, name, nl + 1);
            if (sys_stat(out, &st) == 0 && (st.mode & S_IFMT) == S_IFREG) return 1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

/* Colours (DESIGN D14): `auto` means colour when fd is a terminal, TERM is set and not "dumb", and NO_COLOR is not set
 * to something (no-color.org) - one rule for every program */
int color_rule(int fd, const char *term, const char *no_color)
{
    return sys_isatty(fd) == 1 && term && *term && strcmp(term, "dumb") && !(no_color && *no_color);
}
int color_auto(int fd) { return color_rule(fd, getenv("TERM"), getenv("NO_COLOR")); }
int color_arg(const char *arg, int bare)
{
    const char *w;
    if (!strncmp(arg, "--colour", 8)) w = arg + 8;
    else if (!strncmp(arg, "--color", 7)) w = arg + 7;
    else return -1;
    if (!*w) return bare;
    if (*w++ != '=') return -1;
    if (!strcmp(w, "always") || !strcmp(w, "yes") || !strcmp(w, "force")) return 1;
    if (!strcmp(w, "never") || !strcmp(w, "no") || !strcmp(w, "none")) return 0;
    if (!strcmp(w, "auto") || !strcmp(w, "tty") || !strcmp(w, "if-tty")) return 2;
    return -2;
}

/* a basic regular expression (sed, expr :, the C library's regcomp without REG_EXTENDED) in the extended syntax
 * regex.c speaks: malloc'd, 0 when out of memory */
char *zs_bre_to_ere(const char *s)
{
    char *out = malloc(strlen(s) * 2 + 2), *o = out; int in_br = 0;
    if (!out) return 0;
    while (*s) {
        if (in_br) { *o++ = *s; if (*s == ']') in_br = 0; s++; continue; }
        if (*s == '[') { in_br = 1; *o++ = *s++; if (*s == '^') *o++ = *s++; if (*s == ']') *o++ = *s++; continue; }
        if (*s == '\\' && s[1]) {
            char c = s[1];
            if (c == '(' || c == ')' || c == '{' || c == '}' || c == '|' || c == '+' || c == '?') { *o++ = c; s += 2; continue; }
            *o++ = '\\'; *o++ = c; s += 2; continue;
        }
        if (*s == '(' || *s == ')' || *s == '{' || *s == '}' || *s == '|' || *s == '+' || *s == '?') { *o++ = '\\'; *o++ = *s++; continue; }
        *o++ = *s++;
    }
    *o = 0; return out;
}
