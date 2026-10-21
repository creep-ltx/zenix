#include "usys.h"
/* hands the kernel pointers that are not ours: must get -1, not a crash */
int main(void)
{
    int a = sys_write(1, (const void *)0x1000, 16);    /* low memory */
    int b = sys_write(1, (const void *)0xFFFFFFF0, 64);/* wraps */
    puts_("badptr: write(0x1000)="); putnum(a);
    puts_(" write(0xfffffff0)="); putnum(b); puts_("\n");
    return (a == -1 && b == -1) ? 0 : 1;
}
