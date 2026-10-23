/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* crash: an illegal instruction - the kernel must kill only us.
 * crash stack: recurse until the user stack overflows - the stack guard must kill us with "overflowed its user stack".
 * crash kstack / crash idle: only in a STACK_TEST kernel (tools/diag-test.sh stack): damage the guard of our own kernel
 * stack / of pid 0's, as an overflow would. */
static int deep(int n)
{
    volatile char frame[200];           /* about 220 bytes a level: 16 KB of stack is gone after some 75 */
    frame[0] = (char)n;
    if (n > 100000)                     /* never reached: the guard stops it long before (and gcc wants an exit) */
        return 0;
    sys_getpid();                       /* a system call per level: the guard is checked at each one */
    return deep(n + 1) + frame[0];
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "stack")) {
        printf("crash: recursing until the stack overflows\n");
        oflush();
        return deep(0);
    }
    if (argc > 1 && (!strcmp(argv[1], "kstack") || !strcmp(argv[1], "idle"))) {
        printf("crash: damaging the guard of %s kernel stack\n", argv[1][0] == 'k' ? "our" : "pid 0's");
        oflush();
        sys3(3, argv[1][0] == 'k' ? 0x4B53544B : 0x49444C45, 0, 0);    /* SYS_yield with the test word */
        sys_yield();
        printf("crash: still here (not a STACK_TEST kernel)\n");
        return 1;
    }
    printf("crash: about to fault\n");
    __asm__ volatile("illegal");
    printf("crash: not reached\n");
    return 0;
}
