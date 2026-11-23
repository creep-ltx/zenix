/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* date [+FORMAT]: the time (UTC; Zenix has no time zones yet). Formats: %Y %m %d %H %M %S %s %a %A %b %B %j %e %y %Z %% %F %T %D %n */
static const char *const wd[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mn[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static u32 parse_time(const char *s)
{
    int y, mo, d, h = 0, mi = 0, sec = 0;
    y = (int)strtol(s, (char **)&s, 10); if (*s++ != '-') return 0;
    mo = (int)strtol(s, (char **)&s, 10); if (*s++ != '-') return 0;
    d = (int)strtol(s, (char **)&s, 10);
    if (*s == ' ' || *s == 'T') { s++; h = (int)strtol(s, (char **)&s, 10); if (*s == ':') { s++; mi = (int)strtol(s, (char **)&s, 10); if (*s == ':') { s++; sec = (int)strtol(s, (char **)&s, 10); } } }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    return (u32)zdays(y, mo, d) * 86400 + (u32)(h * 3600 + mi * 60 + sec);
}

static const char USAGE[] = "usage: date [+format]\n       date -s \"YYYY-MM-DD HH:MM:SS\"\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    if (argc > 1 && argv[1][0] == '-' && (strcmp(argv[1], "-s") || argc < 3)) zbadopt(argv[1], USAGE);
    if (argc > 1 && argv[1][0] != '+' && argv[1][0] != '-') zusage(USAGE);
    if (argc > 2 && !strcmp(argv[1], "-s")) {
        u32 t = parse_time(argv[2]);
        if (!t) { dprintf(2, "date: invalid date '%s' (use YYYY-MM-DD HH:MM:SS)\n", argv[2]); return 1; }
        if (sys_settime(t) < 0) { dprintf(2, "date: cannot set the clock: %s\n", zwhy("refused")); return 1; }   /* root only */
    }
    u32 t = (u32)sys_time();
    struct zdt z;
    zdate(t, &z);
    int hh = z.hour, mm = z.min, ss = z.sec, dow = z.wday, y = z.year, doy = z.yday + 1, m = z.mon - 1, d = z.day;
    const char *f = argc > 1 && argv[1][0] == '+' ? argv[1] + 1 : "%a %b %e %H:%M:%S UTC %Y";
    for (; *f; f++) {
        if (*f != '%') { oputc(*f); continue; }
        f++;
        switch (*f) {
        case 'Y': oprintf("%d", y); break; case 'y': oprintf("%02d", y % 100); break; case 'm': oprintf("%02d", m + 1); break;
        case 'd': oprintf("%02d", d); break; case 'e': oprintf("%2d", d); break; case 'H': oprintf("%02d", hh); break;
        case 'M': oprintf("%02d", mm); break; case 'S': oprintf("%02d", ss); break; case 's': oprintf("%u", t); break;
        case 'a': oputn(wd[dow], 3); break; case 'A': oputs(wd[dow]); break;
        case 'b': case 'h': oputn(mn[m], 3); break; case 'B': oputs(mn[m]); break;
        case 'j': oprintf("%03d", doy); break; case 'Z': oputs("UTC"); break; case 'n': oputc('\n'); break;
        case 'F': oprintf("%d-%02d-%02d", y, m + 1, d); break; case 'T': oprintf("%02d:%02d:%02d", hh, mm, ss); break;
        case 'D': oprintf("%02d/%02d/%02d", m + 1, d, y % 100); break; case '%': oputc('%'); break;
        default: oputc('%'); oputc(*f);
        }
    }
    oputc('\n');
    return 0;
}
