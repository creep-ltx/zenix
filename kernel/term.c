#include "kernel.h"

/* Virtual consoles, as on Linux: NCONS (10) of them, /dev/tty1 .. /dev/tty10,
 * switched with Amiga+F1..F10. Each has its own text grid (80 columns, all the
 * rows of the screen: 32 on PAL, 25 on NTSC, with attributes), its own
 * scrollback and its own tty. Only the current console is drawn; switching
 * redraws from the grid. The terminal understands the usual ANSI/VT100
 * sequences (cursor movement, erase, insert/delete line and character,
 * scroll regions, bold/underline/inverse) so ordinary programs and editors
 * work. A console's grid and scrollback are allocated when it is first used. */

#define HLINES 200                      /* scrollback lines per console */

#define A_INV 1
#define A_UND 2
#define A_BOLD 4

struct term {
    u8 ch[TROWS_MAX][COLS];
    u8 at[TROWS_MAX][COLS];
    int cx, cy, sx, sy;                 /* cursor and its saved position */
    u8 attr;                            /* attributes for new characters */
    int top, bot;                       /* scroll region (rows, inclusive) */
    int wrap;                           /* last column written: wrap before the next character */
    int curvis;
    int esc, nparam, param[6], priv;    /* escape sequence parser */
    u8 (*hist)[COLS];                   /* scrollback: the oldest line is hist[(hhead - hn) mod HLINES] */
    int hn, hhead;
    int back;                           /* lines scrolled back (0 = live) */
};

static struct term *wsp[NCONS + 1];
int con_cur = 1;
u16 con_want;                           /* bit n: console n was shown with nobody on it: init starts a login there */

static void reset_term(struct term *t)
{
    memset(t->ch, ' ', sizeof t->ch);
    memset(t->at, 0, sizeof t->at);
    t->cx = t->cy = t->sx = t->sy = 0;
    t->attr = 0;
    t->top = 0;
    t->bot = trows - 1;
    t->wrap = 0;
    t->curvis = 1;
    t->esc = t->nparam = t->priv = 0;
}

static struct term *get(int w)
{
    if (!wsp[w]) {
        wsp[w] = kmalloc(sizeof *wsp[w]);
        if (!wsp[w])
            panic("term: out of memory");
        memset(wsp[w], 0, sizeof *wsp[w]);
        reset_term(wsp[w]);
    }
    return wsp[w];
}

static void draw(struct term *t, int y, int x)
{
    gfx_cell(y, x, t->ch[y][x], t->at[y][x]);
}

static void cursor(struct term *t, int on)
{
    if (!t->curvis || t->back)
        return;
    if (on)
        gfx_cell(t->cy, t->cx, t->ch[t->cy][t->cx], t->at[t->cy][t->cx] ^ A_INV);
    else
        draw(t, t->cy, t->cx);
}

static void draw_row(struct term *t, int y)
{
    for (int x = 0; x < COLS; x++)
        draw(t, y, x);
}

static void redraw_all(struct term *t)
{
    gfx_clear_text();
    for (int y = 0; y < trows; y++)
        for (int x = 0; x < COLS; x++)
            if (t->ch[y][x] != ' ' || t->at[y][x])
                draw(t, y, x);
}

/* ---- scrollback ---- */

static void hist_push(struct term *t, const u8 *line)
{
    if (!t->hist) {
        t->hist = kmalloc(HLINES * COLS);
        if (!t->hist)
            return;
        t->hn = t->hhead = 0;
    }
    memcpy(t->hist[t->hhead], line, COLS);
    t->hhead = (t->hhead + 1) % HLINES;
    if (t->hn < HLINES)
        t->hn++;
}

/* a line of the composite (scrollback then grid): index 0 is the oldest */
static const u8 *comp_line(struct term *t, int k, int *from_grid)
{
    *from_grid = k >= t->hn;
    if (k >= t->hn)
        return t->ch[k - t->hn];
    return t->hist[(t->hhead - t->hn + k + HLINES * 2) % HLINES];
}

static void show_back(struct term *t)
{
    gfx_clear_text();
    for (int r = 0; r < trows; r++) {
        int from_grid;
        const u8 *l = comp_line(t, t->hn - t->back + r, &from_grid);
        for (int x = 0; x < COLS; x++)
            if (l[x] != ' ')
                gfx_cell(r, x, l[x], 0);
    }
}

static void leave_back(struct term *t, int vis)
{
    if (!t->back)
        return;
    t->back = 0;
    if (vis)
        redraw_all(t);
}

void term_scroll_view(int delta)
{
    struct term *t = get(con_cur);
    if (!gfx_up())
        return;
    int nb = t->back + delta;
    if (nb < 0) nb = 0;
    if (nb > t->hn) nb = t->hn;
    if (nb == t->back)
        return;
    cursor(t, 0);
    t->back = nb;
    if (nb)
        show_back(t);
    else {
        redraw_all(t);
        cursor(t, 1);
    }
}

int term_back(void) { return wsp[con_cur] ? wsp[con_cur]->back : 0; }

/* ---- scrolling inside the region ---- */

static void scroll_up(struct term *t, int vis)          /* the region moves up one line */
{
    int full = (t->top == 0 && t->bot == trows - 1);
    if (full)
        hist_push(t, t->ch[0]);
    memmove(t->ch[t->top], t->ch[t->top + 1], (u32)(t->bot - t->top) * COLS);
    memmove(t->at[t->top], t->at[t->top + 1], (u32)(t->bot - t->top) * COLS);
    memset(t->ch[t->bot], ' ', COLS);
    memset(t->at[t->bot], 0, COLS);
    if (!vis)
        return;
    if (full) {
        gfx_scroll();                                   /* the Copper pointer moves: no copying */
    } else {
        for (int y = t->top; y <= t->bot; y++)
            draw_row(t, y);
    }
}

static void scroll_down(struct term *t, int vis)        /* the region moves down one line */
{
    memmove(t->ch[t->top + 1], t->ch[t->top], (u32)(t->bot - t->top) * COLS);
    memmove(t->at[t->top + 1], t->at[t->top], (u32)(t->bot - t->top) * COLS);
    memset(t->ch[t->top], ' ', COLS);
    memset(t->at[t->top], 0, COLS);
    if (vis)
        for (int y = t->top; y <= t->bot; y++)
            draw_row(t, y);
}

static void linefeed(struct term *t, int vis)
{
    if (t->cy == t->bot)
        scroll_up(t, vis);
    else if (t->cy < trows - 1)
        t->cy++;
}

static void clear_cells(struct term *t, int vis, int y, int x0, int x1)       /* [x0,x1) */
{
    for (int x = x0; x < x1; x++) {
        t->ch[y][x] = ' ';
        t->at[y][x] = 0;
        if (vis)
            draw(t, y, x);
    }
}

static void erase_screen(struct term *t, int vis, int mode)
{
    if (mode == 2) {
        memset(t->ch, ' ', sizeof t->ch);
        memset(t->at, 0, sizeof t->at);
        if (vis)
            gfx_clear_text();
        return;
    }
    if (mode == 0) {
        clear_cells(t, vis, t->cy, t->cx, COLS);
        for (int y = t->cy + 1; y < trows; y++)
            clear_cells(t, vis, y, 0, COLS);
    } else {
        for (int y = 0; y < t->cy; y++)
            clear_cells(t, vis, y, 0, COLS);
        clear_cells(t, vis, t->cy, 0, t->cx + 1);
    }
}

static int arg(struct term *t, int i, int def)
{
    return (i < t->nparam && t->param[i] > 0) ? t->param[i] : def;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void csi(struct term *t, int vis, char c)
{
    int n = arg(t, 0, 1);
    t->wrap = 0;
    switch (c) {
    case 'A': t->cy = clamp(t->cy - n, t->cy >= t->top ? t->top : 0, trows - 1); break;
    case 'B': t->cy = clamp(t->cy + n, 0, t->cy <= t->bot ? t->bot : trows - 1); break;
    case 'C': t->cx = clamp(t->cx + n, 0, COLS - 1); break;
    case 'D': t->cx = clamp(t->cx - n, 0, COLS - 1); break;
    case 'E': t->cy = clamp(t->cy + n, 0, trows - 1); t->cx = 0; break;
    case 'F': t->cy = clamp(t->cy - n, 0, trows - 1); t->cx = 0; break;
    case 'G': t->cx = clamp(arg(t, 0, 1) - 1, 0, COLS - 1); break;
    case 'd': t->cy = clamp(arg(t, 0, 1) - 1, 0, trows - 1); break;
    case 'H': case 'f':
        t->cy = clamp(arg(t, 0, 1) - 1, 0, trows - 1);
        t->cx = clamp(arg(t, 1, 1) - 1, 0, COLS - 1);
        break;
    case 'J': erase_screen(t, vis, t->nparam ? t->param[0] : 0); break;
    case 'K': {
        int m = t->nparam ? t->param[0] : 0;
        if (m == 0) clear_cells(t, vis, t->cy, t->cx, COLS);
        else if (m == 1) clear_cells(t, vis, t->cy, 0, t->cx + 1);
        else clear_cells(t, vis, t->cy, 0, COLS);
        break;
    }
    case 'L': case 'M': {                               /* insert / delete lines at the cursor */
        if (t->cy < t->top || t->cy > t->bot) break;
        int save = t->top;
        t->top = t->cy;
        for (int i = 0; i < n; i++) {
            if (c == 'L') scroll_down(t, vis);
            else {
                int full = 0;
                (void)full;
                memmove(t->ch[t->top], t->ch[t->top + 1], (u32)(t->bot - t->top) * COLS);
                memmove(t->at[t->top], t->at[t->top + 1], (u32)(t->bot - t->top) * COLS);
                memset(t->ch[t->bot], ' ', COLS);
                memset(t->at[t->bot], 0, COLS);
                if (vis)
                    for (int y = t->top; y <= t->bot; y++)
                        draw_row(t, y);
            }
        }
        t->top = save;
        t->cx = 0;
        break;
    }
    case 'P': {                                         /* delete characters */
        if (n > COLS - t->cx) n = COLS - t->cx;
        memmove(&t->ch[t->cy][t->cx], &t->ch[t->cy][t->cx + n], (u32)(COLS - t->cx - n));
        memmove(&t->at[t->cy][t->cx], &t->at[t->cy][t->cx + n], (u32)(COLS - t->cx - n));
        for (int x = COLS - n; x < COLS; x++) { t->ch[t->cy][x] = ' '; t->at[t->cy][x] = 0; }
        if (vis) for (int x = t->cx; x < COLS; x++) draw(t, t->cy, x);
        break;
    }
    case '@': {                                         /* insert characters */
        if (n > COLS - t->cx) n = COLS - t->cx;
        memmove(&t->ch[t->cy][t->cx + n], &t->ch[t->cy][t->cx], (u32)(COLS - t->cx - n));
        memmove(&t->at[t->cy][t->cx + n], &t->at[t->cy][t->cx], (u32)(COLS - t->cx - n));
        for (int x = t->cx; x < t->cx + n; x++) { t->ch[t->cy][x] = ' '; t->at[t->cy][x] = 0; }
        if (vis) for (int x = t->cx; x < COLS; x++) draw(t, t->cy, x);
        break;
    }
    case 'X':                                           /* erase characters */
        clear_cells(t, vis, t->cy, t->cx, t->cx + n > COLS ? COLS : t->cx + n);
        break;
    case 'S': for (int i = 0; i < n; i++) scroll_up(t, vis); break;
    case 'T': for (int i = 0; i < n; i++) scroll_down(t, vis); break;
    case 'r': {                                         /* scroll region */
        int tp = arg(t, 0, 1) - 1, bt = arg(t, 1, trows) - 1;
        if (tp >= 0 && bt < trows && tp < bt) { t->top = tp; t->bot = bt; }
        t->cx = t->cy = 0;
        break;
    }
    case 's': t->sx = t->cx; t->sy = t->cy; break;
    case 'u': t->cx = t->sx; t->cy = t->sy; break;
    case 'm':
        if (!t->nparam) t->attr = 0;
        for (int i = 0; i < t->nparam; i++) {
            int p = t->param[i];
            if (p == 0) t->attr = 0;
            else if (p == 1) t->attr |= A_BOLD;
            else if (p == 4) t->attr |= A_UND;
            else if (p == 7) t->attr |= A_INV;
            else if (p == 22) t->attr &= (u8)~A_BOLD;
            else if (p == 24) t->attr &= (u8)~A_UND;
            else if (p == 27) t->attr &= (u8)~A_INV;
            /* colours (30-47, 90-107) are accepted and ignored: one bitplane */
        }
        break;
    case 'h': case 'l':
        if (t->priv && t->nparam && t->param[0] == 25)
            t->curvis = (c == 'h');
        break;
    case 'n':
        if (t->nparam && t->param[0] == 6) {            /* cursor position report */
            char b[16];
            int k = 0;
            b[k++] = 27; b[k++] = '[';
            int r = t->cy + 1, cc = t->cx + 1;
            if (r >= 10) b[k++] = (char)('0' + r / 10);
            b[k++] = (char)('0' + r % 10);
            b[k++] = ';';
            if (cc >= 10) b[k++] = (char)('0' + cc / 10);
            b[k++] = (char)('0' + cc % 10);
            b[k++] = 'R'; b[k] = 0;
            tty_inject(con_cur, b);
        }
        break;
    }
}

/* one character; the caller hides and redraws the cursor around a batch */
static void put1(struct term *t, int vis, u8 ch)
{
    if (t->esc == 1) {                                  /* after ESC */
        t->esc = 0;
        switch (ch) {
        case '[': t->esc = 2; t->nparam = 0; t->priv = 0; t->param[0] = 0; return;
        case '7': t->sx = t->cx; t->sy = t->cy; return;
        case '8': t->cx = t->sx; t->cy = t->sy; t->wrap = 0; return;
        case 'D': linefeed(t, vis); return;
        case 'E': t->cx = 0; linefeed(t, vis); return;
        case 'M':
            if (t->cy == t->top) scroll_down(t, vis); else if (t->cy > 0) t->cy--;
            return;
        case 'c': reset_term(t); if (vis) gfx_clear_text(); return;
        }
        return;
    }
    if (t->esc == 2) {                                  /* CSI: digits ; digits ... final */
        if (ch == '?') { t->priv = 1; return; }
        if (ch >= '0' && ch <= '9') {
            if (!t->nparam) t->nparam = 1;
            t->param[t->nparam - 1] = t->param[t->nparam - 1] * 10 + (ch - '0');
            return;
        }
        if (ch == ';') {
            if (!t->nparam) { t->nparam = 1; t->param[0] = 0; }
            if (t->nparam < 6) { t->param[t->nparam++] = 0; }
            return;
        }
        t->esc = 0;
        if (ch >= 0x40 && ch <= 0x7E)
            csi(t, vis, (char)ch);
        return;
    }
    switch (ch) {
    case 27: t->esc = 1; return;
    case '\n': t->cx = 0; t->wrap = 0; linefeed(t, vis); return;     /* newline = CR+LF on output, as ttys do */
    case '\r': t->cx = 0; t->wrap = 0; return;
    case '\b': if (t->cx) t->cx--; t->wrap = 0; return;
    case '\t': t->cx = (t->cx + 8) & ~7; if (t->cx >= COLS) t->cx = COLS - 1; t->wrap = 0; return;
    case 7: beep(880, 80); return;                      /* bell */
    }
    if (ch < 32)
        return;
    if (t->wrap) {                                      /* deferred wrap, as terminals do */
        t->cx = 0;
        linefeed(t, vis);
        t->wrap = 0;
    }
    t->ch[t->cy][t->cx] = ch;
    t->at[t->cy][t->cx] = t->attr;
    if (vis)
        draw(t, t->cy, t->cx);
    if (t->cx == COLS - 1)
        t->wrap = 1;
    else
        t->cx++;
}

void term_write(int w, const char *s, u32 n)
{
    struct term *t = get(w);
    int vis = (w == con_cur) && gfx_up();
    if (t->back)
        leave_back(t, vis);                             /* output brings the live screen back */
    if (vis)
        cursor(t, 0);
    for (u32 i = 0; i < n; i++)
        put1(t, vis, (u8)s[i]);
    if (vis)
        cursor(t, 1);
}

/* kernel log text onto the visible console (kprintf mirror during boot) */
void term_log(const char *s, u32 n)
{
    term_write(con_cur, s, n);
}

void term_putc(int w, char c)
{
    term_write(w, &c, 1);
}

int term_size(void) { return (trows << 16) | COLS; }

/* is anyone on console w? (a live process whose controlling tty it is) */
int con_in_use(int w)
{
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &procs[i];
        if (p->state != P_FREE && p->state != P_ZOMBIE && p->ctty == w)
            return 1;
    }
    return 0;
}

/* Amiga+Fn or the serial hook: show console w. A console nobody is on asks init for a login there (init waits for
 * that in SYS_wait with WAIT_CONSOLE; called from kwork only, so the wakeup cannot be lost) - like Linux's autovt. */
void con_switch(int w)
{
    if (w < 1 || w > NCONS)
        return;
    if (!con_in_use(w)) {
        con_want |= (u16)(1u << w);
        for (int i = 1; i < NPROC; i++)
            if (procs[i].state == P_WAIT && procs[i].wait_con)
                ready(&procs[i]);
    }
    if (w == con_cur)
        return;
    con_cur = w;
    if (gfx_up()) {
        struct term *t = get(w);
        if (t->back)
            show_back(t);
        else {
            redraw_all(t);
            cursor(t, 1);
        }
    }
}
