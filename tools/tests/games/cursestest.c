/* SPDX-License-Identifier: GPL-2.0-only */
/* The BSD games' curses (ports/bsdgames/zenix/curses.c) on the PC (tools/games-host-test.sh): random drawing through
 * every call the games use, refreshed now and then, written to standard output (a pipe: the size from LINES and
 * COLUMNS). After each refresh the screen the program means is written to the file named by argv[1], a frame after
 * the other, each line its characters with "|~" and its attributes after it; the output marks each frame's end with
 * CSI 999 Z, then refreshes again with nothing changed and marks that with CSI 998 Z (nothing may come between). The
 * test decodes the output (tools/tests/games/screen.py) and compares the terminal's screen with each frame. argv[2]
 * gets the rounds done. */
#include <curses.h>
#include <stdlib.h>
#include <string.h>

static FILE *model, *counts;
static unsigned rnd = 12345;
static unsigned r(unsigned n) { rnd = rnd * 1103515245u + 12345u; return (rnd >> 16) % n; }

static void frame(void)
{
    for (int y = 0; y < LINES; y++) {
        for (int x = 0; x < COLS; x++)
            fputc(stdscr->cells[y * COLS + x] & A_CHARTEXT, model);
        fputs("|~", model);
        for (int x = 0; x < COLS; x++)
            fputc('0' + ((stdscr->cells[y * COLS + x] & A_ATTRIBUTES) >> 8), model);
        fputc('\n', model);
    }
    fputs("==\n", model);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    model = fopen(argv[1], "w");
    counts = fopen(argv[2], "w");
    initscr();
    cbreak();
    noecho();
    leaveok(stdscr, TRUE);
    for (int round = 0; round < 300; round++) {
        int n = 1 + (int)r(40);
        for (int k = 0; k < n; k++) {
            int y = (int)r((unsigned)LINES), x = (int)r((unsigned)COLS);
            switch (r(12)) {
            case 0: case 1: case 2: mvaddch(y, x, 'A' + r(26)); break;
            case 3: move(y, x); addstr("neon rain"); break;
            case 4: standout(); mvaddstr(y, x, "HOT"); standend(); break;
            case 5: move(y, x); clrtoeol(); break;
            case 6: move(y, 0); deleteln(); break;
            case 7: mvprintw(y, x, "%d", (int)r(100000)); break;
            case 8: attron(A_BOLD); mvaddch(y, x, '#'); attroff(A_BOLD); break;
            case 9: if (r(20) == 0) erase(); break;
            case 10: move(y, x); insertln(); break;
            case 11: mvaddch(y, x, 0xE9); break;        /* Latin-1: written as UTF-8 */
            }
        }
        if (r(30) == 0) {
            WINDOW *w = newwin(5, 20, (int)r((unsigned)LINES - 5), (int)r((unsigned)COLS - 20));
            box(w, 0, 0);
            mvwaddstr(w, 2, 2, "derezzed");
            wrefresh(w);
            delwin(w);
            touchwin(stdscr);           /* the window's cells over stdscr's: drawn again whole */
        }
        refresh();
        frame();
        fputs("\033[999Z", stdout);      /* the frame's end, for screen.py; then a refresh that must write nothing */
        fflush(stdout);
        refresh();
        fputs("\033[998Z", stdout);
        fflush(stdout);
        fprintf(counts, "%d\n", round);
    }
    endwin();
    fclose(model);
    fclose(counts);
    return 0;
}
