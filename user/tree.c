// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* tree [-a] [-d] [directory...]: show a directory and everything below it as an indented tree (-a also names starting
 * with a dot, -d directories only) */
static int o_a, o_d, rc; static long ndirs, nfiles;
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void __attribute__((noinline)) show_link(const char *child)
{
    char t[160]; int n = sys_readlink(child, t, sizeof t - 1); if (n > 0) { t[n] = 0; printf(" -> %s", t); }
}
static void walk(const char *path, const char *prefix)
{
    char **names; int nn = dir_list(path, &names);            /* heap, not stack: one level per directory */
    if (nn < 0) { fprintf(2, "tree: %s: %s\n", path, zwhy("cannot read")); rc = 1; return; }
    qsort(names, (u32)nn, sizeof *names, cmp);
    int last_i = -1;                                          /* the last name that is shown gets the corner */
    for (int i = 0; i < nn; i++) if (o_a || names[i][0] != '.') last_i = i;
    for (int i = 0; i < nn; i++) {
        if (!o_a && names[i][0] == '.') continue;
        char *child = path_join(path, names[i]);
        if (!child) { fprintf(2, "tree: %s/%s: %s\n", path, names[i], zwhy("cannot read")); rc = 1; continue; }
        struct ustat st; int isdir = sys_lstat(child, &st) == 0 && (st.mode & S_IFMT) == S_IFDIR;
        if (o_d && !isdir) { free(child); continue; }
        int last = i == last_i;
        printf("%s%s%s", prefix, last ? "`-- " : "|-- ", names[i]);
        if (sys_lstat(child, &st) == 0 && (st.mode & S_IFMT) == S_IFLNK) show_link(child);
        printf("\n");
        if (isdir) {
            ndirs++;
            u32 pl = strlen(prefix);
            char *np = malloc(pl + 5);
            if (np) { memcpy(np, prefix, pl); strcpy(np + pl, last ? "    " : "|   "); walk(child, np); free(np); }
            else { fprintf(2, "tree: out of memory\n"); rc = 1; }
        } else nfiles++;
        free(child);
    }
    dir_free(names, nn);
}
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'a') o_a = 1; else if (*p == 'd') o_d = 1; else { fprintf(2, "tree: unknown option -%c\n", *p); return 2; } }
    if (i >= argc) { printf(".\n"); walk(".", ""); }
    for (; i < argc; i++) { printf("%s\n", argv[i]); walk(argv[i], ""); }
    printf("\n%ld director%s, %ld file%s\n", ndirs, ndirs == 1 ? "y" : "ies", nfiles, nfiles == 1 ? "" : "s");
    return rc;
}
