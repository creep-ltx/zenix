// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* tree [-a] [-d] [directory...]: show a directory and everything below it as an indented tree (-a also names starting
 * with a dot, -d directories only) */
static int o_a, o_d; static long ndirs, nfiles;
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void walk(const char *path, const char *prefix)
{
    int fd = sys_open(path, O_RDONLY); if (fd < 0) return;
    char **names = malloc(512 * sizeof *names); int nn = 0; struct udirent d;      /* heap, not stack: one level per directory */
    char *child = malloc(256), *np = malloc(160);
    if (!names || !child || !np) { fprintf(2, "tree: out of memory\n"); sys_close(fd); return; }
    while (sys_readdir(fd, &d) > 0 && nn < 512) {
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..") || (!o_a && d.name[0] == '.')) continue;
        names[nn++] = strdup(d.name);
    }
    sys_close(fd);
    qsort(names, (u32)nn, sizeof *names, cmp);
    for (int i = 0; i < nn; i++) {
        snprintf(child, 256, "%s/%s", path, names[i]);
        struct ustat st; int isdir = sys_lstat(child, &st) == 0 && (st.mode & S_IFMT) == S_IFDIR;
        if (o_d && !isdir) continue;
        int last = i == nn - 1;
        printf("%s%s%s", prefix, last ? "`-- " : "|-- ", names[i]);
        if (sys_lstat(child, &st) == 0 && (st.mode & S_IFMT) == S_IFLNK) { char t[160]; int n = sys_readlink(child, t, sizeof t - 1); if (n > 0) { t[n] = 0; printf(" -> %s", t); } }
        printf("\n");
        if (isdir) { snprintf(np, 160, "%s%s", prefix, last ? "    " : "|   "); ndirs++; char *c2 = strdup(child), *p2 = strdup(np); walk(c2, p2); } else nfiles++;
    }
}
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'a') o_a = 1; else if (*p == 'd') o_d = 1; else { fprintf(2, "tree: unknown option -%c\n", *p); return 2; } }
    if (i >= argc) { printf(".\n"); walk(".", ""); }
    for (; i < argc; i++) { printf("%s\n", argv[i]); walk(argv[i], ""); }
    printf("\n%ld director%s, %ld file%s\n", ndirs, ndirs == 1 ? "y" : "ies", nfiles, nfiles == 1 ? "" : "s");
    return 0;
}
