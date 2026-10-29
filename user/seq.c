/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* seq [first [step]] last */
int main(int argc, char **argv)
{
    long a = 1, s = 1, b;
    if (argc == 2) b = atoi(argv[1]); else if (argc == 3) { a = atoi(argv[1]); b = atoi(argv[2]); } else if (argc == 4) { a = atoi(argv[1]); s = atoi(argv[2]); b = atoi(argv[3]); }
    else { dprintf(2, "usage: seq [first [step]] last\n"); return 2; }
    if (!s) return 2;
    for (long v = a; s > 0 ? v <= b : v >= b; v += s) oprintf("%d\n", (int)v);
    return 0;
}
