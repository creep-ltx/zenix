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

static int win, m = -1, rows = 24, pen_of[16], bell_due;
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
        dpy_text(win, run.col * 8, run.row * 8, fg, bg, ta, run.b, run.len);
    }
    run.n = run.len = 0;
    run.row = -1;
}
void m_cell(int row, int col, unsigned v, int attr, int colour)
{
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
    run_flush();
    dpy_fill(win, 0, row * 8, 640, n * 8, pen_of[colour >> 4 & 15]);
}
void m_move(int dst, int src, int n)
{
    run_flush();
    dpy_copy(win, 0, src * 8, 640, n * 8, 0, dst * 8);
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

static void resize(int h)
{
    int r = h / 8;
    if (r == rows || r < 4)
        return;
    rows = r;
    m_rows(rows);
    struct winsize ws = { (unsigned short)rows, 80, 0, 0 };
    ioctl(m, TIOCSWINSZ, &ws);          /* the program on the pty gets SIGWINCH */
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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-e") && i + 1 < argc) { cmd = &argv[i + 1]; break; }
        if (!strcmp(argv[i], "-r") && i + 1 < argc) { rows = atoi(argv[++i]); continue; }
        dprintf(2, "usage: term [-r ROWS] [-e COMMAND [ARGUMENTS]]\n");
        return 2;
    }
    if (rows < 4) rows = 4;
    if (rows > 29) rows = 29;
    int r = dpy_open("term");
    if (r < 0) {
        dprintf(2, "term: the display: %s\n", zstrerror(r) ? zstrerror(r) : "not reachable");
        return 1;
    }
    int s = -1;
    struct winsize ws = { (unsigned short)rows, 80, 0, 0 };
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
    setenv("TERM", "xterm-16color", 1);
    int io[3] = { s, s, s };
    int pid = sys_spawn(path, av, io);
    sys_close(s);
    if (pid < 0) {
        dprintf(2, "term: %s: %s\n", path, zwhy("cannot start"));
        return 1;
    }
    struct dpywin d;
    memset(&d, 0, sizeof d);
    d.kind = WK_NORMAL;
    d.x = d.y = -1;
    d.w = 640;
    d.h = (unsigned short)(rows * 8);
    d.minw = d.maxw = 640;              /* the console's model: 80 columns */
    d.minh = 4 * 8;
    d.maxh = 29 * 8;
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
    m_write("\033[H\033[2J", 7);
    dpy_flush();
    char buf[512];
    for (;;) {
        struct pollfd p[2] = { { dpy_port(), POLLPORT | POLLRDBAND, 0 }, { m, (short)(POLLIN | (npend ? POLLOUT : 0)), 0 } };
        if (sys_evwait(p, 2, npend ? 10 : -1) < 0)
            continue;
        if (p[1].revents & (POLLIN | POLLHUP)) {
            int n = sys_read(m, buf, sizeof buf);
            if (n <= 0 && n != -EAGAIN)
                break;                  /* the program ended: the window goes */
            if (n > 0) {
                m_write(buf, (unsigned)n);
                run_flush();
            }
        }
        pend_flush();
        while (sys_waitnb(-1) >= 0)     /* the terminals "New terminal" started that ended */
            ;
        struct dpyev e;
        int got, gone = 0;
        while ((got = dpy_next(&e, 0)) > 0) {
            if (e.type == EV_KEY) key(&e);
            else if (e.type == EV_EXPOSE) { m_redraw(); run_flush(); }
            else if (e.type == EV_RESIZE) { resize(e.h); m_redraw(); run_flush(); }
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
