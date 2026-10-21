#include "libz.h"
/* never yields: only preemption lets the others run (^C to stop) */
int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 3;
    volatile unsigned n = 0;
    for (int r = 1; r <= rounds; r++) {
        for (unsigned i = 0; i < 400000; i++) n = n + 1;
        printf("spin: round %d\n", r);
    }
    return 3;
}
