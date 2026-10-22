#include <stdio.h>

/* Deliberately violates protection: user mode touching the kernel's
 * virtual address. The MMU must kill us; the shell should report
 * [exit 130] (128 + vector 2, bus error). */
int main(void)
{
    printf("crash: about to write to kernel space 0x80000000...\n");
    fflush(stdout);
    *(volatile unsigned *)0x80000000 = 0xDEADBEEF;
    printf("crash: STILL ALIVE - PROTECTION IS BROKEN!\n");
    return 0;
}
