// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* cal [[month] year]: a calendar of a month (default: this one), weeks starting on Sunday */
static int days_in(int y, int m) { static const int d[] = { 31,28,31,30,31,30,31,31,30,31,30,31 }; return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : d[m - 1]; }
static int weekday(int y, int m, int d)               /* 0 = Sunday */
{
    static const int t[] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static void civil(u32 secs, int *y, int *m, int *d)
{
    u32 z = secs / 86400 + 719468, era = z / 146097, doe = z - era * 146097;
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = (int)(yoe + era * 400 + (*m <= 2));
}
int main(int argc, char **argv)
{
    int ty, tm, td; civil(sys_time(), &ty, &tm, &td);
    int y = ty, m = tm, hl = sys_isatty(1);
    if (argc == 2) y = atoi(argv[1]), m = 0;
    else if (argc >= 3) { m = atoi(argv[1]); y = atoi(argv[2]); }
    if (y < 1 || y > 9999 || m < 0 || m > 12) { fprintf(2, "cal: bad month or year\n"); return 2; }
    static const char *names[] = { "January","February","March","April","May","June","July","August","September","October","November","December" };
    for (int mo = m ? m : 1; mo <= (m ? m : 12); mo++) {
        char head[40]; snprintf(head, sizeof head, "%s %d", names[mo - 1], y);
        int pad = (20 - (int)strlen(head)) / 2; for (int k = 0; k < pad; k++) oputc(' ');
        oputs(head); oputc('\n'); oputs("Su Mo Tu We Th Fr Sa\n");
        int wd = weekday(y, mo, 1);
        for (int k = 0; k < wd; k++) oputs("   ");
        for (int d = 1; d <= days_in(y, mo); d++) {
            if (hl && y == ty && mo == tm && d == td) oprintf("\033[7m%2d\033[0m", d); else oprintf("%2d", d);
            wd++; if (wd == 7) { oputc('\n'); wd = 0; } else if (d < days_in(y, mo)) oputc(' ');
        }
        if (wd) oputc('\n');
        if (!m && mo < 12) oputc('\n');
    }
    return 0;
}
