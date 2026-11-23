/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
static const char USAGE[] = "usage: basename path [suffix]\n";
/* basename path [suffix] */
int main(int argc, char **argv)
{
    zoperands(&argc, &argv, USAGE);
    if (argc < 2) zusage(USAGE);
    char *p = strdup(argv[1]); u32 l = strlen(p);
    while (l > 1 && p[l - 1] == '/') p[--l] = 0;
    char *b = strrchr(p, '/'); b = (b && l > 1) ? b + 1 : p;
    if (argc > 2) { u32 sl = strlen(argv[2]), bl = strlen(b); if (bl > sl && !strcmp(b + bl - sl, argv[2])) b[bl - sl] = 0; }
    dprintf(1, "%s\n", b);
    return 0;
}
