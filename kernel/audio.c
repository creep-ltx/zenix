/* SPDX-License-Identifier: GPL-2.0-only */
/* Paula audio: a square-wave beep on the left and right channel (0 and 1), for the terminal bell.
 *
 * Paula plays 8-bit signed samples from chip RAM by DMA: AUDxLC (pointer), AUDxLEN (length in words),
 * AUDxPER (clock ticks per sample), AUDxVOL (0-64); the channel's enable bit in DMACON starts it, and
 * it repeats the sample until the bit is cleared. Frequency = clock / (period * samples per cycle), the
 * clock being 3546895 Hz on PAL and 3579545 Hz on NTSC (PAL assumed: the pitch is not critical).
 * Not heard in an emulator without sound, so the driver logs what it saw: after a beep it reports whether
 * the channel raised its end-of-sample interrupt flag (INTREQ bit 7), which only happens if the DMA ran.
 */
#include "kernel.h"
#include "hw.h"

#define AUD0LCH   REG16(0xDFF0A0)
#define AUD0LCL   REG16(0xDFF0A2)
#define AUD0LEN   REG16(0xDFF0A4)
#define AUD0PER   REG16(0xDFF0A6)
#define AUD0VOL   REG16(0xDFF0A8)
#define AUD1LCH   REG16(0xDFF0B0)
#define AUD1LCL   REG16(0xDFF0B2)
#define AUD1LEN   REG16(0xDFF0B4)
#define AUD1PER   REG16(0xDFF0B6)
#define AUD1VOL   REG16(0xDFF0B8)
#define DMAF_AUD01 0x0003
#define INTF_AUD0  0x0080
#define WAVE_BYTES 32
#define PAL_CLOCK  3546895u

static signed char *wave;                        /* one cycle of a square wave in chip RAM */
static u32 remaining;                   /* ticks (1/100 s) left of the current beep */
static u32 seen;                        /* end-of-sample flags seen while it ran */
static u32 beeps;

static void stop(void)
{
    DMACON = DMAF_AUD01;                /* clear: both channels off */
    AUD0VOL = 0;
    AUD1VOL = 0;
}

void beep(u32 hz, u32 ms)
{
    if (!wave) {
        wave = mem_alloc(WAVE_BYTES, MEM_CHIP, 0);
        if (!wave)
            return;
        for (int i = 0; i < WAVE_BYTES; i++)
            wave[i] = i < WAVE_BYTES / 2 ? 100 : -100;
    }
    if (hz < 100) hz = 100;
    if (hz > 4000) hz = 4000;
    u32 per = PAL_CLOCK / (hz * WAVE_BYTES);
    if (per < 124) per = 124;           /* Paula's fastest rate */
    stop();
    AUD0LCH = (u16)((u32)wave >> 16); AUD0LCL = (u16)(u32)wave;
    AUD1LCH = (u16)((u32)wave >> 16); AUD1LCL = (u16)(u32)wave;
    AUD0LEN = AUD1LEN = WAVE_BYTES / 2;
    AUD0PER = AUD1PER = (u16)per;
    AUD0VOL = AUD1VOL = 24;             /* a quarter of full volume */
    INTREQ = INTF_AUD0;
    seen = 0;
    remaining = (ms + 9) / 10;
    DMACON = DMAF_SETCLR | DMAF_MASTER | DMAF_AUD01;
}

/* called from the 100 Hz tick */
void audio_tick(void)
{
    if (!remaining)
        return;
    if (INTREQR & INTF_AUD0) {
        seen++;
        INTREQ = INTF_AUD0;
    }
    if (--remaining == 0) {
        stop();
        if (beeps++ == 0)               /* once: say whether the hardware did something */
            kprintf("audio: first beep done, channel 0 end-of-sample flag seen %u time(s)%s\n", seen,
                    seen ? "" : " - NO DMA activity");
    }
}
