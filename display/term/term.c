/* SPDX-License-Identifier: GPL-2.0-only */
/* term, the terminal window of Zenix Desktop (DESIGN D38 5 and step 6, docs/man/term.txt): a window of 80 columns
 * (24 rows at first; resized in whole rows) over a pseudo-terminal running the user's shell, or the command after
 * -e. What the program writes goes through the console's own escape parser and cell model (model.c: the kernel's
 * term.c built here unchanged) and is drawn through libdisplay, a run of cells in one colour as one text order; the
 * keys the window gets go to the pty as the console's keyboard sends them (the arrows as ESC [ A, or ESC O A in
 * cursor-key mode). The 16 ANSI colours are the screen's nearest pens. Closing the window hangs the pty up. */
#include "libz.h"
#include "../display.h"
#include "model.h"

static int win, m = -1, rows = 24, cols = 80, pen_of[16], bell_due;
/* the keys for the program: the master does not wait (a program that does not read its input never stops this
 * window); what does not fit waits here, and past it keys are dropped and the bell says so */
static char pend[512];
static int npend;
static void to_pty(const char *s, int n)
{
    if (npend + n > (int)sizeof pend) {
        bell_due = 1;
        return;
    }
    memcpy(pend + npend, s, (u32)n);
    npend += n;
}
static void pend_flush(void)
{
    if (!npend || m < 0)
        return;
    int r = sys_write(m, pend, (u32)npend);
    if (r > 0) {
        memmove(pend, pend + r, (u32)(npend - r));
        npend -= r;
    }
}
static u32 pal[16];

/* ---- the model's calls ---- */
void *m_alloc(unsigned n) { return malloc(n); }
void m_free(void *p) { free(p); }
void m_bell(void) { bell_due = 1; }
static void to_pty(const char *s, int n);
void m_answer(const char *s) { to_pty(s, (int)strlen(s)); }

static struct { int row, col, n, attr, colour, len; char b[256]; } run = { -1, 0, 0, 0, 0, 0, "" };
static void run_flush(void)
{
    if (run.n) {
        int at = run.attr, fg = pen_of[run.colour & 15], bg = pen_of[run.colour >> 4 & 15];
        int ta = (at & 4 ? TA_BOLD : 0) | (at & 2 ? TA_UNDER : 0) | (at & 1 ? TA_INV : 0);
        int y = run.row * 8, sb = 0, sc = 0, cell = 0;     /* the text so far: its first byte and cell */
        for (int i = 0; i <= run.len; ) {
            int k = i;                  /* a run of 6 or more blanks (no underline) is a fill, not 6 glyphs */
            while (k < run.len && run.b[k] == ' ') k++;
            if (!(ta & TA_UNDER) && k - i >= 6) {
                if (i > sb)
                    dpy_text(win, (run.col + sc) * 8, y, fg, bg, ta, run.b + sb, i - sb);
                dpy_fill(win, (run.col + cell) * 8, y, (k - i) * 8, 8, ta & TA_INV ? fg : bg);
                cell += k - i;
                i = sb = k;
                sc = cell;
                continue;
            }
            if (i == run.len)
                break;
            for (i = k > i ? k : i + 1; i < run.len && ((unsigned char)run.b[i] & 0xC0) == 0x80; i++)
                ;                       /* the cell's other bytes */
            cell = sc;                  /* cells from sb to i */
            for (int j = sb; j < i; j++)
                if (((unsigned char)run.b[j] & 0xC0) != 0x80) cell++;
        }
        if (run.len > sb)
            dpy_text(win, (run.col + sc) * 8, y, fg, bg, ta, run.b + sb, run.len - sb);
    }
    run.n = run.len = 0;
    run.row = -1;
}
/* What the model changes is not drawn at once: each row keeps the columns that changed (d0..d1) and a scroll of the
 * whole screen is counted (pscroll), so a burst of output is shown as one copy and the rows it left, at most every
 * PRESENT ticks while it streams (xterm's jump scroll), and a key's echo as the cells it changed. */
#define PRESENT 4
static unsigned char d0[32], d1[32];
static int pscroll, live, nshown, cost;
static unsigned last_shown;
static int gap(void)                    /* a quarter of the time drawing at most, never still for 0.4 s */
{
    return PRESENT > 3 * cost ? PRESENT : 3 * cost > 40 ? 40 : 3 * cost;
}
static void dirty(int row, int x0, int x1)
{
    if (row < 0 || row >= rows) return;
    if (d1[row] == 0) { d0[row] = (unsigned char)x0; d1[row] = (unsigned char)x1; return; }
    if (x0 < d0[row]) d0[row] = (unsigned char)x0;
    if (x1 > d1[row]) d1[row] = (unsigned char)x1;
}
static int any_dirty(void)
{
    for (int r = 0; r < rows; r++)
        if (d1[r]) return 1;
    return pscroll != 0;
}
static void present(void)
{
    unsigned t0 = sys_ticks();
    last_shown = t0;
    if (!any_dirty())
        return;
    nshown++;
    live = 1;
    if (m_back()) {                     /* the scrollback is shown: all of it again */
        m_draw_cells(0, 0, cols);
        pscroll = 0;
        memset(d1, 0, sizeof d1);
    }
    if (pscroll)                        /* the scroll: one copy of what stays */
        dpy_copy(win, 0, pscroll * 8, cols * 8, (rows - pscroll) * 8, 0, 0);
    pscroll = 0;
    for (int r = 0; r < rows; r++)
        if (d1[r]) {
            m_draw_cells(r, d0[r], d1[r]);
            d1[r] = 0;
        }
    run_flush();
    live = 0;
    dpy_flush();
    cost = (int)(sys_ticks() - t0);     /* the server's answer comes when it has drawn (the copy's blit started) */
}
void m_cell(int row, int col, unsigned v, int attr, int colour)
{
    if (!live) {
        dirty(row, col, col + 1);
        return;
    }
    if (row != run.row || col != run.col + run.n || attr != run.attr || colour != run.colour || run.len > 240)
        run_flush();
    if (!run.n) {
        run.row = row;
        run.col = col;
        run.attr = attr;
        run.colour = colour;
    }
    if (v < 0x80)
        run.b[run.len++] = (char)(v < 32 ? ' ' : v);
    else if (v < 0x800) {
        run.b[run.len++] = (char)(0xC0 | v >> 6);
        run.b[run.len++] = (char)(0x80 | (v & 0x3F));
    } else {
        run.b[run.len++] = (char)(0xE0 | v >> 12);
        run.b[run.len++] = (char)(0x80 | (v >> 6 & 0x3F));
        run.b[run.len++] = (char)(0x80 | (v & 0x3F));
    }
    run.n++;
}
void m_fill(int row, int n, int colour)
{
    if (live) {                         /* the scrollback's redraw */
        run_flush();
        dpy_fill(win, 0, row * 8, cols * 8, n * 8, pen_of[colour >> 4 & 15]);
        return;
    }
    for (int r = row; r < row + n; r++)
        dirty(r, 0, cols);
}
void m_move(int dst, int src, int n)
{
    if (dst == 0 && src == 1 && n == rows - 1) {    /* the whole screen up a line: counted, the rows' marks with it */
        if (++pscroll >= rows) {
            pscroll = 0;
            for (int r = 0; r < rows; r++) dirty(r, 0, cols);
            return;
        }
        memmove(d0, d0 + 1, (u32)(rows - 1));
        memmove(d1, d1 + 1, (u32)(rows - 1));
        d1[rows - 1] = 0;
        return;
    }
    if (!any_dirty()) {                 /* a region's move on a screen that is up to date: one copy now */
        dpy_copy(win, 0, src * 8, cols * 8, n * 8, 0, dst * 8);
        return;
    }
    for (int r = dst; r < dst + n; r++) /* else its rows drawn from the model at the next showing */
        dirty(r, 0, cols);
}

/* the console's 16 colours (kernel/gfx.c) as the screen's nearest pens (D38 5) */
static const unsigned short ansi[16] = { 0x000, 0xC22, 0x2B2, 0xBB2, 0x45E, 0xB3B, 0x2BB, 0xBBB,
                                         0x666, 0xF66, 0x6F6, 0xFF6, 0x88F, 0xF7F, 0x6FF, 0xFFF };
static void colours(void)
{
    unsigned short p[16];
    memset(p, 0, sizeof p);
    int depth = dpy_palette(p, 16), n = depth > 0 && depth <= 4 ? 1 << depth : 8;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        u32 bd = ~0u;
        for (int k = 0; k < n; k++) {
            int dr = (ansi[i] >> 8) - (p[k] >> 8), dg = (ansi[i] >> 4 & 15) - (p[k] >> 4 & 15), db = (ansi[i] & 15) - (p[k] & 15);
            u32 d = (u32)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
            if (d < bd) { bd = d; best = k; }
        }
        pen_of[i] = best;
        pal[i] = p[i];
    }
}

/* ---- keys, as kernel/kbd.c sends them to a raw-mode program ---- */
static void key(const struct dpyev *e)
{
    if (e->code & 0x80)
        return;
    int c = e->code, shift = e->qual & (GQ_LSHIFT | GQ_RSHIFT), ctrl = e->qual & GQ_CTRL,
        alt = e->qual & (GQ_LALT | GQ_RALT), mods = (shift ? 1 : 0) | (alt ? 2 : 0) | (ctrl ? 4 : 0);
    char s[16];
    int n = 0;
    if (c >= 0x4C && c <= 0x4F) {
        if (shift && (c == 0x4C || c == 0x4D)) {    /* Shift+Up/Down: the scrollback, a line (the console's Amiga+Up) */
            m_scrollback(c == 0x4C ? 1 : -1);
            dpy_flush();
            return;
        }
        s[n++] = 27;
        s[n++] = !mods && m_ckm() ? 'O' : '[';
        if (mods) { s[n++] = '1'; s[n++] = ';'; s[n++] = (char)('1' + mods); }
        s[n++] = "ABCD"[c - 0x4C];
    } else if (c >= 0x50 && c <= 0x59) {
        static const char *const fk[10] = { "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~", "\033[18~",
                                            "\033[19~", "\033[20~", "\033[21~" };
        strcpy(s, fk[c - 0x50]);
        n = (int)strlen(s);
    } else if (c == 0x46) { strcpy(s, "\033[3~"); n = 4; }          /* Del */
    else if (c == 0x5F) { strcpy(s, "\033[2~"); n = 4; }            /* Help = Insert */
    else if (c == 0x42 && shift) { strcpy(s, "\033[Z"); n = 3; }    /* Shift+Tab */
    else if (c == 0x41) s[n++] = 127;                               /* Backspace: DEL, as the console in raw mode */
    else
        for (int i = 0; i < 4 && e->ch[i]; i++)
            s[n++] = (char)e->ch[i];
    if (n)
        to_pty(s, n);
}

static void resize(int w, int h)
{
    int r = h / 8, c = w / 8;
    if (r < 4 || c < 20 || (r == rows && c == cols))
        return;
    present();
    rows = r;
    cols = c > 80 ? 80 : c;
    m_rows(rows);
    m_setcols(cols);
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    ioctl(m, TIOCSWINSZ, &ws);          /* the program on the pty gets SIGWINCH */
    for (int k = 0; k < rows; k++)
        dirty(k, 0, cols);
}

static int quit_menu_new;
static void menu(const struct dpyev *e)
{
    if (e->code == 0 && e->arg == 0)
        quit_menu_new = 1;              /* Window: New terminal */
    else if (e->code == 0 && e->arg == 1)
        quit_menu_new = 2;              /* Window: Close */
}

int main(int argc, char **argv)
{
    char **cmd = 0;
    int px = -1, py = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-e") && i + 1 < argc) { cmd = &argv[i + 1]; break; }
        if (!strcmp(argv[i], "-r") && i + 1 < argc) { rows = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { cols = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "-p") && i + 1 < argc && strchr(argv[i + 1], ',')) {
            px = atoi(argv[++i]); py = atoi(strchr(argv[i], ',') + 1);
            if (px < 0 || py < 0) px = py = -1;
            continue;
        }
        dprintf(2, "usage: term [-r ROWS] [-c COLUMNS] [-p X,Y] [-e COMMAND [ARGUMENTS]]\n");
        return 2;
    }
    if (rows < 4) rows = 4;
    if (rows > 29) rows = 29;
    if (cols < 20) cols = 20;
    if (cols > 80) cols = 80;
    int r = dpy_open("term");
    if (r < 0) {
        dprintf(2, "term: the display: %s\n", zstrerror(r) ? zstrerror(r) : "not reachable");
        return 1;
    }
    int s = -1;
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    m = posix_openpt(O_RDWR | O_NONBLOCK);
    if (m >= 0 && ptsname(m))
        s = sys_open(ptsname(m), O_RDWR);
    if (m < 0 || s < 0) {
        dprintf(2, "term: no pseudo-terminal: %s\n", zwhy("all in use"));
        return 1;
    }
    ioctl(m, TIOCSWINSZ, &ws);
    char path[ZPATH_MAX];
    const char *sh = getenv("SHELL");
    char *shv[2] = { (char *)(sh && *sh ? sh : "/bin/sh"), 0 };
    char **av = cmd ? cmd : shv;
    if (!path_find(av[0], path, sizeof path)) {
        dprintf(2, "term: %s: not found\n", av[0]);
        return 1;
    }
    struct dpywin d;
    memset(&d, 0, sizeof d);
    d.kind = WK_NORMAL;
    d.x = (short)px;                    /* -1: the window manager's choice */
    d.y = (short)py;
    d.w = (unsigned short)(cols * 8);
    d.h = (unsigned short)(rows * 8);
    d.minw = 20 * 8;                    /* 20 to 80 columns (the console's model: rows of 80), 4 to 29 rows */
    d.maxw = 640;
    d.minh = 4 * 8;
    d.maxh = 29 * 8;
    d.stepw = 8;
    d.steph = 8;
    const char *b = strrchr(av[0], '/');
    strncpy(d.title, b ? b + 1 : av[0], DPY_TITLE - 1);
    win = dpy_newwin(&d);
    if (win < 0) {
        dprintf(2, "term: no window: %d\n", win);
        return 1;
    }
    static const char menus[] = "Window\tNew terminal\tClose";
    dpy_menu(win, menus, sizeof menus - 1);
    colours();
    m_rows(rows);
    m_setcols(cols);
    m_write("\033[H\033[2J", 7);
    present();
    /* the size the window manager gives before the program starts: zdesk keeps a window inside the screen, so 80
     * columns and a frame become 78 or 79; a program that read 80 and drew 80 into them before its first SIGWINCH
     * wrapped its line ends into the next rows (ctop's stray digits, "ru40ing", 2026-11-05).
     * Up to half a second for the window to be shown (without a manager it is shown as asked) */
    for (unsigned t0 = sys_ticks(); (int)(sys_ticks() - t0) < 50; ) {
        struct dpyev e;
        int got = dpy_next(&e, 5);
        if (got < 0) break;
        if (!got) continue;
        if (e.type == EV_RESIZE) resize(e.w, e.h);
        else if (e.type == EV_EXPOSE) break;
    }
    setenv("TERM", "xterm-16color", 1);
    int io[3] = { s, s, s };
    int pid = sys_spawn(path, av, io);
    sys_close(s);
    if (pid < 0) {
        dprintf(2, "term: %s: %s\n", path, zwhy("cannot start"));
        return 1;
    }
    static char buf[2048];              /* a pty's whole output ring at once */
    unsigned last_read = 0;
    for (;;) {
        struct pollfd p[2] = { { dpy_port(), POLLPORT | POLLRDBAND, 0 }, { m, (short)(POLLIN | (npend ? POLLOUT : 0)), 0 } };
        int wait = npend ? 10 : -1;
        if (any_dirty()) {              /* the next showing: PRESENT ticks after the last, sooner if the output pauses */
            int left = gap() - (int)(sys_ticks() - last_shown);
            wait = left < 1 ? 1 : left;
        }
        if (sys_evwait(p, 2, wait) < 0)
            continue;
        if (p[1].revents & (POLLIN | POLLHUP)) {
            int n = sys_read(m, buf, sizeof buf);
            if (n <= 0 && n != -EAGAIN)
                break;                  /* the program ended: the window goes */
            if (n > 0) {
                for (int at = 0; at < n; at += 256) {   /* the cells' work in pieces: a showing between them */
                    m_write(buf + at, (unsigned)(n - at < 256 ? n - at : 256));
                    if ((int)(sys_ticks() - last_shown) >= gap())
                        present();
                }
                /* output streaming in pieces (a line a write): a tick for the program to write more, so the
                 * window takes it in big reads, not a wake-up a line; a key's echo alone is never held */
                if (n < (int)sizeof buf && (unsigned)(sys_ticks() - last_read) <= 1)
                    sys_sleep(1);
                last_read = sys_ticks();
            }
        }
        if (!(p[1].revents & POLLIN) || (int)(sys_ticks() - last_shown) >= gap())
            present();                  /* the output paused, or streams on: what it left */
        pend_flush();
        while (sys_waitnb(-1) >= 0)     /* the terminals "New terminal" started that ended */
            ;
        struct dpyev e;
        int got, gone = 0;
        while ((got = dpy_next(&e, 0)) > 0) {
            if (e.type == EV_KEY) key(&e);
            else if (e.type == EV_EXPOSE) { /* the rows it names (scrolls still pending shown first: screen rows) */
                present();
                for (int r = e.y / 8; r <= (e.y + e.h - 1) / 8; r++) dirty(r, e.x / 8, (e.x + e.w + 7) / 8);
            } else if (e.type == EV_RESIZE) resize(e.w, e.h);
            else if (e.type == EV_CLOSE) gone = 1;
            else if (e.type == EV_MENU) menu(&e);
        }
        if (quit_menu_new == 1) {
            quit_menu_new = 0;
            char *nv[] = { "/bin/term", 0 };
            sys_spawn("/bin/term", nv, 0);
        } else if (quit_menu_new == 2)
            gone = 1;
        if (bell_due)
            bell_due = 0;               /* the bell: no sound on the desktop yet */
        if (any_dirty() && !(p[1].revents & POLLIN))
            present();                  /* an expose, a resize, a scrollback key */
        dpy_flush();
        if (gone || got < 0)
            break;
    }
    sys_close(m);                       /* the pty hangs up: SIGHUP to the program */
    dpy_close();
    for (int t = 0; t < 100 && sys_waitnb(pid) == -2; t++)   /* a second for it to end, then left to init */
        sys_sleep(1);
    return 0;
}
