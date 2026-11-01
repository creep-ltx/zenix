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

/* The names in a directory, without "." and "..", in the order the filesystem gives them: *names is a malloc'd
 * array (as long as needed, NULL-terminated) of malloc'd strings, given back with dir_free. Returns the count, or
 * -1 when the directory cannot be opened (zerrno from the system call) or memory runs out (zerrno 12); *names is
 * then NULL and nothing is left allocated. If reading fails part way (a damaged disk), *err is that error and the
 * names read so far are returned: what can be seen is seen. Without a failure *err is 0. */
int dir_list_some(const char *path, char ***names, int *err)
{
    *names = 0;
    *err = 0;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    char **v = 0; int n = 0, cap = 0, r; struct udirent d;
    while ((r = sys_readdir(fd, &d)) > 0) {
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
        if (n + 1 >= cap) {
            int nc = cap ? cap * 2 : 32;
            char **nv = realloc(v, (u32)nc * sizeof *nv);
            if (!nv) goto oom;
            v = nv; cap = nc;
        }
        if (!(v[n] = strdup(d.name))) goto oom;
        n++;
    }
    if (r < 0 && r != -ENOTDIR) *err = -r;            /* (not a directory: no names, as it always was) */
    sys_close(fd);
    if (!v && !(v = malloc(sizeof *v))) { zerrno = ENOMEM; return -1; }
    v[n] = 0;
    *names = v;
    return n;
oom:
    sys_close(fd);
    dir_free(v, n);
    zerrno = ENOMEM;
    return -1;
}
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
