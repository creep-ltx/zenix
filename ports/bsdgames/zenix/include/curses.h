/* SPDX-License-Identifier: GPL-2.0-only */
/* A small curses for the BSD games on Zenix (ports/bsdgames/README, ports/bsdgames/zenix/curses.c): the calls the
 * games use and little more, over the console's xterm-style escapes (TERM=xterm-16color: cursor moves, erase in
 * line, SGR bold, underline and inverse, the cursor shown or hidden). Windows are character grids; a refresh compares
 * one with what the screen shows (curscr) and writes only the cells that differ. No colour, no wide characters, no
 * pads, no soft keys. */
#ifndef ZGAMES_CURSES_H
#define ZGAMES_CURSES_H
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

typedef unsigned short chtype;          /* a character (low 8 bits, Latin-1) and its attributes */
typedef chtype attr_t;
#define A_CHARTEXT      0x00FFu
#define A_ATTRIBUTES    0xFF00u
#define A_NORMAL        0x0000u
#define A_STANDOUT      0x0100u
#define A_REVERSE       0x0100u
#define A_BOLD          0x0200u
#define A_UNDERLINE     0x0400u
#define A_DIM           0x0000u
#define A_BLINK         0x0000u

#define ERR             (-1)
#define OK              0
#ifndef TRUE
#define TRUE            1
#define FALSE           0
#endif

#define KEY_DOWN        0402
#define KEY_UP          0403
#define KEY_LEFT        0404
#define KEY_RIGHT       0405
#define KEY_HOME        0406
#define KEY_BACKSPACE   0407
#define KEY_DC          0512
#define KEY_END         0550
#define KEY_ENTER       0527

typedef struct _win_st {
    short begy, begx, maxy, maxx;       /* place on the screen; size in lines and columns */
    short cury, curx;
    chtype attrs;                       /* added to every character written */
    unsigned char leave, scroll, clear, keypad, nodelay, touched;
    int delay;                          /* wtimeout: ms, -1 wait */
    chtype *cells;                      /* maxy x maxx */
} WINDOW;

extern WINDOW *stdscr, *curscr;
extern int LINES, COLS;

WINDOW *initscr(void);
int endwin(void);
bool isendwin(void);
WINDOW *newwin(int lines, int cols, int y, int x);
int delwin(WINDOW *w);

int wmove(WINDOW *w, int y, int x);
int waddch(WINDOW *w, chtype c);
int waddstr(WINDOW *w, const char *s);
int waddnstr(WINDOW *w, const char *s, int n);
int wprintw(WINDOW *w, const char *fmt, ...) __attribute__((__format__(__printf__, 2, 3)));
int vw_printw(WINDOW *w, const char *fmt, va_list ap);
int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...) __attribute__((__format__(__printf__, 4, 5)));
int printw(const char *fmt, ...) __attribute__((__format__(__printf__, 1, 2)));
int mvprintw(int y, int x, const char *fmt, ...) __attribute__((__format__(__printf__, 3, 4)));
int wclear(WINDOW *w);
int werase(WINDOW *w);
int wclrtoeol(WINDOW *w);
int wclrtobot(WINDOW *w);
int wdeleteln(WINDOW *w);
int winsertln(WINDOW *w);
chtype winch(WINDOW *w);
int wrefresh(WINDOW *w);
int wnoutrefresh(WINDOW *w);
int doupdate(void);
int touchwin(WINDOW *w);
int box(WINDOW *w, chtype v, chtype h);
int wgetch(WINDOW *w);
int wstandout(WINDOW *w);
int wstandend(WINDOW *w);
int wattron(WINDOW *w, int a);
int wattroff(WINDOW *w, int a);
int wattrset(WINDOW *w, int a);

int leaveok(WINDOW *w, bool b);
int scrollok(WINDOW *w, bool b);
int clearok(WINDOW *w, bool b);
int idlok(WINDOW *w, bool b);
int keypad(WINDOW *w, bool b);
int nodelay(WINDOW *w, bool b);
void wtimeout(WINDOW *w, int ms);

int cbreak(void);
int nocbreak(void);
int raw(void);
int noraw(void);
int echo(void);
int noecho(void);
int nl(void);
int nonl(void);
int curs_set(int v);
int beep(void);
int flash(void);
int baudrate(void);
int mvcur(int oy, int ox, int ny, int nx);
int flushinp(void);
int napms(int ms);
int ungetch(int c);
int has_colors(void);
const char *unctrl(chtype c);

#define getyx(w, y, x)          ((y) = (w)->cury, (x) = (w)->curx)
#define getmaxyx(w, y, x)       ((y) = (w)->maxy, (x) = (w)->maxx)
#define getbegyx(w, y, x)       ((y) = (w)->begy, (x) = (w)->begx)

#define move(y, x)              wmove(stdscr, y, x)
#define addch(c)                waddch(stdscr, c)
#define addstr(s)               waddstr(stdscr, s)
#define addnstr(s, n)           waddnstr(stdscr, s, n)
#define clear()                 wclear(stdscr)
#define erase()                 werase(stdscr)
#define clrtoeol()              wclrtoeol(stdscr)
#define clrtobot()              wclrtobot(stdscr)
#define deleteln()              wdeleteln(stdscr)
#define insertln()              winsertln(stdscr)
#define inch()                  winch(stdscr)
#define refresh()               wrefresh(stdscr)
#define getch()                 wgetch(stdscr)
#define standout()              wstandout(stdscr)
#define standend()              wstandend(stdscr)
#define attron(a)               wattron(stdscr, a)
#define attroff(a)              wattroff(stdscr, a)
#define attrset(a)              wattrset(stdscr, a)
#define timeout(ms)             wtimeout(stdscr, ms)
#define mvwaddch(w, y, x, c)    (wmove(w, y, x) == ERR ? ERR : waddch(w, c))
#define mvwaddstr(w, y, x, s)   (wmove(w, y, x) == ERR ? ERR : waddstr(w, s))
#define mvwinch(w, y, x)        (wmove(w, y, x) == ERR ? (chtype)ERR : winch(w))
#define mvwgetch(w, y, x)       (wmove(w, y, x) == ERR ? ERR : wgetch(w))
#define mvaddch(y, x, c)        mvwaddch(stdscr, y, x, c)
#define mvaddstr(y, x, s)       mvwaddstr(stdscr, y, x, s)
#define mvinch(y, x)            mvwinch(stdscr, y, x)
#define mvgetch(y, x)           mvwgetch(stdscr, y, x)
#endif
