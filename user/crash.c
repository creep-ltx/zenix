#include "libz.h"
/* illegal instruction: the kernel must kill only us */
int main(void)
{
    printf("crash: about to fault\n");
    __asm__ volatile("illegal");
    printf("crash: not reached\n");
    return 0;
}
