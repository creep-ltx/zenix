#include <stdio.h>
#include <signal.h>
#include <setjmp.h>

/* Catches its own protection fault: the handler longjmps out of the
 * faulting context (abandoning the preserved fault frame). */
static jmp_buf escape;

static void on_segv(int sig)
{
    (void)sig;
    longjmp(escape, 1);
}

int main(void)
{
    signal(SIGSEGV, on_segv);
    if (setjmp(escape)) {
        printf("segv: caught SIGSEGV and lived to tell - MMU + signals OK\n");
        return 0;
    }
    printf("segv: dereferencing kernel space 0x80000000...\n");
    fflush(stdout);
    int boom = *(volatile int *)0x80000000;
    printf("segv: read %d - SHOULD NOT HAPPEN\n", boom);
    return 1;
}
