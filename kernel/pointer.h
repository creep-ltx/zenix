/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D61 (docs/d61-mouse.md 4): one axis of the pointer's move in a frame, shared by the kernel's vertical blank
 * (gcon.c) and the host test (tools/mouse-host-test.c). P is the pointer's place in 1/256ths of its units (the
 * screen's pixels, as the display server named them with CON_MOUSE), D the mouse's counts this frame. The counts
 * beyond THR count MULT/256 times (AmigaOS's acceleration: a threshold, then a multiplier; 256 is none), then GAIN/256
 * units a count; the fraction stays in P, so a slow mouse moves the pointer by single pixels and none is lost. The
 * place stays within 0 .. LIM-1 units. Ranges (CON_MOUSE checks them): THR 0..127, MULT 256..2048, GAIN 1..4096, LIM
 * 1..4096; D is cut to +-255, so the product fits 32 bits unsigned: a mouse's counters are 8 bits and read twice a
 * frame or more, so only an injected jump (CON_INPUT, the tests) ever reaches the cut. */
#ifndef ZENIX_POINTER_H
#define ZENIX_POINTER_H

static inline s32 ptr_step(s32 p, s32 d, u32 thr, u32 mult, u32 gain, s32 lim)
{
    u32 a = (u32)(d < 0 ? -d : d), v;
    if (a > 255)
        a = 255;
    v = a > thr ? (thr << 8) + (a - thr) * mult : a << 8;     /* counts, in 1/256ths */
    v = v * gain >> 8;                                          /* units, in 1/256ths */
    p += d < 0 ? -(s32)v : (s32)v;
    lim <<= 8;
    return p < 0 ? 0 : p >= lim ? lim - 1 : p;
}

#endif
