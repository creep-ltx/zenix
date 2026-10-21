#include "usys.h"
/* never yields: only preemption lets the others run */
int main(void)
{
    volatile unsigned n = 0;
    for (int round = 1; round <= 3; round++) {
        for (unsigned i = 0; i < 400000; i++) n++;
        puts_("spin: round "); putnum(round); puts_("\n");
    }
    return 3;
}
