/* SPDX-License-Identifier: GPL-2.0-only */
/* A small curses for the BSD games (include/curses.h has what it is). The screen is the terminal on standard output,
 * driven by the xterm-style escapes Zenix's console, term and telnet clients understand: CSI row;col H, CSI K, CSI m
 * (0, 1, 4, 7), CSI ?25 l/h, through the system's one screen layer (user/zscr.c: the buffered output, the size,
 * the cursor placed; docs/audit-2026-11-23-one-system.md OS-33). curscr holds what the terminal shows; wrefresh writes the cells of a window that differ
 * from it (all of them after touchwin or clearok), in runs, moving the cursor only where a run starts. Input is read
 * from standard input a byte at a time (poll for the timeouts); keypad() turns the arrows' escape sequences into
 * KEY_UP and the rest. The terminal's modes are termios's (cbreak and noecho take the console out of its line mode;
 * endwin puts back what initscr found). */
#include <curses.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zenix.h>

WINDOW *stdscr, *curscr;
int LINES, COLS;

static struct termios saved, now;
static int have_saved, ended = 1, nlmode = 1, echoing = 1, cursor_vis = 1;
static int py = -1, px = -1;            /* the terminal's cursor (-1: not known) */
static chtype pattr;                    /* the attributes the terminal draws with */
static int pushed = -1;                 /* ungetch */

/* ---- output: one buffer, written when full and at the end of a refresh (its own, not zenix.h's oputn: that one
 * links the error texts, 1.3 KB more in every game) */
static char obuf[1024];
static int on;
static void cflush(void)
{
    int k = 0;
    while (k < on) {
        int r = (int)write(1, obuf + k, (size_t)(on - k));
        if (r <= 0)
            break;
        k += r;
    }
    on = 0;
}
static void oput(const char *s, int n)
{
    while (n > 0) {
        if (on == (int)sizeof obuf)
            cflush();
        int k = (int)sizeof obuf - on < n ? (int)sizeof obuf - on : n;
        memcpy(obuf + on, s, (size_t)k);
        on += k;
        s += k;
        n -= k;
    }
}
static void ostr(const char *s) { oput(s, (int)strlen(s)); }
static void omove(int y, int x)
{
    if (y == py && x == px)
        return;
    char b[24];
    oput(b, zscr_move(b, y + 1, x + 1));       /* the screen layer's escape */
    py = y;
    px = x;
}
static void oattr(chtype a)
{
    a &= A_ATTRIBUTES;
    if (a == pattr)
        return;
    ostr("\033[0");
    if (a & A_BOLD) ostr(";1");
    if (a & A_UNDERLINE) ostr(";4");
    if (a & A_STANDOUT) ostr(";7");
    oput("m", 1);
    pattr = a;
}
static void ochar(chtype c)             /* Latin-1 to UTF-8 (the console's) */
{
    unsigned char b = (unsigned char)(c & A_CHARTEXT);
    if (b < 32 || b == 127)
        b = ' ';
    if (b < 0x80)
        oput((char *)&b, 1);
    else {
        char u[2] = { (char)(0xC0 | b >> 6), (char)(0x80 | (b & 0x3F)) };
        oput(u, 2);
    }
    px++;
    if (px >= COLS) {                   /* the last column: where the cursor is now is not known (deferred wrap) */
        py = -1;
        px = -1;
    }
}

/* ---- windows */
static void blank(chtype *p, int n, chtype a) { while (n-- > 0) *p++ = (chtype)(' ' | (a & A_ATTRIBUTES)); }

WINDOW *newwin(int lines, int cols, int y, int x)
{
    if (lines <= 0) lines = LINES - y;
    if (cols <= 0) cols = COLS - x;
    if (lines <= 0 || cols <= 0 || y < 0 || x < 0 || y + lines > LINES || x + cols > COLS)
        return NULL;
    WINDOW *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->cells = malloc((size_t)lines * (size_t)cols * sizeof(chtype));
    if (!w->cells) {
        free(w);
        return NULL;
    }
    w->begy = (short)y; w->begx = (short)x; w->maxy = (short)lines; w->maxx = (short)cols;
    w->delay = -1;
    w->touched = 1;
    blank(w->cells, lines * cols, 0);
    return w;
}
int delwin(WINDOW *w)
{
    if (!w || w == stdscr || w == curscr)
        return ERR;
    free(w->cells);
    free(w);
    return OK;
}

static void getsize(void)
{
    LINES = COLS = 0;
    if (isatty(1) || isatty(0)) {       /* the terminal's size: the screen layer's (zscr_size) */
        zscr_size();
        LINES = zscr_rows;
        COLS = zscr_cols;
    }
    const char *e;
    if (LINES <= 0 && (e = getenv("LINES"))) LINES = atoi(e);
    if (COLS <= 0 && (e = getenv("COLUMNS"))) COLS = atoi(e);
    if (LINES <= 0 || LINES > 200) LINES = 24;
    if (COLS <= 0 || COLS > 250) COLS = 80;
}

WINDOW *initscr(void)
{
    if (!have_saved && tcgetattr(0, &saved) == 0)
        have_saved = 1;
    now = saved;
    getsize();
    if (!stdscr) {
        stdscr = newwin(LINES, COLS, 0, 0);
        curscr = newwin(LINES, COLS, 0, 0);
        if (!stdscr || !curscr) {
            fprintf(stderr, "curses: not enough memory for a %d x %d screen\n", COLS, LINES);
            exit(1);
        }
    }
    ended = 0;
    curscr->clear = 1;                  /* the first refresh clears the terminal */
    return stdscr;
}
int endwin(void)
{
    if (ended)
        return OK;
    oattr(0);
    omove(LINES - 1, 0);
    if (!cursor_vis)
        ostr("\033[?25h");
    ostr("\r\n");
    cflush();
    py = px = -1;
    if (have_saved)
        tcsetattr(0, TCSANOW, &saved);
    ended = 1;
    curscr->clear = 1;                  /* a refresh after endwin draws the whole screen again */
    return OK;
}
bool isendwin(void) { return ended; }

static int setmode(void) { ended = 0; return have_saved && tcsetattr(0, TCSANOW, &now) == 0 ? OK : ERR; }
int cbreak(void) { now.c_lflag &= ~(tcflag_t)ICANON; now.c_lflag |= ISIG; return setmode(); }
int nocbreak(void) { now.c_lflag |= ICANON; return setmode(); }
int raw(void) { now.c_lflag &= ~(tcflag_t)(ICANON | ISIG); return setmode(); }
int noraw(void) { now.c_lflag |= ICANON | ISIG; return setmode(); }
int echo(void) { echoing = 1; return OK; }             /* the echo is curses's own (wgetch), never the terminal's */
int noecho(void) { echoing = 0; now.c_lflag &= ~(tcflag_t)ECHO; return setmode(); }
int nl(void) { nlmode = 1; return OK; }
int nonl(void) { nlmode = 0; return OK; }

int wmove(WINDOW *w, int y, int x)
{
    if (!w || y < 0 || x < 0 || y >= w->maxy || x >= w->maxx)
        return ERR;
    w->cury = (short)y;
    w->curx = (short)x;
    return OK;
}
static void scrollup(WINDOW *w)
{
    memmove(w->cells, w->cells + w->maxx, (size_t)(w->maxy - 1) * (size_t)w->maxx * sizeof(chtype));
    blank(w->cells + (w->maxy - 1) * w->maxx, w->maxx, w->attrs);
}
static void newline(WINDOW *w)
{
    w->curx = 0;
    if (w->cury + 1 < w->maxy)
        w->cury++;
    else if (w->scroll)
        scrollup(w);
}
int waddch(WINDOW *w, chtype c)
{
    if (!w)
        return ERR;
    unsigned char ch = (unsigned char)(c & A_CHARTEXT);
    switch (ch) {
    case '\n':
        wclrtoeol(w);
        newline(w);
        return OK;
    case '\r':
        w->curx = 0;
        return OK;
    case '\b':
        if (w->curx > 0) w->curx--;
        return OK;
    case '\t': {
        int n = 8 - (w->curx & 7);
        while (n-- > 0)
            if (waddch(w, ' ' | (c & A_ATTRIBUTES)) == ERR)
                return ERR;
        return OK;
    }
    }
    w->cells[w->cury * w->maxx + w->curx] = (chtype)(c | w->attrs);
    if (++w->curx >= w->maxx) {
        if (w->cury + 1 >= w->maxy && !w->scroll) {
            w->curx = (short)(w->maxx - 1);     /* the bottom right: curses's ERR, the character kept */
            return ERR;
        }
        newline(w);
    }
    return OK;
}
int waddnstr(WINDOW *w, const char *s, int n)
{
    for (; *s && n != 0; s++, n--)
        if (waddch(w, (unsigned char)*s) == ERR)
            return ERR;
    return OK;
}
int waddstr(WINDOW *w, const char *s) { return waddnstr(w, s, -1); }
int vw_printw(WINDOW *w, const char *fmt, va_list ap)
{
    char b[512];
    vsnprintf(b, sizeof b, fmt, ap);
    return waddstr(w, b);
}
int wprintw(WINDOW *w, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); int r = vw_printw(w, fmt, ap); va_end(ap); return r;
}
int printw(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); int r = vw_printw(stdscr, fmt, ap); va_end(ap); return r;
}
int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...)
{
    if (wmove(w, y, x) == ERR)
        return ERR;
    va_list ap; va_start(ap, fmt); int r = vw_printw(w, fmt, ap); va_end(ap); return r;
}
int mvprintw(int y, int x, const char *fmt, ...)
{
    if (wmove(stdscr, y, x) == ERR)
        return ERR;
    va_list ap; va_start(ap, fmt); int r = vw_printw(stdscr, fmt, ap); va_end(ap); return r;
}
int werase(WINDOW *w)
{
    blank(w->cells, w->maxy * w->maxx, w->attrs);
    w->cury = w->curx = 0;
    return OK;
}
int wclear(WINDOW *w)
{
    werase(w);
    w->clear = 1;
    return OK;
}
int wclrtoeol(WINDOW *w)
{
    blank(w->cells + w->cury * w->maxx + w->curx, w->maxx - w->curx, w->attrs);
    return OK;
}
int wclrtobot(WINDOW *w)
{
    blank(w->cells + w->cury * w->maxx + w->curx, (w->maxy - w->cury) * w->maxx - w->curx, w->attrs);
    return OK;
}
int wdeleteln(WINDOW *w)
{
    chtype *row = w->cells + w->cury * w->maxx;
    memmove(row, row + w->maxx, (size_t)(w->maxy - w->cury - 1) * (size_t)w->maxx * sizeof(chtype));
    blank(w->cells + (w->maxy - 1) * w->maxx, w->maxx, w->attrs);
    return OK;
}
int winsertln(WINDOW *w)
{
    chtype *row = w->cells + w->cury * w->maxx;
    memmove(row + w->maxx, row, (size_t)(w->maxy - w->cury - 1) * (size_t)w->maxx * sizeof(chtype));
    blank(row, w->maxx, w->attrs);
    return OK;
}
chtype winch(WINDOW *w) { return w->cells[w->cury * w->maxx + w->curx]; }
int touchwin(WINDOW *w) { w->touched = 1; return OK; }
int box(WINDOW *w, chtype v, chtype h)
{
    if (!v) v = '|';
    if (!h) h = '-';
    int y = w->cury, x = w->curx, r = w->maxy - 1, c = w->maxx - 1;
    for (int i = 1; i < c; i++) {
        w->cells[i] = (chtype)(h | w->attrs);
        w->cells[r * w->maxx + i] = (chtype)(h | w->attrs);
    }
    for (int i = 1; i < r; i++) {
        w->cells[i * w->maxx] = (chtype)(v | w->attrs);
        w->cells[i * w->maxx + c] = (chtype)(v | w->attrs);
    }
    w->cells[0] = w->cells[c] = w->cells[r * w->maxx] = w->cells[r * w->maxx + c] = (chtype)('+' | w->attrs);
    w->cury = (short)y;
    w->curx = (short)x;
    return OK;
}
int wstandout(WINDOW *w) { w->attrs |= A_STANDOUT; return OK; }
int wstandend(WINDOW *w) { w->attrs = 0; return OK; }
int wattron(WINDOW *w, int a) { w->attrs |= (chtype)(a & A_ATTRIBUTES); return OK; }
int wattroff(WINDOW *w, int a) { w->attrs &= (chtype)~(a & A_ATTRIBUTES); return OK; }
int wattrset(WINDOW *w, int a) { w->attrs = (chtype)(a & A_ATTRIBUTES); return OK; }
int leaveok(WINDOW *w, bool b) { w->leave = b; return OK; }
int scrollok(WINDOW *w, bool b) { w->scroll = b; return OK; }
int clearok(WINDOW *w, bool b) { w->clear = b; return OK; }
int idlok(WINDOW *w, bool b) { (void)w; (void)b; return OK; }
int keypad(WINDOW *w, bool b) { w->keypad = b; return OK; }
int nodelay(WINDOW *w, bool b) { w->nodelay = b; if (!b) w->delay = -1; return OK; }
void wtimeout(WINDOW *w, int ms) { w->delay = ms < 0 ? -1 : ms; w->nodelay = 0; }

/* ---- refresh: the window's cells onto the terminal where they differ from curscr */
int wnoutrefresh(WINDOW *w)
{
    if (!w || ended)
        return ERR;
    if (w == curscr || curscr->clear || w->clear) {     /* everything again (wrefresh(curscr), clearok) */
        ostr("\033[0m\033[H\033[2J");
        pattr = 0;
        py = px = 0;
        blank(curscr->cells, LINES * COLS, 0);
        curscr->clear = 0;
        if (w != curscr)
            w->clear = 0;
        else {                          /* curscr's own copy is the model: draw stdscr's over it */
            w = stdscr;
            w->touched = 1;
        }
    }
    for (int y = 0; y < w->maxy; y++) {
        int sy = w->begy + y;
        chtype *src = w->cells + y * w->maxx, *dst = curscr->cells + sy * COLS + w->begx;
        for (int x = 0; x < w->maxx; x++) {
            if (!w->touched && src[x] == dst[x])
                continue;
            omove(sy, w->begx + x);
            oattr(src[x]);
            ochar(src[x]);
            dst[x] = src[x];
        }
    }
    w->touched = 0;
    if (!w->leave) {
        omove(w->begy + w->cury, w->begx + w->curx);
    }
    return OK;
}
int doupdate(void)
{
    oattr(0);
    cflush();
    return OK;
}
int wrefresh(WINDOW *w)                 /* SIGALRM held off meanwhile: worm and robots -r refresh from its handler */
{
    sigset_t a, old;
    sigemptyset(&a);
    sigaddset(&a, SIGALRM);
    sigprocmask(SIG_BLOCK, &a, &old);
    int r = wnoutrefresh(w);
    if (r == OK)
        doupdate();
    sigprocmask(SIG_SETMASK, &old, 0);
    return r;
}

int curs_set(int v)
{
    int old = cursor_vis;
    cursor_vis = v != 0;
    ostr(cursor_vis ? "\033[?25h" : "\033[?25l");
    cflush();
    return old;
}
int beep(void) { oput("\a", 1); cflush(); return OK; }
int flash(void) { return beep(); }
int baudrate(void) { return 38400; }
int has_colors(void) { return 0; }
const char *unctrl(chtype c)            /* a character as it is shown: ^X for a control character */
{
    static char b[3];
    unsigned char ch = (unsigned char)(c & A_CHARTEXT);
    if (ch < 32 || ch == 127) {
        b[0] = '^';
        b[1] = (char)(ch == 127 ? '?' : ch + '@');
        b[2] = 0;
    } else {
        b[0] = (char)ch;
        b[1] = 0;
    }
    return b;
}
int mvcur(int oy, int ox, int ny, int nx)
{
    (void)oy; (void)ox;
    if (ny < 0 || nx < 0 || ny >= LINES || nx >= COLS)
        return ERR;
    py = px = -1;
    omove(ny, nx);
    cflush();
    return OK;
}
int napms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, 0);
    return OK;
}

/* ---- input */
static int eof;
static int rd(int ms)                   /* a byte, or ERR when none came in ms (-1: wait); the end of input is one
                                         * Ctrl-D (the games quit on it), then ERR */
{
    if (eof)
        return ERR;
    if (ms >= 0) {
        struct pollfd p = { 0, POLLIN, 0 };
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            int r = poll(&p, 1, ms);
            if (r > 0)
                break;
            if (r == 0 || errno != EINTR)
                return ERR;
            struct timespec t1;         /* a signal (an alarm): the rest of the time */
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ms -= (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
            t0 = t1;
            if (ms < 0)
                return ERR;
        }
    }
    unsigned char c;
    int r;
    while ((r = (int)read(0, &c, 1)) < 0 && errno == EINTR)
        ;
    if (r == 0) {
        eof = 1;
        return 4;
    }
    return r == 1 ? c : ERR;
}
int flushinp(void)
{
    pushed = -1;
    while (rd(0) != ERR)
        ;
    return OK;
}
int ungetch(int c) { pushed = c; return OK; }
int wgetch(WINDOW *w)
{
    if (pushed != -1) {
        int c = pushed;
        pushed = -1;
        return c;
    }
    if (w->touched || w->clear)
        wrefresh(w);
    else
        doupdate();
    int c = rd(w->nodelay ? 0 : w->delay);
    if (c == ERR)
        return ERR;
    if (c == '\r' && nlmode)
        c = '\n';
    if (c == 033 && w->keypad) {        /* CSI or SS3 and a letter: the arrows, Home, End; else the Escape itself */
        int c2 = rd(50);
        if (c2 == '[' || c2 == 'O') {
            int c3 = rd(50), k = ERR;
            switch (c3) {
            case 'A': k = KEY_UP; break;
            case 'B': k = KEY_DOWN; break;
            case 'C': k = KEY_RIGHT; break;
            case 'D': k = KEY_LEFT; break;
            case 'H': k = KEY_HOME; break;
            case 'F': k = KEY_END; break;
            }
            if (k != ERR)
                return k;
            if (c3 >= '1' && c3 <= '6' && c2 == '[') {    /* CSI n ~: Home, Delete, End */
                int c4 = rd(50);
                if (c4 == '~')
                    return c3 == '1' ? KEY_HOME : c3 == '3' ? KEY_DC : c3 == '4' ? KEY_END : wgetch(w);
                c3 = c4;
            }
            while (c3 != ERR && !(c3 >= '@' && c3 <= '~'))      /* an escape we do not know: skipped whole */
                c3 = rd(50);
            return wgetch(w);
        }
        if (c2 != ERR)
            pushed = c2;
    }
    if (echoing && c >= 32 && c < 256) {
        waddch(w, (chtype)c);
        wrefresh(w);
    }
    return c;
}
