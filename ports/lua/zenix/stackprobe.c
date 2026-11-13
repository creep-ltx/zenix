/* SPDX-License-Identifier: GPL-2.0-only */
/* Lua's stack probe for the test build (DESIGN D52 section 9, "The C stack"; ports/lua/zenix/luauser.h): when the
 * Lua state opens, ZX_PROBE_BYTES of the stack below this function's frame are filled with a pattern; when it
 * closes, the lowest word no longer holding it is the deepest the stack went, and the depth from the program's
 * arguments (the top of the stack, where the kernel put argv) to there is printed on standard error. A kernel guard
 * sees an overflow only at a system call or a tick; this sees every word written. The program's stack must be bigger
 * than ZX_PROBE_BYTES plus what lies above this frame (the Makefile builds it so). */
#include <stdint.h>
#include <stdio.h>

#define PAINT 0x5a7e5a7eu
extern char **_zargv;
static uint32_t *lo, *hi;

__attribute__((noinline)) void zx_stackprobe_open(void)
{
    hi = (uint32_t *)(((uintptr_t)__builtin_frame_address(0) - 256) & ~(uintptr_t)3);
    lo = hi - ZX_PROBE_BYTES / 4;
    for (volatile uint32_t *p = lo; p < hi; p++)
        *p = PAINT;
}

__attribute__((noinline)) void zx_stackprobe_close(void)
{
    uint32_t *p = lo;
    while (p < hi && *p == PAINT)
        p++;
    fprintf(stderr, "luaprobe: stack %ld bytes deep%s\n", (long)((char *)_zargv - (char *)p),
            p == lo ? " (all of the painted part: more than measured)" : "");
}
