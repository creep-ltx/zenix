#include "libz.h"
int main(void)
{
    u32 s = sys_ticks() / 100;
    printf("up %u:%02u:%02u\n", s / 3600, (s / 60) % 60, s % 60);
    return 0;
}
