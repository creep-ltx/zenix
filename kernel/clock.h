/* SPDX-License-Identifier: GPL-2.0-only */
/* The wall clock behind SYS_time and SYS_settime (audit 2026-10-26 F8). The time is the seconds since 1970 (UTC) at
 * boot (`base`, set by the battery clock at boot or by SYS_settime) plus the whole seconds since boot (`secs`), which
 * the 100 Hz tick counts itself: every 100th tick adds one. The old form, base + ticks / 100, jumped back 497 days
 * when the 32-bit tick count wrapped (after 497 days of uptime); `secs` wraps only after 136 years.
 * The time is unsigned (u32): seconds since 1970 up to 2106. SYS_time hands back these 32 bits unchanged, as it always
 * did; a caller reads them as an unsigned number (libz's sys_time is u32), so 2038 is no limit.
 * Plain inline arithmetic, no system calls: tools/clock-host-test.c runs the same code across the wraps. */
#ifndef ZENIX_CLOCK_H
#define ZENIX_CLOCK_H

struct wclock {
    u32 base;                   /* seconds since 1970 at boot (whatever secs counted is taken off when set) */
    volatile u32 secs;          /* whole seconds since boot */
    volatile u8 sub;            /* ticks into the current second, 0-99 */
    u8 trust;                   /* CLOCK_RTC, CLOCK_NET (kernel.h): who set the time this boot; SYS_ticks 2 */
};

#define WCLOCK_HZ 100           /* the tick's rate (main.c's CIA-A timer A) */

/* the tick (interrupt): one hundredth of a second */
static inline void wclock_tick(struct wclock *c)
{
    if (++c->sub >= WCLOCK_HZ) {
        c->sub = 0;
        c->secs++;
    }
}

/* seconds since 1970, unsigned */
static inline u32 wclock_now(const struct wclock *c)
{
    return c->base + c->secs;
}

/* set the time to t (seconds since 1970): call with the tick masked, so secs does not move in between */
static inline void wclock_set(struct wclock *c, u32 t)
{
    c->base = t - c->secs;
}

#endif
