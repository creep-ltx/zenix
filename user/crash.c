/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* crash: an illegal instruction - the kernel must kill only us.
 * crash stack: recurse until the user stack overflows - the stack guard must kill us with "overflowed its user stack".
 * crash kstack / crash idle: only in a STACK_TEST kernel (tools/diag-test.sh stack): damage the guard of our own kernel
 * stack / of pid 0's, as an overflow would.
 * The memory protection tests (DESIGN D41, tools/mmu-test.sh): crash victim N [FILE]: a region filled with a pattern,
 * its address said, N seconds later (or once FILE exists) checked ("intact" or "damaged"); crash read ADDR: a read there; crash write ADDR: a
 * write there, refused unless the kernel's log says protection is on (without it the write would land). */
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

static int protected_on(void)          /* the boot's "mmu: memory protection on:" line in the kernel's log */
{
    static char b[1025];
    static const char want[] = "mmu: memory protection on:";
    for (unsigned off = 0; off < 65536; off += 1024 - sizeof want) {
        int n = sys_klog(off, b, 1024);
        if (n <= 0)
            return 0;
        b[n] = 0;
        if (strstr(b, want))
            return 1;
        if (n < 1024)
            return 0;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "victim")) {
        unsigned n = 8192, bad = 0;
        volatile unsigned *r = sys_memalloc(n);
        if (!r) { dprintf(1, "crash: no memory\n"); return 1; }
        for (unsigned i = 0; i < n / 4; i++) r[i] = 0x5A17C0DEu ^ i;
        dprintf(1, "crash: victim pid %d region %p\n", sys_getpid(), (void *)r);
        oflush();
        for (int t = atoi(argv[2]) * 10; t > 0; t--) {    /* N s, or until the file argv[3] is there */
            struct ustat st;
            if (argc > 3 && sys_stat(argv[3], &st) == 0)
                break;
            sys_sleep(10);
        }
        for (unsigned i = 0; i < n / 4; i++) bad += r[i] != (0x5A17C0DEu ^ i);
        dprintf(1, "crash: victim pid %d %s\n", sys_getpid(), bad ? "damaged" : "intact");
        return bad != 0;
    }
    if (argc > 2 && (!strcmp(argv[1], "read") || !strcmp(argv[1], "write"))) {
        volatile unsigned *a = (volatile unsigned *)strtoul(argv[2], 0, 0);
        if (argv[1][0] == 'w' && !protected_on()) {
            dprintf(1, "crash: memory protection is off: no write to %p\n", (void *)a);
            return 2;
        }
        dprintf(1, "crash: %s %p\n", argv[1][0] == 'r' ? "reading" : "writing", (void *)a);
        oflush();
        if (argv[1][0] == 'r')
            dprintf(1, "crash: read %x\n", *a);
        else
            *a = 0x5A5A5A5Au;
        dprintf(1, "crash: still here\n");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "sleep")) {
        for (int t = atoi(argv[2]) * 10; t > 0; t--) {    /* N s, or until the file argv[3] is there */
            struct ustat st;
            if (argc > 3 && sys_stat(argv[3], &st) == 0)
                break;
            sys_sleep(10);
        }
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
