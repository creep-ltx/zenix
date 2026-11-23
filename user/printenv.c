/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
static const char USAGE[] = "usage: printenv [name]\n";
int main(int argc, char **argv)
{
    zoperands(&argc, &argv, USAGE);
    if (argc > 1) { const char *v = getenv(argv[1]); if (!v) return 1; dprintf(1, "%s\n", v); return 0; }
    getenv(""); for (char **e = environ; e && *e; e++) dprintf(1, "%s\n", *e);
    return 0;
}
