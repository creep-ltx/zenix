/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D61 (docs/d61-mouse.md 4): the pointer's arithmetic, kernel/pointer.h, on the PC: D38's old moves exactly
 * (two pixels a count across, the acceleration "low" of before, measured in FS-UAE 2026-11-23), single pixels at slow
 * speed with no count lost, the fraction kept both ways, the screen's edges, and the largest values without overflow. */
#include <stdio.h>
#include <stdint.h>
typedef int32_t s32;
typedef uint32_t u32;
#include "../kernel/pointer.h"

static int fails, checks;
static void ok(int c, const char *what)
{
    checks++;
    if (!c) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

int main(void)
{
    /* D38's moves, as gcontest measured them before D61 (accel 0x204: beyond 4 counts doubled, 2 pixels a count) */
    static const int d[] = { 1, 1, 2, 3, 4, 5, 6, 8, 12, 20 }, want[] = { 10, 12, 16, 22, 30, 42, 58, 82, 122, 194 };
    s32 p = 8 << 8;
    int same = 1;
    for (int i = 0; i < 10; i++) {
        p = ptr_step(p, d[i], 4, 0x200, 0x200, 640);
        same &= p >> 8 == want[i] && (p & 0xFF) == 0;
    }
    ok(same, "D38's old moves exactly (gain 2, threshold 4, multiplier 2): 10 12 16 22 30 42 58 82 122 194");

    /* gain 1: every count one pixel, a slow sweep of 1 count a frame across the whole screen */
    p = 0;
    int ones = 1;
    for (int i = 1; i < 640; i++) {
        s32 q = ptr_step(p, 1, 4, 0x200, 0x100, 640);
        ones &= (q >> 8) - (p >> 8) == 1;
        p = q;
    }
    ok(ones && p >> 8 == 639, "gain 1: a count a frame moves exactly one pixel, 0 .. 639");
    p = ptr_step(p, 1, 4, 0x200, 0x100, 640);
    ok(p >> 8 == 639, "... and stops at the last column");

    /* a fractional gain (0.4 lines a count, the vertical of speed 80%): no count lost, the steps never above one */
    p = 0;
    int maxstep = 0;
    for (int i = 0; i < 500; i++) {
        s32 q = ptr_step(p, 1, 4, 0x200, 102, 512);
        if ((q >> 8) - (p >> 8) > maxstep)
            maxstep = (q >> 8) - (p >> 8);
        p = q;
    }
    ok(p == 500 * 102 && maxstep == 1, "gain 102/256: 500 counts give exactly 500 x 102 256ths, in steps of one line");
    s32 top = p;
    int maxback = 0;
    for (int i = 0; i < 500; i++) {
        s32 q = ptr_step(p, -1, 4, 0x200, 102, 512);
        if ((p >> 8) - (q >> 8) > maxback)
            maxback = (p >> 8) - (q >> 8);
        p = q;
    }
    ok(top == 500 * 102 && p == 0 && maxback == 1, "... and 500 counts back: at 0 again, in steps of one line");

    /* the fraction is kept both ways: there and back again lands on the same place */
    p = 100 << 8;
    for (int i = 0; i < 37; i++)
        p = ptr_step(p, 3, 4, 0x200, 77, 640);
    ok(p == (100 << 8) + 37 * 231, "37 moves of 3 counts right (gain 77/256): 37 x 231 256ths further, none lost");
    for (int i = 0; i < 37; i++)
        p = ptr_step(p, -3, 4, 0x200, 77, 640);
    ok(p == 100 << 8, "37 moves of 3 counts right then left (gain 77/256): back on the same 256th");

    /* acceleration: below the threshold the same as none; beyond it the multiplier, continuous at the threshold */
    s32 a = ptr_step(1000 << 8, 4, 4, 0x300, 0x100, 4096), b = ptr_step(1000 << 8, 4, 0, 0x100, 0x100, 4096);
    ok(a == b, "at the threshold: no acceleration yet");
    a = ptr_step(1000 << 8, 5, 4, 0x300, 0x100, 4096);
    ok(a == (1000 + 4 + 3) << 8, "one count beyond a threshold of 4, multiplier 3: 4 + 3 pixels");
    a = ptr_step(1000 << 8, 6, 4, 0x180, 0x100, 4096);
    ok(a == (1000 + 4 + 3) << 8, "two counts beyond, multiplier 1.5: 4 + 3 pixels");

    /* the edges: below 0 is 0, the fraction dropped there; the largest values do not overflow */
    ok(ptr_step(5 << 8, -100, 0, 0x100, 0x100, 640) == 0, "left of the screen: 0");
    p = ptr_step(0, 255, 0, 0x800, 0x1000, 4096);
    ok(p == ((4096 << 8) - 1), "255 counts, multiplier 8, gain 16: clamped at the edge, no overflow");
    p = ptr_step(4095 << 8, -255, 0, 0x800, 0x1000, 4096);
    ok(p == 0, "... and the same leftwards: 0");
    p = ptr_step(2048 << 8, 1000, 0, 0x100, 0x100, 4096);
    ok(p == (2048 + 255) << 8, "a jump of 1,000 counts is cut to 255");
    printf("%s mouse-host-test: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
