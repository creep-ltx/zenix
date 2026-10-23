/* SPDX-License-Identifier: GPL-2.0-only */
/* Amiga floppy drives: the disk DMA channel of Paula and the drive control lines on the CIAs.
 *
 * Written from the Amiga Hardware Reference Manual (chapter 8, the disk interface) and the ROM Kernel Manual
 * (Devices, "Commodore-Amiga disk format"); the MFM track coding is in mfm.h and tested on the host.
 * Standard 880 KB disks only (80 cylinders, 2 sides, 11 sectors of 512 bytes): sector number = (cylinder * 2 +
 * side) * 11 + sector in the track. Also what a Gotek or other floppy emulator presents.
 *
 * How: the drive lines are CIA-B port B (0xBFD100: bit 7 motor, 6..3 drive select 3..0, 2 side, 1 direction,
 * 0 step; all active low) and CIA-A port A inputs (bit 5 ready, 4 track 0, 3 write protect, 2 disk changed;
 * active low, valid while the drive is selected). A track is read by DMA into chip RAM: DSKSYNC = 0x4489 with
 * WORDSYNC in ADKCON starts the DMA at the first sync word; one revolution (plus some) is read and decoded in
 * memory. A write re-encodes the whole track from the cache. Polling only (the disk-block interrupt flag is
 * read, never enabled), one operation at a time. Motors go off 3 seconds after the last use (fd_tick).
 */
#include "kernel.h"
#include "hw.h"
#include "mfm.h"

#define DSKPTH  (*(volatile u32 *)0xDFF020)
#define DSKLEN  REG16(0xDFF024)
#define DSKSYNC REG16(0xDFF07E)
#define ADKCON  REG16(0xDFF09E)
#define CIAB_PRB REG8(0xBFD100)
#define CIAB_DDRB REG8(0xBFD300)
#define CIAA_PRA REG8(0xBFE001)
#define DMAF_DISK 0x0010
#define INTF_DSKBLK 0x0002

#define SECTORS 1760
#define RAW_WORDS 6656                  /* a bit more than one revolution (about 6250 words) */
#define WRITE_GAP_WORDS 224
#define IDLE_TICKS 300                  /* motor off after 3 s */

#define A_RDY   0x20
#define A_TK0   0x10
#define A_PROT  0x08
#define A_CHNG  0x04

static struct {
    int probed, present, motor, cyl;    /* cyl = -1: unknown */
    int disk, prot;
    u32 last_use;
} drv[4];
static mfm_u16 *raw;                    /* chip RAM, RAW_WORDS words */
static mfm_u8 *cache;                   /* 11 sectors of the track in "ctrack" */
static int cdrive = -1, ctrack = -1;
static volatile int busy;

/* hr_time() reads the CIA-B E-clock counter (hardware only, never early), so a wait can trust "now - start" */
static u32 now(void)
{
    return hr_time();
}

static void wait_us(u32 us)
{
    u32 t0 = now(), need = us * (eclock_hz / 1000) / 1000 + 1;     /* E-clock: 709379 Hz PAL, 715909 NTSC */
    while ((u32)(now() - t0) < need)
        ;
}
static void wait_ms(u32 ms) { wait_us(ms * 1000); }

/* the CIA-B port B value: motor, select, side, direction, step */
static u8 pb(int d, int sel, int motor, int side, int dir_in, int step)
{
    u8 v = 0xFF;
    if (motor) v &= (u8)~0x80;
    if (sel) v &= (u8)~(1 << (3 + d));
    if (side) v &= (u8)~0x04;           /* 0 = upper head = side 1 */
    if (dir_in) v &= (u8)~0x02;         /* 0 = towards the centre */
    if (step) v &= (u8)~0x01;
    return v;
}

static void set_motor(int d, int on)
{
    CIAB_PRB = pb(d, 0, on, 0, 0, 0);   /* the motor line first, then select: the drive latches it */
    wait_us(5);
    CIAB_PRB = pb(d, 1, on, 0, 0, 0);
    wait_us(5);
    CIAB_PRB = pb(d, 0, on, 0, 0, 0);
    drv[d].motor = on;
    if (!on)
        CIAB_PRB = 0xFF;
}

static void spin_up(int d)
{
    if (!drv[d].motor) {
        set_motor(d, 1);
        CIAB_PRB = pb(d, 1, 1, 0, 0, 0);
        u32 t0 = now();
        while ((CIAA_PRA & A_RDY) && (u32)(now() - t0) < eclock_hz * 8 / 10)   /* ready, or 800 ms */
            ;
        CIAB_PRB = 0xFF;
    }
}

static void step(int d, int side, int inward)
{
    u8 m = (u8)drv[d].motor;
    CIAB_PRB = pb(d, 1, m, side, inward, 0);            /* the direction needs its own write before the pulse */
    wait_us(2);
    CIAB_PRB = pb(d, 1, m, side, inward, 1);            /* step pulse: low ... */
    wait_us(5);
    CIAB_PRB = pb(d, 1, m, side, inward, 0);            /* ... and high again */
    wait_ms(3);
}

/* select drive d, move the heads to cylinder cyl (recalibrating first if the position is unknown) */
static int seek(int d, int cyl, int side)
{
    u8 m = (u8)drv[d].motor;
    CIAB_PRB = pb(d, 1, m, side, 0, 0);
    wait_us(10);
    if (drv[d].cyl < 0) {                               /* recalibrate: step out until track 0 */
        int n = 0;
        while ((CIAA_PRA & A_TK0) && n++ < 90)
            step(d, side, 0);
        if (CIAA_PRA & A_TK0)
            return -1;
        drv[d].cyl = 0;
        wait_ms(15);
    }
    if (cyl != drv[d].cyl) {
        int inward = cyl > drv[d].cyl;
        static int lastdir;
        if (lastdir != inward + 1)
            wait_ms(18);                                /* a reversal needs a longer pause */
        lastdir = inward + 1;
        while (drv[d].cyl != cyl) {
            step(d, side, inward);
            drv[d].cyl += inward ? 1 : -1;
        }
        wait_ms(15);                                    /* settle */
    }
    CIAB_PRB = pb(d, 1, m, side, 0, 0);
    wait_us(120);                                       /* the side line must be stable */
    return 0;
}

static void deselect(void)
{
    CIAB_PRB = 0xFF;
}

/* is a disk in the drive? a step makes the drive update its disk-change line */
static void media(int d)
{
    u8 m = (u8)drv[d].motor;
    CIAB_PRB = pb(d, 1, m, 0, 0, 0);
    wait_us(20);
    if (!(CIAA_PRA & A_CHNG)) {                         /* no disk, or one that was just taken out: step to refresh */
        if (drv[d].cyl >= 0) {
            int inward = drv[d].cyl == 0;
            step(d, 0, inward);
            step(d, 0, !inward);
        }
        wait_us(20);
    }
    drv[d].disk = (CIAA_PRA & A_CHNG) != 0;
    drv[d].prot = !(CIAA_PRA & A_PROT);
    deselect();
}

static int probe(int d)
{
    if (drv[d].probed)
        return drv[d].present;
    drv[d].probed = 1;
    drv[d].cyl = -1;
    CIAB_DDRB = 0xFF;
    deselect();
    if (seek(d, 0, 0) == 0)
        drv[d].present = 1;
    if (drv[d].present) {
        deselect();
        media(d);
    } else
        deselect();
    return drv[d].present;
}

/* returns: bit 0 drive present, bit 1 disk inserted, bit 2 write protected; the sector count << 8 */
int fd_info(u32 d)
{
    if (d > 3)
        return 0;
    busy = 1;
    int r = 0;
    if (probe((int)d)) {
        if (cdrive == (int)d)
            cdrive = -1;                                /* a disk may have been changed: forget the cached track */
        media((int)d);
        r = 1 | (drv[d].disk ? 2 : 0) | (drv[d].prot ? 4 : 0);
        if (drv[d].disk)
            r |= SECTORS << 8;
        drv[d].last_use = ticks;
    }
    busy = 0;
    return r;
}

static int dma(int write, int words, int cyl)
{
    DMACON = DMAF_SETCLR | DMAF_MASTER | DMAF_DISK;
    INTREQ = INTF_DSKBLK;
    ADKCON = 0x7F00;                                    /* clear the disk bits ... */
    if (write)
        ADKCON = (u16)(0x9100 | (cyl >= 40 ? 0x2000 : 0));      /* MFM, fast; precompensation on the inner tracks */
    else {
        ADKCON = 0x9500;                                /* MFM, fast, WORDSYNC */
        DSKSYNC = 0x4489;
    }
    DSKPTH = (u32)raw;
    u16 len = (u16)(0x8000 | (write ? 0x4000 : 0) | words);
    DSKLEN = 0x4000;
    DSKLEN = len;
    DSKLEN = len;                                       /* the second write starts the DMA */
    u32 t0 = now();
    while (!(INTREQR & INTF_DSKBLK))
        if ((u32)(now() - t0) > eclock_hz * 2) {
            DSKLEN = 0x4000;
            return -1;
        }
    if (write)
        wait_ms(3);                                     /* the last words are still on their way to the head */
    DSKLEN = 0x4000;
    return 0;
}

static int read_track(int d, int track)
{
    if (cdrive == d && ctrack == track)
        return 0;
    for (int tries = 0; tries < 6; tries++) {
        if (seek(d, track >> 1, track & 1)) {
            drv[d].cyl = -1;
            continue;
        }
        if (dma(0, RAW_WORDS, track >> 1) == 0 && mfm_decode_track(raw, RAW_WORDS, track, cache) == 0x7FF) {
            cdrive = d;
            ctrack = track;
            return 0;
        }
    }
    kprintf("floppy: df%d track %d: cannot read it\n", d, track);
    return -1;
}

static int write_track(int d, int track)
{
    int words = mfm_encode_track(raw, WRITE_GAP_WORDS, track, cache);
    if (seek(d, track >> 1, track & 1))
        return -1;
    media(d);
    if (drv[d].prot) {
        kprintf("floppy: df%d is write protected\n", d);
        return -1;
    }
    CIAB_PRB = pb(d, 1, drv[d].motor, track & 1, 0, 0);
    wait_us(120);
    if (dma(1, words, track >> 1))
        return -1;
    cdrive = -1;                                        /* check what really got onto the disk */
    static mfm_u8 keep[11 * 512];
    memcpy(keep, cache, sizeof keep);
    if (read_track(d, track))
        return -1;
    int differ = 0;
    for (u32 i = 0; i < sizeof keep; i++)
        differ |= keep[i] ^ cache[i];
    if (differ) {
        kprintf("floppy: df%d track %d: verify after write failed\n", d, track);
        return -1;
    }
    return 0;
}

int fd_rw(u32 d, u32 lba, u32 count, void *buf, int write)
{
    if (d > 3 || !count || lba + count > SECTORS || lba + count < lba)
        return -1;
    if (!raw)
        raw = mem_alloc(RAW_WORDS * 2, MEM_CHIP, 0);
    if (!cache)
        cache = kmalloc(11 * 512);
    if (!raw || !cache)
        return -1;
    busy = 1;
    int rc = -1;
    if (!probe((int)d))
        goto out;
    spin_up((int)d);
    u8 *p = buf;
    while (count) {
        int track = (int)(lba / 11), sec = (int)(lba % 11);
        if (read_track((int)d, track))
            goto out;
        u32 n = 11 - (u32)sec < count ? 11 - (u32)sec : count;
        if (write) {
            memcpy(cache + 512 * sec, p, n * 512);
            if (write_track((int)d, track))
                goto out;
        } else
            memcpy(p, cache + 512 * sec, n * 512);
        p += n * 512; lba += n; count -= n;
    }
    rc = 0;
out:
    deselect();
    drv[d].last_use = ticks;
    busy = 0;
    return rc;
}

/* from the 100 Hz tick: motors off after the idle time */
void fd_tick(void)
{
    if (busy)
        return;
    for (int d = 0; d < 4; d++)
        if (drv[d].motor && (u32)(ticks - drv[d].last_use) > IDLE_TICKS) {
            busy = 1;
            set_motor(d, 0);
            busy = 0;
        }
}
