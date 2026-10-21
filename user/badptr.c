#include "libz.h"
/* hands the kernel pointers that are not ours: must get -1, not a crash */
int main(void)
{
    int a = sys_write(1, (const void *)0x1000, 16);
    int b = sys_write(1, (const void *)0xFFFFFFF0, 64);
    printf("badptr: write(0x1000)=%d write(0xfffffff0)=%d\n", a, b);
    return (a == -1 && b == -1) ? 0 : 1;
}
