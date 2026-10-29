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

/* the modes for the shared-code tests (DESIGN D13; tools/pure-test.sh runs a ZEX2 build of crash, /tests/crashpure):
 * crash sleep N: N seconds, then exit 0 (another process running the same code); crash poke: change a byte of our own
 * constants (in a ZEX2 build: the shared code everyone running it uses), then exit 0; crash damage: the same, then the
 * fault; crash literal: system calls with string literals (read into one must fail with -EFAULT in a ZEX2 build) */
static const char victim[32] = "a constant the poke changes";
static const char lit[16] = "literal text";

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "sleep")) {
        sys_sleep((u32)atoi(argv[2]) * 100);
        dprintf(1, "crash: slept %s s\n", argv[2]);
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "poke") || !strcmp(argv[1], "damage"))) {
        *(volatile char *)(unsigned long)victim ^= 0x20;    /* no MMU: nothing stops it */
        dprintf(1, "crash: changed a byte of my constants\n");
        oflush();
        if (argv[1][0] == 'p')
            return 0;
        __asm__ volatile("illegal");
    }
    if (argc > 1 && !strcmp(argv[1], "literal")) {
        int fd = sys_open("/etc/passwd", 0);
        char buf[4];
        int n = fd >= 0 ? sys_read(fd, buf, 4) : fd;
        int r = fd >= 0 ? sys_read(fd, (char *)(unsigned long)lit, 4) : fd;
        sys_write(1, "crash: a literal written\n", 25);
        int c = sys_spawn("/bin/true", 0, 0);
        dprintf(1, "crash: open of a literal path %d, read %d, read into a literal %d, it says: %s, spawn %s\n",
               fd >= 0, n, r, lit, c > 0 && sys_wait(c) == 0 ? "ok" : "failed");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "stack")) {
        dprintf(1, "crash: recursing until the stack overflows\n");
        oflush();
        return deep(0);
    }
    if (argc > 1 && (!strcmp(argv[1], "kstack") || !strcmp(argv[1], "idle"))) {
        dprintf(1, "crash: damaging the guard of %s kernel stack\n", argv[1][0] == 'k' ? "our" : "pid 0's");
        oflush();
        sys3(3, argv[1][0] == 'k' ? 0x4B53544B : 0x49444C45, 0, 0);    /* SYS_yield with the test word */
        sys_yield();
        dprintf(1, "crash: still here (not a STACK_TEST kernel)\n");
        return 1;
    }
    dprintf(1, "crash: about to fault\n");
    __asm__ volatile("illegal");
    dprintf(1, "crash: not reached\n");
    return 0;
}
