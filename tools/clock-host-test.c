/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the wall clock (kernel/clock.h, audit 2026-10-26 F8): the same inline code the kernel's tick and
 * SYS_time/SYS_settime run. The tick count (a 32-bit number, 100 a second) wraps after 497 days; the time must go on
 * across that wrap without a jump, and times past 2038 (above 2^31) must come back as the unsigned number they are.
 * The old form, base + ticks / 100, is run beside it as the reference that fails. Run by tools/clock-host-test.sh. */
#include <stdio.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
#include "../kernel/clock.h"

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

/* a clock that has run `t` ticks since boot, as the kernel's tick leaves it */
static void run_to(struct wclock *c, volatile u32 *ticks, u32 t)
{
    c->secs = t / WCLOCK_HZ;            /* where the tick's own count stands after t ticks */
    c->sub = (u8)(t % WCLOCK_HZ);
    *ticks = t;
}

int main(void)
{
    struct wclock c = { 0, 0, 0 };
    volatile u32 ticks = 0;

    /* 1. counting from boot: 100 ticks make a second */
    for (int i = 0; i < 250; i++) { ticks++; wclock_tick(&c); }
    check(c.secs == 2 && c.sub == 50, "250 ticks: 2 seconds and 50 ticks");

    /* 2. across the tick count's wrap (497 days of uptime): no jump, one second per 100 ticks */
    const u32 set = 1800000000u;        /* 2027 */
    run_to(&c, &ticks, 0xFFFFFFFFu - 1000);
    wclock_set(&c, set);
    u32 old_base = set - ticks / 100;   /* the old SYS_settime */
    u32 prev = wclock_now(&c), jumps = 0, old_back = 0, old_prev = old_base + ticks / 100;
    for (int i = 0; i < 5000; i++) {
        ticks++;
        wclock_tick(&c);
        u32 now = wclock_now(&c), old = old_base + ticks / 100;
        if (now != prev && now != prev + 1)
            jumps++;
        if ((s32)(old - old_prev) < 0)
            old_back++;
        prev = now;
        old_prev = old;
    }
    check(ticks < 5000, "the tick count wrapped during the run");
    check(jumps == 0, "the time never jumps across the wrap");
    check(wclock_now(&c) == set + 50, "5,000 ticks after setting it the time is 50 s later (the seconds count on)");
    check(old_back == 1, "the old form (base + ticks / 100) jumps back once there (the bug this fixes)");
    check((u32)(old_prev - old_base) < 100, "... by 497 days");

    /* 3. after 2038: a time above 2^31 comes back unsigned, and setting it is exact */
    run_to(&c, &ticks, 12345);
    wclock_set(&c, 2208988800u);        /* 2040-01-01 */
    check(wclock_now(&c) == 2208988800u, "2040-01-01 (above 2^31) set and read back unchanged");
    s32 r = (s32)wclock_now(&c);        /* what SYS_time puts in d0 */
    check((u32)r == 2208988800u && (u32)r > 0x7FFFFFFFu, "SYS_time's 32 bits read unsigned are 2040");
    for (int i = 0; i < 100; i++) wclock_tick(&c);
    check(wclock_now(&c) == 2208988801u, "and it goes on from there");

    /* 4. the seconds count itself wraps only after 136 years */
    c.secs = 0xFFFFFFFFu; c.sub = 99; c.base = 5;
    wclock_tick(&c);
    check(c.secs == 0 && wclock_now(&c) == 5, "the seconds count wraps after 2^32 s (136 years), modulo arithmetic");

    printf("%s\n", fails ? "clock-host-test: FAILED" : "clock-host-test: ok");
    return fails != 0;
}
