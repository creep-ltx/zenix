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
#include "random.h"
/* This file is built with -Os (Makefile, KOPT_floppy: cold code, every wait timed); the MFM track coding is the one
 * loop that touches every word of a track, so it keeps -O2 */
#pragma GCC push_options
#pragma GCC optimize("O2")
#include "mfm.h"
#pragma GCC pop_options

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
#define RAW_WORDS 6656                  /* read length: a bit more than one revolution (about 6250 words) */
/* A write must be longer than one revolution, or a strip of the old track survives after the new data. A track holds
 * 6000-6812 words for the drive speeds and clocks a long-proven driver allows for (hardware hand-off note, 2026-10-23);
 * 836 words of gap + 11 sectors (5984) + the filler word = 6821 words. */
#define WRITE_GAP_WORDS 836
#define WRITE_WORDS (WRITE_GAP_WORDS + MFM_TRACK_WORDS + MFM_TAIL_WORDS)
#define RAW_BUF_WORDS (WRITE_WORDS > RAW_WORDS ? WRITE_WORDS : RAW_WORDS)
#define IDLE_TICKS 300                  /* motor off after 3 s */
/* Timings (HRM 3rd ed. p.244: 3 ms a step, 18 ms when the direction changes, 15 ms settle, the side line stable
 * 100 us before and 1.3 ms after a write). The hand-off note's longer values from long-proven drivers: 1.5 ms for the
 * heads to settle after a side or select change before any DMA, 4 ms a step towards track 0 while recalibrating (the
 * track-0 signal needs the extra millisecond), 4 ms after a write for the erase head (some drives need it). */
#define STEP_MS 3
#define CAL_STEP_MS 4
#define SIDE_SETTLE_US 1500
#define WRITE_TAIL_MS 4
#define READ_TRIES 10                   /* a full recalibration before every 4th */

#define A_RDY   0x20
#define A_TK0   0x10
#define A_PROT  0x08
#define A_CHNG  0x04

static struct {
    int probed, present, motor, cyl;    /* cyl = -1: unknown */
    int disk, prot;
    int changed;                        /* the disk-change line was seen: every request fails until fd_info */
    u32 last_use;
} drv[4];
static mfm_u16 *raw;                    /* chip RAM, RAW_BUF_WORDS words */
static mfm_u8 *cache;                   /* 11 sectors of the track in "ctrack" */
static mfm_u8 *keep;                    /* a written track's sectors, to verify (allocated with cache: 5.5 KB of BSS
                                         * on every machine before, used or not; the 2 MB diet, DESIGN D18) */
static int cdrive = -1, ctrack = -1;
static u16 cmask;                       /* which sectors of the cached track are valid */
static volatile int busy;
static int lastdir;                     /* 1 out, 2 in: the direction of the last step */

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

static void step(int d, int side, int inward, u32 ms)
{
    u8 m = (u8)drv[d].motor;
    CIAB_PRB = pb(d, 1, m, side, inward, 0);            /* the direction needs its own write before the pulse */
    wait_us(2);
    CIAB_PRB = pb(d, 1, m, side, inward, 1);            /* step pulse: low ... */
    wait_us(5);
    CIAB_PRB = pb(d, 1, m, side, inward, 0);            /* ... and high again */
    wait_ms(ms);
    lastdir = inward + 1;
}

/* Find track 0 (hand-off note F5): if the track-0 signal is already on, the head may sit beyond it - step inward until
 * it goes off (at most 40), pause for the change of direction, then step outward at 4 ms a step until it comes on (at
 * most 80 cylinders + 20), then let the head settle. */
static int recalibrate(int d)
{
    CIAB_PRB = pb(d, 1, drv[d].motor, 0, 0, 0);
    wait_us(10);
    if (!(CIAA_PRA & A_TK0)) {
        for (int n = 0; !(CIAA_PRA & A_TK0) && n < 40; n++)
            step(d, 0, 1, STEP_MS);
        wait_ms(18);
    } else if (lastdir == 2)
        wait_ms(18);
    for (int n = 0; (CIAA_PRA & A_TK0) && n < 100; n++)
        step(d, 0, 0, CAL_STEP_MS);
    if (CIAA_PRA & A_TK0)
        return -1;
    drv[d].cyl = 0;
    wait_ms(15);
    return 0;
}

/* select drive d, move the heads to cylinder cyl (recalibrating first if the position is unknown), select the side
 * and give the heads 1.5 ms to settle before the DMA (F3) */
static int seek(int d, int cyl, int side)
{
    u8 m = (u8)drv[d].motor;
    CIAB_PRB = pb(d, 1, m, side, 0, 0);
    wait_us(10);
    if (drv[d].cyl < 0 && recalibrate(d))
        return -1;
    if (cyl != drv[d].cyl) {
        int inward = cyl > drv[d].cyl;
        if (lastdir != inward + 1)
            wait_ms(18);                                /* a reversal needs a longer pause */
        while (drv[d].cyl != cyl) {
            step(d, side, inward, STEP_MS);
            drv[d].cyl += inward ? 1 : -1;
        }
        wait_ms(15);                                    /* settle */
    }
    CIAB_PRB = pb(d, 1, m, side, 0, 0);
    wait_us(SIDE_SETTLE_US);
    return 0;
}

static void deselect(void)
{
    CIAB_PRB = 0xFF;
}

/* The disk-change line, read with the drive selected (and the side kept): 1 = the disk was taken out since the last
 * step with a disk in (HRM Appendix H). 20 us after the select: one drive make needs about 12 (hand-off note F7). */
static int change_line(int d, int side)
{
    CIAB_PRB = pb(d, 1, drv[d].motor, side, 0, 0);
    wait_us(20);
    return !(CIAA_PRA & A_CHNG);
}

/* The change line was seen: whatever is cached or known about this disk is stale. Requests fail until fd_info (the
 * disk server calls it on every open of the drive) has looked at the new disk. */
static void disk_changed(int d)
{
    if (cdrive == d)
        cdrive = -1;
    drv[d].cyl = -1;                                    /* recalibrate: a new disk, perhaps a moved head */
    if (!drv[d].changed)
        kprintf("floppy: df%d: the disk was changed or taken out; requests fail until it is opened again\n", d);
    drv[d].changed = 1;
}

/* is a disk in the drive? a step makes the drive update its disk-change line (it resets only on a step with a disk
 * in); the side line is left as given */
static void media(int d, int side)
{
    if (change_line(d, side)) {                         /* no disk, or one that was just taken out: step to refresh */
        if (drv[d].cyl >= 0) {
            int inward = drv[d].cyl == 0;
            step(d, side, inward, STEP_MS);
            step(d, side, !inward, STEP_MS);
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
        media(d, 0);
    } else
        deselect();
    return drv[d].present;
}

/* DESIGN D29 (ZINE) 7.2 step 8 and 9.2: before and after an Amiga program had the machine: every motor off, the
 * head positions unknown (recalibrated at the next use) and the track cache dropped (it may have moved the heads or
 * changed the disk) */
void fd_zine(void)
{
    for (int d = 0; d < 4; d++)
        if (drv[d].present) {
            set_motor(d, 0);
            drv[d].cyl = -1;
        }
    cdrive = -1;
    deselect();
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
        media((int)d, 0);
        drv[d].changed = 0;                             /* the disk is looked at afresh from here */
        r = 1 | (drv[d].disk ? 2 : 0) | (drv[d].prot ? 4 : 0);
        if (drv[d].disk)
            r |= SECTORS << 8;
        drv[d].last_use = ticks;
    }
    busy = 0;
    return r;
}

/* One DMA transfer. The disk-block flag must come within the time a sync word can take to arrive (300 ms: one turn,
 * the speed slop, a sector and the gap, hand-off note F6) plus the transfer itself (2 us a bit cell, +10%). */
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
    u32 t0 = now(), limit = eclock_hz / 1000 * ((write ? 20u : 300u) + (u32)words * 32u * 11u / 10000u);
    (void)CIAB(CIA_ICR);                                /* an index pulse from before: not this wait's */
    while (!(INTREQR & INTF_DSKBLK)) {
        if (CIAB(CIA_ICR) & 0x10)                       /* FLG: the index pulse (CIA-B interrupts are never enabled, */
            rnd_event(RND_CR_FLOPPY);                   /* its flags are free): when it came is the motor's jitter */
        if ((u32)(now() - t0) > limit) {
            DSKLEN = 0x4000;
            return -1;
        }
    }
    if (write)
        wait_ms(WRITE_TAIL_MS);                         /* the last bits are still on their way to the head */
    DSKLEN = 0x4000;
    return 0;
}

/* Read track "track" of drive d until the sectors in "want" are in the cache. Good sectors are kept from try to try
 * (a weak sector costs only itself: only the request that needs it fails); a full recalibration before every 4th try
 * (lost steps are a common cause). The buffer is cleared before each read, so a DMA that did not run can never
 * leave an older track's sectors to be decoded again. Returns 0, -1 (unreadable), -2 (the disk was changed). */
static int read_track(int d, int track, u16 want)
{
    if (cdrive == d && ctrack == track && (cmask & want) == want)
        return 0;
    if (cdrive != d || ctrack != track) {
        cdrive = d;
        ctrack = track;
        cmask = 0;
    }
    for (int tries = 0; tries < READ_TRIES; tries++) {
        if (tries && tries % 4 == 0)
            drv[d].cyl = -1;
        if (seek(d, track >> 1, track & 1)) {
            drv[d].cyl = -1;
            continue;
        }
        memset(raw, 0, RAW_WORDS * 2);
        int r = dma(0, RAW_WORDS, track >> 1);
        if (change_line(d, track & 1)) {
            disk_changed(d);
            return -2;
        }
        if (r == 0)
            cmask |= (u16)mfm_decode_track(raw, RAW_WORDS, track, cache);
        if ((cmask & want) == want)
            return 0;
    }
    kprintf("floppy: df%d track %d: cannot read it (sectors %x of %x missing)\n", d, track, want & ~cmask, want);
    return -1;
}

/* Write the cached track (all 11 sectors valid) and read it back to compare. Never a track that was not read
 * cleanly (or wholly given by the caller). */
static int write_track(int d, int track)
{
    if (disk_frozen)
        return -1;                                      /* the reset warning's flush is done (kbd.c) */
    int words = mfm_encode_track(raw, WRITE_GAP_WORDS, track, cache);
    if (seek(d, track >> 1, track & 1))
        return -1;
    if (change_line(d, track & 1)) {                    /* (seek kept the drive selected and the side set) */
        disk_changed(d);
        return -2;
    }
    if (!(CIAA_PRA & A_PROT)) {
        kprintf("floppy: df%d is write protected\n", d);
        return -1;
    }
    int r = dma(1, words, track >> 1);
    if (change_line(d, track & 1)) {
        disk_changed(d);
        return -2;
    }
    if (r)
        return -1;
    memcpy(keep, cache, 11 * 512);                      /* check what really got onto the disk */
    cmask = 0;
    if (read_track(d, track, 0x7FF))
        return -1;
    int differ = 0;
    for (u32 i = 0; i < 11 * 512; i++)
        differ |= keep[i] ^ cache[i];
    if (differ) {
        kprintf("floppy: df%d track %d: verify after write failed\n", d, track);
        cmask = 0;
        return -1;
    }
    return 0;
}

int fd_rw(u32 d, u32 lba, u32 count, void *buf, int write)
{
    if (d > 3 || !count || lba + count > SECTORS || lba + count < lba)
        return -1;
    if (write && disk_frozen)
        return -1;
    if (!raw)
        raw = mem_alloc(RAW_BUF_WORDS * 2, MEM_CHIP, 0);
    if (!cache && (cache = kmalloc(2 * 11 * 512)))
        keep = cache + 11 * 512;
    if (!raw || !cache)
        return -1;
    busy = 1;
    int rc = -1;
    if (!probe((int)d) || drv[d].changed)
        goto out;
    if (change_line((int)d, 0)) {                       /* swapped or taken out since the last request: fail at once */
        media((int)d, 0);
        disk_changed((int)d);
        goto out;
    }
    spin_up((int)d);
    u8 *p = buf;
    while (count) {
        int track = (int)(lba / 11), sec = (int)(lba % 11);
        u32 n = 11 - (u32)sec < count ? 11 - (u32)sec : count;
        u16 want = (u16)(((1u << n) - 1) << sec);
        if (write) {
            if (n < 11 && read_track((int)d, track, 0x7FF))         /* the rest of the track must be read cleanly */
                goto out;
            if (n == 11) {                              /* the whole track is new: nothing to read first */
                cdrive = (int)d;
                ctrack = track;
            }
            memcpy(cache + 512 * sec, p, n * 512);
            cmask = 0x7FF;
            if (write_track((int)d, track))
                goto out;
        } else {
            if (read_track((int)d, track, want))
                goto out;
            memcpy(p, cache + 512 * sec, n * 512);
        }
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
