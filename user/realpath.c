// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
static const char USAGE[] = "usage: realpath [-e] path...\n";
/* realpath [-e] path...: print the absolute name with ., .. and symbolic links resolved (-e: the path must exist).
 * Links are followed as the kernel follows them (DESIGN D26): at most 40 in one path, then "cannot resolve" (libz's
 * zrealpath, shared with wasabid) */
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int must = 0, i = 1, rc = 0;
    if (i < argc && !strcmp(argv[i], "-e")) { must = 1; i++; }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    else if (i < argc && argv[i][0] == '-' && argv[i][1]) zbadopt(argv[i], USAGE);
    if (i >= argc) zusage(USAGE);
    for (; i < argc; i++) {
        static char out[ZPATH_MAX]; struct ustat st;
        if (zrealpath(argv[i], out, sizeof out) < 0) { dprintf(2, "realpath: %s: cannot resolve\n", argv[i]); rc = 1; continue; }
        if (must) { int e = sys_stat(out, &st); if (e < 0) { dprintf(2, "realpath: %s: %s\n", argv[i], zstrerror(e)); rc = 1; continue; } }
        dprintf(1, "%s\n", out);
    }
    return rc;
}
