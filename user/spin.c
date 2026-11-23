/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
static const char USAGE[] = "usage: spin [rounds]\n";
/* never yields: only preemption lets the others run (^C to stop) */
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int rounds = argc > 1 ? atoi(argv[1]) : 3;
    volatile unsigned n = 0;
    for (int r = 1; r <= rounds; r++) {
        for (unsigned i = 0; i < 400000; i++) n = n + 1;
        dprintf(1, "spin: round %d\n", r);
    }
    return 3;
}
