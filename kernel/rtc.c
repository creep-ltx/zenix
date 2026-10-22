// SPDX-License-Identifier: GPL-2.0-only
/* Battery-backed clock at $DC0000: the Oki MSM6242 (A500/A600/A2000 add-ons, most A1200 trapdoor clocks) or the
 * Ricoh RP5C01 (A3000, A4000, some add-ons). Both have 16 four-bit registers, register n is the low nibble of the
 * byte at $DC0003 + 4n:
 *
 *     n:  0   1   2   3   4   5   6   7   8   9   10  11  12  13  14  15
 *   MSM:  s1  s10 m1  m10 h1  h10 d1  d10 mo1 mo10 y1 y10 wd  CD  CE  CF
 *   RP:   s1  s10 m1  m10 h1  h10 wd  d1  d10 mo1 mo10 y1 y10 MODE TEST RESET     (bank 0; bank 1 reg 10 bit 0 = 24 h)
 *
 * Without a clock this space mirrors the custom chips, so nothing is written, and the clock counts as present only
 * when its seconds are seen ticking. Same logic as the Fuzix Amiga port (verified in FS-UAE with both chip types).
 * Two year digits: 78-99 are 19xx, 00-77 are 20xx (FS-UAE's MSM6242 keeps years since 1900: taken mod 100). */
#include "kernel.h"

#define RTC(n)  (*(volatile u8 *)(0xDC0003UL + ((n) << 2)))
#define VHPOSR  (*(volatile u8 *)0xDFF006)

static u8 nib(u8 n) { return RTC(n) & 0x0F; }
static u8 two(u8 lo) { return nib(lo) + 10 * nib(lo + 1); }

static void wait_lines(u16 n)           /* a scan line is about 64 us */
{
    while (n--) { u8 c = VHPOSR; while (VHPOSR == c) ; }
}

static int date_ok(u8 d, u8 wd)         /* do the registers from d on hold a sane date? (day d, month d + 2) */
{
    u8 day = two(d), mon = two(d + 2);
    return day >= 1 && day <= 31 && mon >= 1 && mon <= 12 && nib(d + 4) <= 9 && nib(wd) <= 6;
}

static u32 days_from_civil(u32 y, u32 m, u32 d)
{
    y -= m <= 2;
    u32 era = y / 400, yoe = y - era * 400;
    u32 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Seconds since 1970, or 0 when there is no usable clock. */
u32 rtc_read(void)
{
    u8 i, s, mode = 0, msm_type;
    if ((nib(13) & 8) && (nib(13) & 3)) {       /* Kickstart leaves an RP5C01 on bank 1: the time is in bank 0 */
        mode = nib(13);
        RTC(13) = mode & 0x0C;
    }
    for (i = 1; i < 16 && nib(i) == nib(0); i++) ;
    if (i == 16) goto none;                     /* all registers alike: nothing there */
    if (nib(0) > 9 || nib(1) > 5 || nib(2) > 9 || nib(3) > 5) goto none;
    s = nib(0);
    for (i = 0; i < 120 && nib(0) == s; i++) wait_lines(160);       /* up to 1.2 s, read only */
    if (nib(0) == s || nib(0) > 9) goto none;

    int msm = date_ok(6, 12), rp = date_ok(7, 6);
    if (rp && !msm) msm_type = 0;
    else if (msm && !rp) msm_type = 1;
    else msm_type = (nib(13) & 8) ? 0 : 1;      /* the RP5C01 runs with its timer bit set in MODE */

    u8 d, h24, wd_unused;
    if (msm_type) {
        RTC(13) = 1;                            /* hold, then wait for busy to clear (within 190 us) */
        for (i = 0; i < 8 && (nib(13) & 2); i++) { RTC(13) = 0; wait_lines(1); RTC(13) = 1; wait_lines(2); }
        h24 = nib(15) & 4; d = 6;
    } else {
        RTC(13) = (nib(13) & 0x0C) | 1; h24 = nib(10) & 1; RTC(13) = (nib(13) & 0x0C);
        d = 7;
    }
    (void)wd_unused;
    u32 sec = two(0), min = two(2), hh = nib(4), h10 = nib(5), pm = 0;
    if (!h24) { pm = (h10 & (msm_type ? 4 : 2)) != 0; h10 &= 1; }
    hh += 10 * (h10 & 3);
    if (!h24) { if (hh == 12) hh = 0; if (pm) hh += 12; }
    u32 day = two(d), mon = two(d + 2), yr = two(d + 4) % 100;
    if (msm_type) RTC(13) = 0;                  /* release the hold */
    if (sec > 59 || min > 59 || hh > 23 || day < 1 || day > 31 || mon < 1 || mon > 12) goto none;
    u32 year = yr >= 78 ? 1900 + yr : 2000 + yr;
    kprintf("clock: %s %u-%s%u-%s%u %s%u:%s%u:%s%u\n", msm_type ? "MSM6242" : "RP5C01", year, mon < 10 ? "0" : "", mon,
            day < 10 ? "0" : "", day, hh < 10 ? "0" : "", hh, min < 10 ? "0" : "", min, sec < 10 ? "0" : "", sec);
    return days_from_civil(year, mon, day) * 86400 + hh * 3600 + min * 60 + sec;
none:
    if (mode) RTC(13) = mode;
    kprintf("clock: none\n");
    return 0;
}
