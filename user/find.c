/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* find [path...] [-name PAT] [-iname PAT] [-type f|d] [-maxdepth N] [-mindepth N] [-print] [-exec cmd {} ;] [-delete]
 * Patterns: * ? [..]. The tests are and-ed; with no action the path is printed. Symbolic links below the starting
 * points are never followed (GNU find's default -P), so -delete cannot leave the tree; with -delete a starting point
 * that is a link is not followed either. */
static const char *p_name, *p_iname; static int p_type, maxdepth = 100000, mindepth, do_print = 1, do_delete;
static char **exec_argv; static int exec_n, rc;

static int gm(const char *p, const char *s, int icase)
{
    for (;; p++, s++) {
        if (*p == '*') { while (*p == '*') p++; if (!*p) return 1; for (;; s++) { if (gm(p, s, icase)) return 1; if (!*s) return 0; } }
        if (!*s) return !*p;
        if (*p == '?') continue;
        if (*p == '[') {
            int neg = p[1] == '!' || p[1] == '^', ok = 0; const char *q = p + 1 + neg;
            for (; *q && *q != ']'; q++) { if (q[1] == '-' && q[2] && q[2] != ']') { if (*s >= q[0] && *s <= q[2]) ok = 1; q += 2; } else if (*q == *s) ok = 1; }
            if (ok == neg || !*q) return 0;
            p = q; continue;
        }
        int a = (unsigned char)*p, b = (unsigned char)*s;
        if (icase) { a = tolower(a); b = tolower(b); }
        if (a != b) return 0;
    }
}

/* -exec: its buffers live in this function, not in visit(), which recurses once per directory level */
static void __attribute__((noinline)) run_exec(const char *path)
{
    char **av = xmalloc((u32)(exec_n + 1) * sizeof(char *)); int n = 0;
    for (int i = 0; i < exec_n; i++) av[n++] = !strcmp(exec_argv[i], "{}") ? (char *)path : exec_argv[i];
    av[n] = 0;
    char prog[ZPATH_MAX];
    oflush();
    if (!path_find(av[0], prog, sizeof prog)) { dprintf(2, "find: %s: command not found\n", av[0]); rc = 1; free(av); return; }
    int pid = sys_spawn(prog, av, 0);
    if (pid < 0) { dprintf(2, "find: %s: %s\n", av[0], zwhy("cannot run")); rc = 1; }
    else sys_wait(pid);                                     /* as GNU find: the command's own status does not change find's */
    free(av);
}
static void visit(const char *path, const char *name, int depth, const struct ustat *st)
{
    int isdir = (st->mode & S_IFMT) == S_IFDIR;
    if (depth >= mindepth) {
        int ok = 1;
        if (p_name && !gm(p_name, name, 0)) ok = 0;
        if (p_iname && !gm(p_iname, name, 1)) ok = 0;
        if (p_type == 'f' && (st->mode & S_IFMT) != S_IFREG) ok = 0;
        if (p_type == 'd' && !isdir) ok = 0;
        if (ok) {
            if (do_print) { oputs(path); oputc('\n'); }
            if (exec_n) run_exec(path);
            if (do_delete && !isdir) sys_unlink(path);
        }
    }
    if (isdir && depth < maxdepth) {
        char **names; int nn = dir_list(path, &names);        /* heap, not stack: one level per directory */
        if (nn < 0) { dprintf(2, "find: %s: %s\n", path, zwhy("cannot read")); rc = 1; return; }
        for (int i = 0; i < nn; i++) {
            char *child = path_join(path, names[i]);
            if (!child) { dprintf(2, "find: %s/%s: %s\n", path, names[i], zwhy("cannot read")); rc = 1; continue; }
            struct ustat cs; int le = sys_lstat(child, &cs);
            if (le == 0) visit(child, names[i], depth + 1, &cs);        /* a link is not followed */
            else if (le != -ENOENT) { dprintf(2, "find: %s: %s\n", child, zwhy("cannot read")); rc = 1; }
            free(child);
        }
        dir_free(names, nn);
        if (do_delete && depth >= mindepth) sys_rmdir(path);
    }
}
int main(int argc, char **argv)
{
    int i = 1, np = 0; char **paths = argv + 1;
    for (; i < argc && argv[i][0] != '-' && argv[i][0] != '!'; i++) np++;
    static char *dot[] = { ".", 0 };
    if (!np) { paths = dot; np = 1; }
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-name") && i + 1 < argc) p_name = argv[++i];
        else if (!strcmp(argv[i], "-iname") && i + 1 < argc) p_iname = argv[++i];
        else if (!strcmp(argv[i], "-type") && i + 1 < argc) p_type = argv[++i][0];
        else if (!strcmp(argv[i], "-maxdepth") && i + 1 < argc) maxdepth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-mindepth") && i + 1 < argc) mindepth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-print")) do_print = 1;
        else if (!strcmp(argv[i], "-delete")) { do_delete = 1; do_print = 0; }
        else if (!strcmp(argv[i], "-exec")) { exec_argv = argv + i + 1; do_print = 0; while (i + 1 < argc && strcmp(argv[i + 1], ";")) { i++; exec_n++; } i++; }
        else { dprintf(2, "find: unknown expression %s\n", argv[i]); return 2; }
    }
    for (int k = 0; k < np; k++) {
        struct ustat st;
        if ((do_delete ? sys_lstat(paths[k], &st) : sys_stat(paths[k], &st)) < 0) { dprintf(2, "find: %s: %s\n", paths[k], zwhy("not found")); rc = 1; continue; }
        const char *nm = strrchr(paths[k], '/'); nm = nm && nm[1] ? nm + 1 : paths[k];
        visit(paths[k], nm, 0, &st);
    }
    return rc;
}
