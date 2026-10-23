#include "libz.h"
/* date [+FORMAT]: the time (UTC; Zenix has no time zones yet). Formats: %Y %m %d %H %M %S %s %a %A %b %B %j %e %y %Z %% %F %T %D %n */
static const char *const wd[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mn[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static u32 parse_time(const char *s)
{
    int y, mo, d, h = 0, mi = 0, sec = 0;
    y = (int)strtol(s, (char **)&s, 10); if (*s++ != '-') return 0;
    mo = (int)strtol(s, (char **)&s, 10); if (*s++ != '-') return 0;
    d = (int)strtol(s, (char **)&s, 10);
    if (*s == ' ' || *s == 'T') { s++; h = (int)strtol(s, (char **)&s, 10); if (*s == ':') { s++; mi = (int)strtol(s, (char **)&s, 10); if (*s == ':') { s++; sec = (int)strtol(s, (char **)&s, 10); } } }
    static const int ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    u32 days = 0;
    for (int yy = 1970; yy < y; yy++) days += leap(yy) ? 366 : 365;
    for (int mm = 0; mm < mo - 1; mm++) days += (u32)(ml[mm] + (mm == 1 && leap(y)));
    days += (u32)(d - 1);
    return days * 86400 + (u32)(h * 3600 + mi * 60 + sec);
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "-s")) {
        u32 t = parse_time(argv[2]);
        if (!t) { fprintf(2, "date: invalid date '%s' (use YYYY-MM-DD HH:MM:SS)\n", argv[2]); return 1; }
        if (sys_settime(t) < 0) { fprintf(2, "date: cannot set the clock: %s\n", zwhy("refused")); return 1; }   /* root only */
    }
    u32 t = (u32)sys_time();
    u32 days = t / 86400, rem = t % 86400;
    int hh = (int)(rem / 3600), mm = (int)(rem / 60 % 60), ss = (int)(rem % 60), dow = (int)((days + 4) % 7), y = 1970;
    for (;;) { u32 yl = leap(y) ? 366 : 365; if (days < yl) break; days -= yl; y++; }
    static const int ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int doy = (int)days + 1, m = 0;
    for (; m < 11; m++) { int l = ml[m] + (m == 1 && leap(y)); if ((int)days < l) break; days -= (u32)l; }
    int d = (int)days + 1;
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
