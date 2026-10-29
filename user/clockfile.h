/* SPDX-License-Identifier: GPL-2.0-only */
/* /var/lib/clock, the certificate time's floor (DESIGN D21 8.3; the format and its parser are crypto/zc/zc_clock.c):
 * read and written by sntp and reboot, one copy of the file handling. */
#ifndef ZENIX_CLOCKFILE_H
#define ZENIX_CLOCKFILE_H
#include <stdint.h>

/* the file's time into *t: 0, or -1 (no file, or not the format: ignored) */
int clockfile_read(uint32_t *t);
/* /var/lib/clock := t (written beside, then renamed: never half a line; mode 644); 0 or a negative error */
int clockfile_write(uint32_t t);

/* does reboot raise the floor to `now`? rtc/net: the battery clock / sntp set the time this boot (SYS_ticks 2);
 * have: the file was read, as `file`. sntp's checked answer always may (it judged it); the battery clock alone only
 * within 400 days of the floor (sntp's SNTP_AHEAD), so one wrong battery clock (2061) cannot raise the floor past every
 * real time, refusing sntp and every certificate after (audit 2026-10-29 B2-2). The floor never falls. */
#define CLOCKFILE_RTC_AHEAD (400u * 86400u)
static inline int clockfile_rises(int rtc, int net, int have, uint32_t file, uint32_t now)
{
    if (have && now <= file)
        return 0;
    if (net)
        return 1;
    return rtc && (!have || now - file <= CLOCKFILE_RTC_AHEAD);
}

#endif
