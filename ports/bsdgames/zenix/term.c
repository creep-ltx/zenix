/* SPDX-License-Identifier: GPL-2.0-only */
/* The terminfo calls tetris makes (include/term.h): the console's capabilities (TERM=xterm-16color, the escapes
 * kernel/term.c reads), built in. tiparm knows the two parameterised ones by their address: cursor_address (row,
 * column, from 0) and set_a_foreground (an ANSI colour 0-7); any other string comes back unchanged. No padding:
 * tputs writes the string. The size comes from the terminal (TIOCGWINSZ), else LINES and COLUMNS, else 24 x 80. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include "term.h"

const char *cursor_address = "\033[%i%p1%d;%p2%dH", *cursor_up = "\033[A", *cursor_home = "\033[H",
           *cursor_to_ll = 0, *clear_screen = "\033[H\033[2J", *clr_eol = "\033[K",
           *enter_standout_mode = "\033[7m", *exit_standout_mode = "\033[0m", *enter_ca_mode = 0,
           *exit_ca_mode = 0, *cursor_invisible = "\033[?25l", *cursor_normal = "\033[?25h",
           *set_a_foreground = "\033[3%p1%dm";
int lines = 24, columns = 80, move_standout_mode = 1;     /* msgr: moving in standout mode is safe */
short ospeed;

int setupterm(const char *term, int fd, int *errret)
{
    (void)term;
    struct winsize ws;
    if (ioctl(fd > 0 ? fd : 1, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        lines = ws.ws_row;
        columns = ws.ws_col;
    } else {
        const char *e;
        if ((e = getenv("LINES")) && atoi(e) > 0) lines = atoi(e);
        if ((e = getenv("COLUMNS")) && atoi(e) > 0) columns = atoi(e);
    }
    if (errret)
        *errret = 1;
    return 0;
}

char *tiparm(const char *cap, ...)
{
    static char b[24];
    va_list ap;
    va_start(ap, cap);
    int a = va_arg(ap, int), c = cap == cursor_address ? va_arg(ap, int) : 0;
    va_end(ap);
    if (cap == cursor_address)
        snprintf(b, sizeof b, "\033[%d;%dH", a + 1, c + 1);
    else if (cap == set_a_foreground)
        snprintf(b, sizeof b, "\033[3%dm", a & 7);
    else
        return (char *)cap;
    return b;
}

int tputs(const char *s, int affcnt, int (*out)(int))
{
    (void)affcnt;
    if (!s)
        return -1;
    while (*s)
        out((unsigned char)*s++);
    return 0;
}
