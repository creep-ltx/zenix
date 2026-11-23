/* SPDX-License-Identifier: GPL-2.0-only */
/* The screen of a text program, one layer for every one (docs/audit-2026-11-23-one-system.md OS-32, OS-33; section 3
 * "Screen"): the buffered output (oputn, oflush: zout.c), the terminal's size read from the system at the start and
 * again after SIGWINCH (never a fixed 80 x 25), the cursor placed, a text cut to whole characters in so many columns,
 * and a line in reverse video (the status line at the bottom, a title at the top). One source for libz and the C
 * library (user/zshare.h): irc, mail, modplay, less and edit, the games' curses. */
#include "zshare.h"
#ifdef ZENIX_LIBC
#include <signal.h>
#endif

int zscr_rows = 25, zscr_cols = 80;
static volatile int zscr_winch;

int zscr_size(void)
{
    int sz = sys_ttyinfo(), r = sz > 0 ? sz >> 16 : 0, c = sz > 0 ? sz & 0xFFFF : 0;
    if (r < 2 || r > 255) r = 25;
    if (c < 10 || c > 255) c = 80;
    int changed = r != zscr_rows || c != zscr_cols;
    zscr_rows = r;
    zscr_cols = c;
    return changed;
}
static void zscr_onwinch(int sig) { (void)sig; zscr_winch = 1; }
void zscr_init(void)
{
    zscr_size();
    signal(SIGWINCH, zscr_onwinch);
}
int zscr_resized(void)
{
    if (!zscr_winch) return 0;
    zscr_winch = 0;
    zscr_size();
    return 1;
}

static void zscr_num(u32 v, char *e)
{
    char d[10];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) *e++ = d[--k];
    *e = 0;
}
int zscr_move(char *b, int row, int col)
{
    b[0] = '\033';
    b[1] = '[';
    zscr_num((u32)(row > 0 ? row : 1), b + 2);
    u32 n = strlen(b);
    b[n++] = ';';
    zscr_num((u32)(col > 0 ? col : 1), b + n);
    n = strlen(b);
    b[n++] = 'H';
    b[n] = 0;
    return (int)n;
}
void zscr_at(int row, int col)
{
    char b[24];
    oputn(b, (u32)zscr_move(b, row, col));
}
#ifndef ZENIX_LIBC                     /* (the UTF-8 widths are libz's: the C library's programs, the games, need none) */
void zscr_putw(const char *s, int w)
{
    u32 n = (u32)strlen(s), k = zu8fit(s, n, w);
    oputn(s, k);
    for (int c = zu8cols(s, k); c < w; c++) oputn(" ", 1);
}
void zscr_bar(int row, const char *s)
{
    zscr_at(row, 1);
    oputn("\033[7m", 4);
    zscr_putw(s, zscr_cols - 1);        /* not the last column: a write there may scroll the screen */
    oputn("\033[m", 3);
}
void zscr_status(const char *s) { zscr_bar(zscr_rows, s); }
#endif
