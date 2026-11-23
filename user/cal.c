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
static const char USAGE[] = "usage: cal [[month] year]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    if (argc > 1 && argv[1][0] == '-') zbadopt(argv[1], USAGE);
    struct zdt z; zdate(sys_time(), &z); int ty = z.year, tm = z.mon, td = z.day;
    int y = ty, m = tm, hl = sys_isatty(1);
    if (argc == 2) y = atoi(argv[1]), m = 0;
    else if (argc >= 3) { m = atoi(argv[1]); y = atoi(argv[2]); }
    if (y < 1 || y > 9999 || m < 0 || m > 12) { dprintf(2, "cal: bad month or year\n"); return 2; }
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
