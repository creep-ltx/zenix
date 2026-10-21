#include "usys.h"
/* illegal instruction (the 68020 allows odd addresses, so no address
 * error): the kernel must kill only us */
int main(void)
{
    puts_("crash: about to fault\n");
    __asm__ volatile("illegal");
    puts_("crash: not reached\n");
    return 0;
}
