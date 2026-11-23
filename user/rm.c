/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* rm [-f] [-r] [-v] file... : remove files; -r removes directories and what is in them */
static int o_f, o_r, o_v, rc;
static void rmtree(const char *path, int top)
{
    struct ustat st;
    int le = sys_lstat(path, &st);
    if (le == -ENOENT) { if (!o_f) { dprintf(2, "rm: %s: %s\n", path, zstrerror(le)); rc = 1; } return; }
    if (le < 0) { dprintf(2, "rm: %s: %s\n", path, zstrerror(le)); rc = 1; return; }  /* there but out of reach: -f hides only "not there" */
    if ((st.mode & S_IFMT) == S_IFDIR) {
        if (!o_r) { dprintf(2, "rm: %s: is a directory (use -r)\n", path); rc = 1; return; }
        char **names; int n = dir_list(path, &names);         /* heap, not stack: one level per directory */
        if (n < 0) { dprintf(2, "rm: %s: %s\n", path, zwhy("cannot read")); rc = 1; return; }   /* not all its names known: it stays, and says why */
        for (int i = 0; i < n; i++) {
            char *c = path_join(path, names[i]);
            if (c) rmtree(c, 0); else { dprintf(2, "rm: %s/%s: %s\n", path, names[i], zwhy("cannot remove")); rc = 1; }
            free(c);
        }
        dir_free(names, n);
        if (sys_rmdir(path) < 0) { dprintf(2, "rm: %s: %s\n", path, zwhy("cannot remove")); rc = 1; } else if (o_v) dprintf(1, "removed directory '%s'\n", path);
    } else {
        if (sys_unlink(path) < 0) { dprintf(2, "rm: %s: %s\n", path, zwhy("cannot remove")); rc = 1; } else if (o_v) dprintf(1, "removed '%s'\n", path);
    }
    (void)top;
}
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'f') o_f = 1; else if (*p == 'r' || *p == 'R') o_r = 1; else if (*p == 'v') o_v = 1; else { dprintf(2, "rm: unknown option -%c\n", *p); return 2; } }
    }
    if (i >= argc) { if (!o_f) { dprintf(2, "usage: rm [-frv] file...\n"); return 2; } return 0; }
    for (; i < argc; i++) rmtree(argv[i], 1);
    return rc;
}
