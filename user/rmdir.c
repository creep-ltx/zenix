/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) { dprintf(2, "usage: rmdir dir...\n"); return 2; }
    for (int i = 1; i < argc; i++) {
        int r = sys_rmdir(argv[i]);
        if (r == -ENOTEMPTY) { dprintf(2, "rmdir: %s: not empty\n", argv[i]); rc = 1; }
        else if (r < 0) { dprintf(2, "rmdir: %s: %s\n", argv[i], zwhy("cannot remove")); rc = 1; }
    }
    return rc;
}
