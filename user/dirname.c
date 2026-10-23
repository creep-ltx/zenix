/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* dirname path */
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(2, "usage: dirname path\n"); return 2; }
    char *p = strdup(argv[1]); u32 l = strlen(p);
    while (l > 1 && p[l - 1] == '/') p[--l] = 0;
    char *s = strrchr(p, '/');
    if (!s) { printf(".\n"); return 0; }
    while (s > p && s[-1] == '/') s--;
    if (s == p) { printf("/\n"); return 0; }
    *s = 0; printf("%s\n", p);
    return 0;
}
