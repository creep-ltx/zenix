/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
static const char USAGE[] = "usage: mkdir [-pv] dir...\n";
/* mkdir [-p] [-v] dir... */
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int o_p = 0, o_v = 0, i = 1, rc = 0;
    for (; zisopt(argc, argv, &i, USAGE); i++) for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'p') o_p = 1; else if (*p == 'v') o_v = 1; else zbadch(*p, USAGE); }
    if (i >= argc) zusage(USAGE);
    for (; i < argc; i++) {
        char *path = argv[i];                           /* (-p cuts it at each slash in turn and puts the slash back) */
        if (o_p) {
            for (char *q = path + 1; *q; q++) if (*q == '/') { *q = 0; struct ustat st; if (sys_stat(path, &st) < 0) sys_mkdir(path); *q = '/'; }
            struct ustat st; if (sys_stat(path, &st) == 0 && (st.mode & S_IFMT) == S_IFDIR) continue;
        }
        int r = sys_mkdir(path);
        if (r == -EEXIST) { dprintf(2, "mkdir: %s: %s\n", argv[i], zstrerror(r)); rc = 1; }
        else if (r < 0) { dprintf(2, "mkdir: %s: %s\n", argv[i], zwhy("cannot create")); rc = 1; }
        else if (o_v) dprintf(1, "mkdir: created directory '%s'\n", argv[i]);
    }
    return rc;
}
