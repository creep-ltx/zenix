/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* ln [-s] [-f] target link_name   |   ln [-s] [-f] target... directory */
int main(int argc, char **argv)
{
    int sym = 0, force = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) for (const char *p = argv[i] + 1; *p; p++) { if (*p == 's') sym = 1; else if (*p == 'f') force = 1; else { dprintf(2, "ln: unknown option -%c\n", *p); return 2; } }
    int n = argc - i;
    if (n < 2) { dprintf(2, "usage: ln [-sf] target link_name\n"); return 2; }
    struct ustat st; int rc = 0;
    int todir = n > 2 || (sys_stat(argv[argc - 1], &st) == 0 && (st.mode & S_IFMT) == S_IFDIR);
    for (; i < argc - 1; i++) {
        char *link;
        if (todir) { const char *b = strrchr(argv[i], '/'); b = b ? b + 1 : argv[i]; link = path_join(argv[argc - 1], b); }
        else link = strdup(argv[argc - 1]);
        if (!link) { dprintf(2, "ln: %s: %s\n", argv[i], zwhy("cannot create the link")); rc = 1; continue; }
        if (force) sys_unlink(link);
        int r = sym ? sys_symlink(argv[i], link) : sys_link(argv[i], link);
        if (r < 0) {                    /* zwhy() skips error 1 (EPERM: a bare -1 from an old call looks the same) */
            const char *why = zstrerror(r);
            if (!sym && sys_stat(argv[i], &st) == 0 && (st.mode & S_IFMT) == S_IFDIR)
                why = "hard link not allowed for directory";        /* GNU ln's words */
            dprintf(2, "ln: cannot create %s: %s\n", link, why ? why : "failed");
            rc = 1;
        }
        free(link);
    }
    return rc;
}
