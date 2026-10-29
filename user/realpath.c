// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* realpath [-e] path...: print the absolute name with ., .. and symbolic links resolved (-e: the path must exist).
 * Links are followed as the kernel follows them (DESIGN D26): at most 40 in one path, then "cannot resolve" (libz's
 * zrealpath, shared with wasabid) */
int main(int argc, char **argv)
{
    int must = 0, i = 1, rc = 0;
    if (i < argc && !strcmp(argv[i], "-e")) { must = 1; i++; }
    if (i >= argc) { dprintf(2, "usage: realpath [-e] path...\n"); return 2; }
    for (; i < argc; i++) {
        static char out[160]; struct ustat st;
        if (zrealpath(argv[i], out, sizeof out) < 0) { dprintf(2, "realpath: %s: cannot resolve\n", argv[i]); rc = 1; continue; }
        if (must && sys_stat(out, &st) < 0) { dprintf(2, "realpath: %s: not found\n", argv[i]); rc = 1; continue; }
        dprintf(1, "%s\n", out);
    }
    return rc;
}
