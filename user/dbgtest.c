// SPDX-License-Identifier: GPL-2.0-only
/* dbgtest - the program tools/zdb-test.sh debugs (DESIGN D54 section 21). Built with a frame pointer, so zdb's bt has a
 * chain; a plain (ZEXE) and a shared-code (ZEX2) build.
 *   dbgtest N       count to N through three calls, print the counter and the marker, status 3
 *   dbgtest wait    print "ready", then read a line from standard input before counting to 5 (a copy held at a pipe)
 *   dbgtest loop    count for ever, a short sleep a round (to be attached to)
 *   dbgtest crash   three calls deep, then a division by zero
 *   dbgtest trap    a system call in a loop of 3 (zdb's step over a TRAP) */
#include "libz.h"

volatile int counter;
volatile int marker = 7;

__attribute__((noinline)) void leaf(int i)
{
    counter += i;
    if (i == 3)
        marker += 1;
}

__attribute__((noinline)) void middle(int i) { leaf(i); }
__attribute__((noinline)) void outer(int i) { middle(i); }

volatile int zero;
__attribute__((noinline)) int boom(void) { return counter / zero; }      /* a division by zero: vector 5 on every CPU */
__attribute__((noinline)) int b2(void) { return boom() + 1; }
__attribute__((noinline)) int b1(void) { return b2() + 1; }

int main(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "5";
    int n = 5;
    if (!strcmp(a, "crash")) {
        return b1();
    }
    if (!strcmp(a, "trap")) {
        for (int i = 0; i < 3; i++)
            sys_getpid();
        oprintf("trap done\n");
        return 4;
    }
    if (!strcmp(a, "loop")) {
        oprintf("looping\n");
        oflush();
        for (;;) {
            outer(1);
            sys_sleep(2);
        }
    }
    if (!strcmp(a, "sysport")) {        /* makes a reserved port while its debugger waits (a system process before D60) */
        sys_sleep(20);
        return sys_port_create("fs.dbgk") >= 0 ? 6 : 7;
    }
    if (!strcmp(a, "wait")) {
        char c;
        oprintf("ready\n");
        oflush();
        sys_read(0, &c, 1);
    } else
        n = atoi(a);
    for (int i = 1; i <= n; i++)
        outer(i);
    oprintf("counter %d marker %d\n", counter, marker);
    return 3;
}
