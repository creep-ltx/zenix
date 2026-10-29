/* SPDX-License-Identifier: GPL-2.0-only */
/* The text-UI library's test program (roadmap block 3; ports/tui): a test in /tests, not a command. It links
 * libtui.a (the owner's tui.library, its upstream source over ports/tui/zenix) and the C library, opens the console
 * through the old tui_* names (Sumi's, Maki's and Markdown's way) and the Tui* calls, draws its widgets, and checks
 * what the console shows by reading the console's grid back (CON_READ, root: the glyphs, attributes and colours,
 * and the cursor). tools/tui-test.sh types the single keys it waits for after each prompt; the escape sequences it
 * puts into the console's input itself (CON_INPUT, all at once). The results are kept until the
 * terminal is the shell's again, then printed: "ok NAME" or "FAIL NAME: got ...", the events read, and a last line
 * counting them. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <tui.h>
#define ZENIX_LIBC
#include "usys.h"                       /* CON_READ, SYS_ttyinfo, SYS_procinfo */

static int nchk, nfail;
static char *res;                       /* the results, printed after the terminal is given back */
static size_t rlen, rcap;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof b) n = sizeof b - 1;
    if (rlen + (size_t)n + 1 > rcap) {
        size_t c = rcap ? rcap * 2 : 2048;
        char *r = realloc(res, c);
        if (!r) return;
        res = r;
        rcap = c;
    }
    memcpy(res + rlen, b, (size_t)n);
    rlen += (size_t)n;
}
static void check(const char *name, int ok, const char *got)
{
    nchk++;
    if (ok) say("ok %s\n", name);
    else { nfail++; say("FAIL %s: got %s\n", name, got && *got ? got : "(no text)"); }
}
static void checki(const char *name, long got, long want)
{
    char b[24];
    snprintf(b, sizeof b, "%ld", got);
    check(name, got == want, b);
}

/* the console's grid (CON_READ on descriptor 0) */
static unsigned char *cells;
static struct uconread grid;
static int readgrid(void)
{
    unsigned cap = 132 * 64 * 3;
    if (!cells && !(cells = malloc(cap))) return -1;
    memset(&grid, 0, sizeof grid);
    grid.size = sizeof grid;
    grid.cells = (u32)cells;
    grid.ncells = cap;
    if (sys_conread(0, &grid) < 0 || !grid.cols || grid.cols > 132 || grid.rows > 64) return -1;
    return 0;
}
static unsigned char *cell(int col, int row) { return cells + ((unsigned)row * grid.cols + (unsigned)col) * 3; }
/* the glyphs of one row from col, n of them, as text */
static const char *text(int col, int row, int n)
{
    static char b[133];
    for (int i = 0; i < n && i < 132; i++) b[i] = (char)cell(col + i, row)[0];
    b[n] = 0;
    return b;
}
static void checkt(const char *name, int col, int row, const char *want)
{
    check(name, !strcmp(text(col, row, (int)strlen(want)), want), text(col, row, (int)strlen(want)));
}
/* a cell's attribute bits and colour byte, as "a<bits> c<colour>" */
static void checkc(const char *name, int col, int row, int at, int co)
{
    char b[24];
    unsigned char *c = cell(col, row);
    snprintf(b, sizeof b, "a%d c%02x", c[1], c[2]);
    check(name, c[1] == at && c[2] == co, b);
}

static int cooked(void)
{
    struct termios t;
    return tcgetattr(0, &t) == 0 && (t.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG);
}
static unsigned mymem(void)
{
    struct uproc p;
    for (u32 i = 0; i < 256; i++)
        if (sys_procinfo(i, &p) >= 0 && p.state && (int)p.pid == getpid()) return p.mem;
    return 0;
}
static long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *keyname(int k)
{
    static char b[32];
    static const char *const names[] = { "UP", "DOWN", "LEFT", "RIGHT", "HOME", "END", "PGUP", "PGDN", "INS", "DEL",
                                         "BS", "TAB", "ENTER", "ESC", "HELP" };
    int base = K_BASE(k), n = 0;
    b[0] = 0;
    if (k & TM_SHIFT) n += snprintf(b + n, sizeof b - (size_t)n, "S-");
    if (k & TM_ALT) n += snprintf(b + n, sizeof b - (size_t)n, "A-");
    if (k & TM_CTRL) n += snprintf(b + n, sizeof b - (size_t)n, "C-");
    if (base >= K_F1 && base < K_F1 + 12) snprintf(b + n, sizeof b - (size_t)n, "F%d", base - K_F1 + 1);
    else if (base >= K_UP && base <= K_HELP) snprintf(b + n, sizeof b - (size_t)n, "%s", names[base - K_UP]);
    else if (base > 32 && base < 127) snprintf(b + n, sizeof b - (size_t)n, "%c", base);
    else snprintf(b + n, sizeof b - (size_t)n, "U+%04X", base);
    return b;
}

int main(void)
{
    int ti = sys_ttyinfo(), rows = ti >> 16, cols = ti & 0xFFFF, fd[2];
    TuiEv e;

    /* before the terminal is opened, and what is not a terminal */
    check("tui_box before tui_open: ASCII", tui_box(BX_TL) == '+' && tui_box(BX_H) == '-', "");
    checki("TuiCharWidth: a", TuiCharWidth('a'), 1);
    checki("TuiCharWidth: a Latin-1 letter", TuiCharWidth(0xE5), 1);
    checki("TuiCharWidth: a control", TuiCharWidth(7), -1);
    checki("TuiCharWidth: CJK takes two cells", TuiCharWidth(0x4E00), 2);
    checki("TuiCharWidth: a combining mark none", TuiCharWidth(0x301), 0);
    pipe(fd);
    check("TuiOpenIO on a pipe: no terminal", TuiOpenIO(fd[0], 1) == NULL && !TuiPossible(fd[0]), "");
    close(fd[0]);
    close(fd[1]);
    check("tui_possible on the console", tui_possible(), "");

    unsigned m0 = mymem();
    if (!tui_open()) {
        printf("FAIL tui_open\n");
        return 1;
    }
    unsigned m1 = mymem();
    char b[64];
    snprintf(b, sizeof b, "%dx%d", tui_cols, tui_rows);
    check("the size: the console's (TIOCGWINSZ)", tui_cols == cols && tui_rows == rows, b);
    check("an ANSI terminal (CTerm's path), Latin-1 (UTF8 0)", tui_cterm == 1 && tui_utf8 == 0, "");
    check("raw mode: no line editing, no echo, no signals", !cooked(), "");

    /* the widgets: a title bar, a box, Latin-1 and UTF-8 text, colours and attributes, a wide character, the full
     * block, the last cell, the cursor */
    struct Tui *t = tui_h;
    TuiClear(t);
    TuiFill(t, 0, 0, cols, ' ', 7, 4, 0);
    TuiPut(t, 1, 0, "tuitest: the text-UI library", 28, 15, 4, TA_BOLD);
    int bx = 2, by = 2, bw = 24, bh = 5;
    TuiPutC(t, bx, by, TuiBox(t, BX_TL), 7, TC_DEFAULT, 0);
    TuiFill(t, bx + 1, by, bw - 2, TuiBox(t, BX_H), 7, TC_DEFAULT, 0);
    TuiPutC(t, bx + bw - 1, by, TuiBox(t, BX_TR), 7, TC_DEFAULT, 0);
    for (int r = by + 1; r < by + bh - 1; r++) {
        TuiPutC(t, bx, r, TuiBox(t, BX_V), 7, TC_DEFAULT, 0);
        TuiPutC(t, bx + bw - 1, r, TuiBox(t, BX_V), 7, TC_DEFAULT, 0);
    }
    TuiPutC(t, bx, by + bh - 1, TuiBox(t, BX_BL), 7, TC_DEFAULT, 0);
    TuiFill(t, bx + 1, by + bh - 1, bw - 2, TuiBox(t, BX_H), 7, TC_DEFAULT, 0);
    TuiPutC(t, bx + bw - 1, by + bh - 1, TuiBox(t, BX_BR), 7, TC_DEFAULT, 0);
    TuiPut(t, bx + 2, by + 1, "Sm\xf6rg\xe5sbord", 11, 2, TC_DEFAULT, 0);              /* Latin-1 bytes */
    long w = TuiPutUTF8(t, bx + 2, by + 2, "\xc5\x81\xc3\xb3" "d\xc5\xba \xe2\x80\x93 \xe2\x80\x9cx\xe2\x80\x9d \xe2\x86\x92 ok",
                        -1, 3, TC_DEFAULT, TA_UNDER);                                  /* Łódź – “x” → ok */
    TuiPutC(t, bx + 2, by + 3, 0x4E00, 7, TC_DEFAULT, 0);                             /* wide: '?' and a space */
    TuiPutC(t, bx + 4, by + 3, 0x2588, 5, TC_DEFAULT, 0);                             /* full block: reversed space */
    TuiPut(t, 30, 3, "red", 3, 1, 4, 0);
    TuiPut(t, 30, 4, "x196", 4, TC_X(196), TC_DEFAULT, 0);
    TuiPut(t, 30, 5, "rgb", 3, TC_RGB(255, 0, 0), TC_DEFAULT, 0);   /* (green 2, 3, 6, 7 ...: see ports/tui/README) */
    TuiPut(t, 30, 6, "rev", 3, 6, TC_DEFAULT, TA_REV);
    TuiPutC(t, cols - 1, rows - 1, 'Z', 7, TC_DEFAULT, 0);
    TuiCursor(t, 10, 5, 1);
    TuiFlush(t);
    checki("TuiPutUTF8: the cells it took", w, 15);
    if (readgrid() < 0) check("CON_READ", 0, "cannot read the grid");
    else {
        checkt("the title", 1, 0, "tuitest: the text-UI library");
        checkc("the title: bold, white on blue", 1, 0, CONA_BOLD, 0x4F);
        checkt("the box's top", bx, by, "+----------------------+");
        checkt("the box's side", bx, by + 1, "|");
        checkt("the box's bottom", bx, by + bh - 1, "+----------------------+");
        checkt("Latin-1 text as it is", bx + 2, by + 1, "Sm\xf6rg\xe5sbord");
        checkc("Latin-1 text: green", bx + 2, by + 1, 0, 0x02);
        checkt("UTF-8 text: the nearest Latin-1", bx + 2, by + 2, "L\xf3" "dz - \"x\" > ok");
        checkc("UTF-8 text: underlined", bx + 2, by + 2, CONA_UND, 0x03);
        checkt("a wide character: '?' and a space", bx + 2, by + 3, "? ");
        checkc("the full block: a reversed space", bx + 4, by + 3, CONA_INV, 0x05);
        checkt("the full block's glyph", bx + 4, by + 3, " ");
        checkc("colours: red on blue", 30, 3, 0, 0x41);
        unsigned char x = cell(30, 4)[2], g = cell(30, 5)[2];
        snprintf(b, sizeof b, "%02x %02x", x, g);
        check("256 and 24-bit colours: the nearest of the 16", (x == 0x01 || x == 0x09) && (g == 0x01 || g == 0x09), b);
        checkc("reverse", 30, 6, CONA_INV, 0x06);
        checkt("the last cell drawn, nothing scrolled", cols - 1, rows - 1, "Z");
        checkt("... the title still on the first row", 1, 0, "tuitest");
        snprintf(b, sizeof b, "%u,%u %d", grid.ccol, grid.crow, (grid.flags & CONRD_CURSOR) != 0);
        check("the cursor where it was put, shown", grid.ccol == 10 && grid.crow == 5 && (grid.flags & CONRD_CURSOR), b);
    }

    /* only what changed is sent: a cell written behind the toolkit's back stays until a redraw */
    write(1, "\x1b[20;1H@@", 9);
    TuiPut(t, 40, 10, "changed", 7, 7, TC_DEFAULT, 0);
    TuiFlush(t);
    if (readgrid() == 0) {
        checkt("a flush sends only what changed", 0, 19, "@@");
        checkt("... and what changed", 40, 10, "changed");
    }
    TuiRedraw(t);
    TuiFlush(t);
    if (readgrid() == 0) checkt("TuiRedraw draws everything again", 0, 19, "  ");

    /* keys: single keys typed by the test after each prompt; the escape sequences put into the console's input here
     * all at once (CON_INPUT), as the keyboard gives them (the test's typing trickles bytes 20 ms apart, and more than
     * 40 ms between ESC and the rest is a lone Esc, as upstream decides) */
    static const char *const want[] = { "a", "UP", "F1", "C-RIGHT", "U+00E5", "C-c", "ESC", "S-TAB", "q" };
    static const char *const inject[] = { 0, "\x1b[A", "\x1bOP", "\x1b[1;5C", 0, 0, 0, "\x1b[Z", 0 };
    char got[200] = "";
    int nev = 0, bad = 0;
    for (;;) {
        if (nev < (int)(sizeof inject / sizeof inject[0]) && inject[nev] &&
            sys_coninput(0, CONI_TEXT, inject[nev], strlen(inject[nev])) != (int)strlen(inject[nev]))
            bad = 1;
        snprintf(b, sizeof b, "ev%d> ", nev);     /* a row of its own each time: sent whole (the test waits for it) */
        TuiPut(t, 0, 12 + nev % 10, b, (long)strlen(b), 7, TC_DEFAULT, 0);
        TuiFlush(t);
        if (!TuiWait(t, &e, -1, 0, NULL)) continue;
        if (e.type == TE_END) { bad = 1; break; }
        if (e.type != TE_KEY) continue;
        const char *k = keyname(e.key);
        if (strlen(got) + strlen(k) + 2 < sizeof got) { strcat(got, k); strcat(got, " "); }
        if (nev < (int)(sizeof want / sizeof want[0]) && strcmp(k, want[nev])) bad = 1;
        nev++;
        if (e.key == 'q' || nev > 20) break;
    }
    say("events: %s\n", got);
    check("the keys: letters, arrows, function keys, modifiers, Latin-1, ^C, a lone Esc", !bad &&
          nev == (int)(sizeof want / sizeof want[0]), got);
    TuiPut(t, 40, 12, "waiting", 7, 7, TC_DEFAULT, 0);
    TuiFlush(t);
    long t0 = ms_now();
    int r = TuiWait(t, &e, 300000, 0, NULL);
    long dt = ms_now() - t0;
    snprintf(b, sizeof b, "%d %d after %ld ms", r, e.type, dt);
    check("TuiWait with a time: TE_NONE when it is up", r == 0 && e.type == TE_NONE && dt >= 250 && dt < 2000, b);
    r = TuiPoll(t, &e);
    check("TuiPoll: nothing there", r == 0 && e.type == TE_NONE && !TuiKeyWaiting(t), "");
    checki("TuiSignal: no AmigaOS signals", (long)TuiSignal(t), 0);

    /* the terminal lent to a command and taken back */
    tui_suspend();
    check("suspended: cooked again", cooked(), "");
    printf("tuitest: a command's output\n");
    fflush(stdout);
    tui_keymsg = " tuitest: a key goes back ";
    tui_keyafter();
    check("TuiKeyAfter: cooked after the key", cooked(), "");
    tui_resume();
    check("resumed: raw again", !cooked(), "");
    TuiFlush(t);
    if (readgrid() == 0) {
        checkt("resumed: everything drawn again", 1, 0, "tuitest: the text-UI library");
        checkt("... the box too", bx, by, "+----------------------+");
    }

    tui_close();
    check("closed: cooked", cooked(), "");
    if (readgrid() == 0) {
        check("closed: the cursor shown", (grid.flags & CONRD_CURSOR) != 0, "");
        checkt("closed: the screen cleared", 1, 0, "                            ");
    }
    check("tui_wait after tui_close: TE_END", tui_wait(&e, -1, 0, NULL) == 1 && e.type == TE_END, "");
    unsigned m2 = mymem();
    printf("\ntuitest: the results\n%.*s", (int)rlen, res ? res : "");   /* a line of their own after the clear */
    printf("memory: %u bytes while open, %u after\n", m1 - m0, m2 - m0);
    printf("tuitest: %d checks, %d failed\n", nchk, nfail);
    return nfail ? 1 : 0;
}
