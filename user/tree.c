// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* tree [-a] [-d] [directory...]: show a directory and everything below it as an indented tree (-a also names starting
 * with a dot, -d directories only) */
static int o_a, o_d, rc; static long ndirs, nfiles;
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void __attribute__((noinline)) show_link(const char *child)
{
    char t[160]; int n = sys_readlink(child, t, sizeof t - 1);
    if (n > 0) { t[n] = 0; if (n == (int)sizeof t - 1) memcpy(t + n - 3, "...", 3); dprintf(1, " -> %s", t); }   /* longer: shown cut */
}
static void walk(const char *path, const char *prefix)
{
    char **names; int nn = dir_list(path, &names);            /* heap, not stack: one level per directory */
    if (nn < 0) { dprintf(2, "tree: %s: %s\n", path, zwhy("cannot read")); rc = 1; return; }
    qsort(names, (u32)nn, sizeof *names, cmp);
    int last_i = -1;                                          /* the last name that is shown gets the corner */
    for (int i = 0; i < nn; i++) if (o_a || names[i][0] != '.') last_i = i;
    for (int i = 0; i < nn; i++) {
        if (!o_a && names[i][0] == '.') continue;
        char *child = path_join(path, names[i]);
        if (!child) { dprintf(2, "tree: %s/%s: %s\n", path, names[i], zwhy("cannot read")); rc = 1; continue; }
        struct ustat st; int le = sys_lstat(child, &st), isdir = le == 0 && (st.mode & S_IFMT) == S_IFDIR;
        if (le < 0 && le != -ENOENT) { dprintf(2, "tree: %s: %s\n", child, zwhy("cannot read")); rc = 1; free(child); continue; }
        if (o_d && !isdir) { free(child); continue; }
        int last = i == last_i;
        dprintf(1, "%s%s%s", prefix, last ? "`-- " : "|-- ", names[i]);
        if (sys_lstat(child, &st) == 0 && (st.mode & S_IFMT) == S_IFLNK) show_link(child);
        dprintf(1, "\n");
        if (isdir) {
            ndirs++;
            u32 pl = strlen(prefix);
            char *np = malloc(pl + 5);
            if (np) { memcpy(np, prefix, pl); strcpy(np + pl, last ? "    " : "|   "); walk(child, np); free(np); }
            else { dprintf(2, "tree: out of memory\n"); rc = 1; }
        } else nfiles++;
        free(child);
    }
    dir_free(names, nn);
}
static const char USAGE[] = "usage: tree [-a] [-d] [directory...]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int i = 1;
    for (; zisopt(argc, argv, &i, USAGE); i++)
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'a') o_a = 1; else if (*p == 'd') o_d = 1; else zbadch(*p, USAGE); }
    if (i >= argc) { dprintf(1, ".\n"); walk(".", ""); }
    for (; i < argc; i++) { dprintf(1, "%s\n", argv[i]); walk(argv[i], ""); }
    dprintf(1, "\n%ld director%s, %ld file%s\n", ndirs, ndirs == 1 ? "y" : "ies", nfiles, nfiles == 1 ? "" : "s");
    return rc;
}
