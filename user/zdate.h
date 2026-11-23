/* SPDX-License-Identifier: GPL-2.0-only */
/* The date conversion, one source (docs/audit-2026-11-23-one-system.md OS-35): libz's and the C library's (user/zhelp.c
 * includes it) and the PC tests' (tools/host/zhost.h, with ZDATE_LINK static). struct zdt is libz.h's. */
#ifndef ZDATE_LINK
#define ZDATE_LINK __attribute__((optimize("Os")))
#endif
/* dates: seconds since 1970 (UTC, to 2106) as a calendar date, and back (the civil-from-days formula, one for all) */
ZDATE_LINK void zdate(u32 t, struct zdt *d)
{
    u32 days = t / 86400, rem = t % 86400;
    long z = (long)days + 719468, era = z / 146097;
    u32 doe = (u32)(z - era * 146097);
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u32 mp = (5 * doy + 2) / 153;
    int m = (int)(mp < 10 ? mp + 3 : mp - 9), y = (int)(yoe + era * 400) + (m <= 2);
    d->year = y;
    d->mon = m;
    d->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    d->hour = (int)(rem / 3600);
    d->min = (int)(rem / 60 % 60);
    d->sec = (int)(rem % 60);
    d->wday = (int)((days + 4) % 7);
    d->yday = m <= 2 ? (int)doy - 306 : (int)doy + 59 + ((y % 4 == 0 && y % 100) || y % 400 == 0);  /* doy: from 1 March */
}
ZDATE_LINK long zdays(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    u32 yoe = (u32)(y - era * 400);
    u32 doy = (u32)((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1);
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

