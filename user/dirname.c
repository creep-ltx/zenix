/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
static const char USAGE[] = "usage: dirname path\n";
/* dirname path */
int main(int argc, char **argv)
{
    zoperands(&argc, &argv, USAGE);
    if (argc < 2) zusage(USAGE);
    char *p = strdup(argv[1]); u32 l = strlen(p);
    while (l > 1 && p[l - 1] == '/') p[--l] = 0;
    char *s = strrchr(p, '/');
    if (!s) { dprintf(1, ".\n"); return 0; }
    while (s > p && s[-1] == '/') s--;
    if (s == p) { dprintf(1, "/\n"); return 0; }
    *s = 0; dprintf(1, "%s\n", p);
    return 0;
}
