/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
static const char USAGE[] = "usage: rmdir dir...\n";
int main(int argc, char **argv)
{
    zoperands(&argc, &argv, USAGE);
    int rc = 0;
    if (argc < 2) zusage(USAGE);
    for (int i = 1; i < argc; i++) {
        int r = sys_rmdir(argv[i]);
        if (r == -ENOTEMPTY) { dprintf(2, "rmdir: %s: %s\n", argv[i], zstrerror(r)); rc = 1; }
        else if (r < 0) { dprintf(2, "rmdir: %s: %s\n", argv[i], zwhy("cannot remove")); rc = 1; }
    }
    return rc;
}
