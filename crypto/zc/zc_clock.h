/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The time certificates are judged by (DESIGN D21 8.3), without system calls: the file /var/lib/clock (frozen once
 * shipped: one line "zenix-clock 1 SECONDS", unsigned decimal Unix seconds), the floor (the newer of the library's
 * build date and that file: the time used for certificates never goes below it), and BearSSL's time callback for
 * the X.509 wrapper (zc_x509_set_time_callback): with a trusted clock (the battery clock, or an accepted SNTP answer
 * this boot) both dates are checked against the clock, never below the floor; without one, "not after" is checked
 * against the floor and "not before" is skipped. Unix seconds are unsigned 32-bit (good until 2106); the conversion
 * to BearSSL's days uses 32-bit arithmetic only (no 64-bit division: D21 3.5, 8.3).
 */
#ifndef ZC_CLOCK_H
#define ZC_CLOCK_H

#include <stddef.h>
#include <stdint.h>

#define ZC_CLOCK_PATH  "/var/lib/clock"
#define ZC_CLOCK_LINE  32               /* room for the file's line and its NUL */
#define ZC_UNIX_DAY0   719528u          /* BearSSL's day number of 1970-01-01 (days since 1 January of year 0) */

/* The file's seconds from its bytes (as read): 0 and *secs set, or -1 for anything but "zenix-clock 1 N" with an
   optional newline (another format number, junk, a number over 4294967295): the caller then ignores the file. */
int zc_clock_parse(const char *buf, size_t n, uint32_t *secs);
/* The line for secs into buf (ZC_CLOCK_LINE bytes), with its newline; returns its length. */
int zc_clock_line(char *buf, uint32_t secs);
/* The floor: the newer of the build date and the file's time (has_file 0: the build date alone). */
uint32_t zc_clock_floor(uint32_t build, int has_file, uint32_t file);

/* Unix seconds as BearSSL's days and seconds in the day. */
void zc_clock_days(uint32_t unix_secs, uint32_t *days, uint32_t *secs);

/* The callback's context. now: SYS_time (unsigned); trusted: the battery clock ticks or an SNTP answer was accepted
   this boot (the program knows which); floor: zc_clock_floor's. used_floor is set when the floor decided a date (the
   program then says once "the clock is not set: certificate dates checked only against <date>"). */
struct zc_clock {
    uint32_t now, floor;
    int trusted;
    int used_floor;
};
/* br_x509_time_check: -1 not yet valid, 0 valid, 1 expired (BearSSL's convention). */
int zc_clock_check(void *ctx, uint32_t nbd, uint32_t nbs, uint32_t nad, uint32_t nas);

#endif
