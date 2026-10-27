/* SPDX-License-Identifier: GPL-2.0-only */
/* Mouse and joystick ports: polled from the 100 Hz tick.
 *
 * A mouse or joystick in port 1 (the left one, "port 0") or port 2 ("port 1") is read through JOY0DAT /
 * JOY1DAT: two 8-bit counters (vertical in the high byte, horizontal in the low byte) that a mouse's quadrature
 * pulses count up and down; a joystick's direction switches show in the low bits of the same words. The
 * buttons: the left mouse button / the joystick fire button is a CIA-A port A input (bit 6 port 1, bit 7 port 2,
 * low = pressed); the other mouse buttons are POTGOR inputs. Mouse movement is accumulated (the counters are
 * 8 bits, so at 100 Hz a mouse can move up to 127 counts per tick without losing any) and handed over
 * with input_read(), which clears it.
 */
#include "kernel.h"
#include "hw.h"
#include "random.h"

#define JOY0DAT REG16(0xDFF00A)
#define JOY1DAT REG16(0xDFF00C)
#define POTGOR  REG16(0xDFF016)
#define POTGO   REG16(0xDFF034)
#define CIAA_PRA REG8(0xBFE001)

static u16 last0, last1;
static s32 dx, dy;                       /* mouse in port 1 (JOY0DAT) accumulated since the last read */
static u32 buttons, joy;
static int inited;

void input_init(void)
{
    POTGO = 0xFF00;                      /* pot pins as inputs, data high: the pot buttons read as inputs */
    last0 = JOY0DAT;
    last1 = JOY1DAT;
    inited = 1;
}

/* called from the 100 Hz tick */
void input_poll(void)
{
    if (!inited)
        return;
    u16 j0 = JOY0DAT, j1 = JOY1DAT;
    if (j0 != last0)
        rnd_isr(j0, 0);                  /* mouse movement: mixed, never credited (D19 9.3) */
    dx += (signed char)((j0 & 0xFF) - (last0 & 0xFF));
    dy += (signed char)((j0 >> 8) - (last0 >> 8));
    last0 = j0;
    last1 = j1;
    u8 pra = CIAA_PRA;
    u16 pot = POTGOR;
    u32 b = 0;
    if (!(pra & 0x40)) b |= 1;           /* left button, port 1 */
    if (!(pot & 0x0400)) b |= 2;         /* right button, port 1 */
    if (!(pot & 0x0100)) b |= 4;         /* middle button, port 1 */
    buttons = b;
    u32 j = 0;                           /* joystick in port 2 */
    if ((j1 >> 9 ^ j1 >> 8) & 1) j |= 1;         /* up */
    if ((j1 >> 1 ^ j1) & 1) j |= 2;              /* down */
    if (j1 & 0x0200) j |= 4;                     /* left */
    if (j1 & 0x0002) j |= 8;                     /* right */
    if (!(pra & 0x80)) j |= 16;                  /* fire */
    joy = j;
}

/* state for the SYS_input call: out[0] = mouse dx, out[1] = mouse dy (both since the last call, cleared),
 * out[2] = mouse buttons (bit 0 left, 1 right, 2 middle), out[3] = joystick in port 2 (bit 0 up, 1 down,
 * 2 left, 3 right, 4 fire) */
void input_read(s32 *out)
{
    u16 sv = irq_save2();
    out[0] = dx; out[1] = dy; out[2] = (s32)buttons; out[3] = (s32)joy;
    dx = dy = 0;
    irq_restore(sv);
}
