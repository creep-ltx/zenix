/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
int main(int argc, char **argv)
{
    if (argc > 1) { const char *v = getenv(argv[1]); if (!v) return 1; printf("%s\n", v); return 0; }
    getenv(""); for (char **e = environ; e && *e; e++) printf("%s\n", *e);
    return 0;
}
