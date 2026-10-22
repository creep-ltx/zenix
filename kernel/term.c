#include "kernel.h"

/* Workspaces: each has its own text grid (80x31) and its own tty. Only
 * the current workspace is drawn; switching redraws from the grid.
 * The bar on top is i3-style: it lists the current workspace plus every
 * workspace that has a program running, nothing else. */

#define NWS 9

struct term {
    u8 ch[TROWS][COLS];
    int cx, cy;
    int esc;                            /* 0 none, 1 after ESC, 2 in CSI */
    int arg;
};

static struct term *wsp[NWS + 1];
int ws_cur = 1;
int ws_spawn_pending[NWS + 1];

static struct term *get(int w)
{
    if (!wsp[w]) {
        wsp[w] = kmalloc(sizeof *wsp[w]);
        if (!wsp[w])
            panic("term: out of memory");
        memset(wsp[w]->ch, ' ', sizeof wsp[w]->ch);
        wsp[w]->cx = wsp[w]->cy = wsp[w]->esc = wsp[w]->arg = 0;
    }
    return wsp[w];
}

static void draw(struct term *t, int y, int x)
{
    gfx_cell(y, x, t->ch[y][x], 1, 0);
}

static void cursor(struct term *t, int on)
{
    if (on)
        gfx_cell(t->cy, t->cx, t->ch[t->cy][t->cx], 0, 1);
    else
        draw(t, t->cy, t->cx);
}

static void newline(struct term *t, int vis)
{
    t->cx = 0;
    if (++t->cy == TROWS) {
        t->cy = TROWS - 1;
        memmove(t->ch[0], t->ch[1], (TROWS - 1) * COLS);
        memset(t->ch[TROWS - 1], ' ', COLS);
        if (vis)
            gfx_scroll();
    }
}

static void redraw_all(struct term *t)
{
    gfx_clear_text();
    for (int y = 0; y < TROWS; y++)
        for (int x = 0; x < COLS; x++)
            if (t->ch[y][x] != ' ')
                draw(t, y, x);
    cursor(t, 1);
}

static void clear_screen(struct term *t, int vis)
{
    memset(t->ch, ' ', sizeof t->ch);
    t->cx = t->cy = 0;
    if (vis) {
        gfx_clear_text();
        cursor(t, 1);
    }
}

/* one character; the caller hides and redraws the cursor around a batch */
static void put1(struct term *t, int vis, char c)
{
    u8 ch = (u8)c;
    if (t->esc == 1) {                  /* after ESC */
        t->esc = (ch == '[') ? 2 : 0;
        t->arg = 0;
    } else if (t->esc == 2) {           /* CSI: digits then a final letter */
        if (ch >= '0' && ch <= '9') {
            t->arg = t->arg * 10 + (ch - '0');
        } else {
            if (ch == 'J' && t->arg == 2)
                clear_screen(t, vis);
            else if (ch == 'H') {
                t->cx = t->cy = 0;
            }
            t->esc = 0;
        }
    } else if (ch == 27) {
        t->esc = 1;
    } else if (ch == '\n') {
        newline(t, vis);
    } else if (ch == '\r') {
        t->cx = 0;
    } else if (ch == '\b') {
        if (t->cx)
            t->cx--;
    } else if (ch == '\t') {
        t->cx = (t->cx + 8) & ~7;
        if (t->cx >= COLS)
            newline(t, vis);
    } else if (ch >= 32) {
        t->ch[t->cy][t->cx] = ch;
        if (vis)
            draw(t, t->cy, t->cx);
        if (++t->cx == COLS)
            newline(t, vis);
    }
}

void term_write(int w, const char *s, u32 n)
{
    struct term *t = get(w);
    int vis = (w == ws_cur) && gfx_up();
    if (vis)
        cursor(t, 0);
    for (u32 i = 0; i < n; i++)
        put1(t, vis, s[i]);
    if (vis)
        cursor(t, 1);
}

void term_putc(int w, char c)
{
    term_write(w, &c, 1);
}

/* ---- bar ---- */

static int in_use(int w)
{
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &procs[i];
        if (p->state != P_FREE && p->state != P_ZOMBIE && p->fd[0] &&
            p->fd[0]->type == FT_TTY && (int)p->fd[0]->off == w)
            return 1;
    }
    return 0;
}

int ws_in_use(int w) { return in_use(w); }

static int put_u(char *b, int n, u32 v, int minw)
{
    char t[10];
    int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i < minw)
        t[i++] = '0';
    while (i--)
        b[n++] = t[i];
    return n;
}

void bar_draw(void)
{
    if (!gfx_up())
        return;
    char b[COLS + 1];
    u8 inv[COLS];
    memset(b, ' ', COLS);
    memset(inv, 0, COLS);
    int n = 0;
    b[n++] = ' ';
    for (int w = 1; w <= NWS; w++) {
        if (w != ws_cur && !in_use(w))
            continue;
        inv[n] = inv[n + 1] = inv[n + 2] = (w == ws_cur);
        b[n + 1] = (char)('0' + w);
        n += 3;
    }
    /* right side: free memory and uptime */
    u32 chip, fast;
    mem_stats(&chip, &fast);
    char r[40];
    int m = 0;
    r[m++] = 'c'; m = put_u(r, m, chip / 1024, 0); r[m++] = 'K'; r[m++] = ' ';
    r[m++] = 'f'; m = put_u(r, m, fast / 1024, 0); r[m++] = 'K'; r[m++] = ' ';
    r[m++] = ' ';
    u32 s = ticks / 100;
    m = put_u(r, m, s / 3600, 2); r[m++] = ':';
    m = put_u(r, m, (s / 60) % 60, 2); r[m++] = ':';
    m = put_u(r, m, s % 60, 2); r[m++] = ' ';
    memcpy(b + COLS - m, r, (u32)m);
    for (int i = 0; i < COLS; i++)
        gfx_bar_cell(i, (u8)b[i], inv[i] ? 0 : 1, inv[i] ? 1 : 0);
}

void ws_switch(int w)
{
    if (w < 1 || w > NWS || w == ws_cur)
        return;
    ws_cur = w;
    if (gfx_up())
        redraw_all(get(w));
    bar_draw();
}

void ws_request_shell(int w)
{
    if (w >= 1 && w <= NWS && !in_use(w)) {
        ws_spawn_pending[w] = 1;
        wake_io();                      /* the session manager waits for this */
    }
}
