/* SPDX-License-Identifier: GPL-2.0-only */
/* make's stack probe for the test build (DESIGN D52 section 4; tools/make-test.sh): the linker's --wrap=main makes
 * the start code call __wrap_main, which fills ZX_PROBE_BYTES of the stack below its own frame with a pattern, runs
 * upstream's main, and at the program's end (an atexit function: make leaves through exit() when it fails) prints
 * the depth of the lowest word that no longer holds the pattern, counted from the program's arguments (the top of
 * the stack, where the kernel put argv), on standard error. A kernel guard sees an overflow only at a system call or
 * a tick; this sees every word written. No change to upstream's source. The stack must be bigger than ZX_PROBE_BYTES
 * plus what lies above this frame (the Makefile builds it so). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define PAINT 0x5a7e5a7eu
extern char **_zargv;
extern int __real_main(int argc, char **argv);
static uint32_t *lo, *hi;

static void report(void)
{
    uint32_t *p = lo;
    while (p < hi && *p == PAINT)
        p++;
    fprintf(stderr, "makeprobe: stack %ld bytes deep%s\n", (long)((char *)_zargv - (char *)p),
            p == lo ? " (all of the painted part: more than measured)" : "");
}

__attribute__((noinline)) int __wrap_main(int argc, char **argv)
{
    hi = (uint32_t *)(((uintptr_t)__builtin_frame_address(0) - 256) & ~(uintptr_t)3);
    lo = hi - ZX_PROBE_BYTES / 4;
    for (volatile uint32_t *p = lo; p < hi; p++)
        *p = PAINT;
    atexit(report);
    return __real_main(argc, argv);
}
