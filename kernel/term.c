/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* Virtual consoles, as on Linux: NCONS (10) of them, /dev/tty1 .. /dev/tty10,
 * switched with Amiga+F1..F10. Each has its own text grid (80 columns, all the
 * rows of the screen: 32 on PAL, 25 on NTSC, with attributes and colours), its
 * own scrollback and its own tty. Only the current console is drawn; switching
 * redraws from the grid. The terminal understands the usual ANSI/VT100/xterm
 * sequences (cursor movement, erase, insert/delete line and character, scroll
 * regions, bold/underline/inverse, the 16 colours; 256 and true colours are
 * shown as the nearest of the 16) so ordinary programs and editors work. Erased
 * cells take the current background colour, as on Linux and xterm. A console's
 * grid and scrollback are allocated when it is first used.
 *
 * For BBS terminals (DESIGN D20 4.2-4.5) a console may get a loaded 8-bit font (CON_FONT: each byte one glyph of it)
 * and seven rules (CON_RULES, CONR_*), each set by a program through a descriptor of that console and dropped when its
 * last setter ends. The colour rules act when a cell is stored: the colour byte in the grid (and the scrollback)
 * already holds the bright foreground or background, so redraws and console switches stay right. */

#define HLINES 200                      /* scrollback lines per console */

/* the escape parser's states (ECMA-48): after ESC; inside CSI; inside a control string (OSC, DCS, APC, PM, SOS:
 * swallowed up to BEL or ESC); after ESC and an intermediate byte (ESC ( B: swallowed up to the final byte) */
#define E_ESC 1
#define E_CSI 2
#define E_STR 3
#define E_ESCI 4
#define NPAR 16                         /* CSI parameters kept; more are read and ignored */
#define PMAX 9999                       /* a parameter's largest value: more digits keep it there */

#define A_INV 1
#define A_UND 2
#define A_BOLD 4
#define CO_DEF 0x07                     /* colour byte: foreground in the low nibble, background in the high one */

struct term {
    u8 ch[TROWS_MAX][COLS];
    u8 at[TROWS_MAX][COLS];
    u8 co[TROWS_MAX][COLS];
    int cx, cy, sx, sy;                 /* cursor and its saved position */
    u8 attr, colour;                    /* attributes and colours for new characters */
    u8 sattr, scolour;                  /* saved with the cursor by ESC 7 */
    u8 ckm;                             /* cursor-key mode (DECCKM, ESC[?1h): arrows are sent as ESC O A */
    int top, bot;                       /* scroll region (rows, inclusive) */
    int wrap;                           /* last column written: wrap before the next character */
    int curvis;
    u8 esc, priv, inter;                /* escape sequence parser: state (E_*), private marker, intermediate seen */
    u8 nparam;                          /* parameters seen (up to NPAR are kept) */
    u16 sub;                            /* bit i: parameter i came after ':' (a sub-parameter, 38:5:196) */
    u16 param[NPAR];
    u8 (*hist)[COLS];                   /* scrollback: the oldest line is hist[(hhead - hn) mod HLINES]; its colours */
    int hn, hhead;                      /* are hist[HLINES + the same index] (inverse folded in) */
    int back;                           /* lines scrolled back (0 = live) */
    u8 con;                             /* its console's number: where its answers (ESC[6n) go */
    u8 rules;                           /* CONR_* (D20 4.3) */
    u8 cor, scor;                       /* or-ed into stored colours: 0x08 bright (CONR_BRIGHT's SGR 1), 0x80 iCE's
                                         * bright background (SGR 5); scor saved by ESC 7 */
    struct gfont *font;                 /* a loaded 8-bit font (CON_FONT), 0: Topaz */
    int fowner, rowner;                 /* who set the font, the rules: dropped when that process ends */
    u32 changes;                        /* raised on every write and font or rules change (CON_READ, D24 6.1) */
};

static struct term *wsp[NCONS + 1];
int con_cur = 1;
u16 con_want;                           /* bit n: console n was shown with nobody on it: init starts a login there */

static void reset_term(struct term *t)
{
    memset(t->ch, ' ', sizeof t->ch);
    memset(t->at, 0, sizeof t->at);
    memset(t->co, CO_DEF, sizeof t->co);
    t->cx = t->cy = t->sx = t->sy = 0;
    t->attr = t->sattr = 0;
    t->colour = t->scolour = CO_DEF;
    t->ckm = 0;
    t->top = 0;
    t->bot = trows - 1;
    t->wrap = 0;
    t->curvis = 1;
    t->esc = 0;
    t->cor = t->scor = 0;               /* ESC c keeps the font and the rules (D20 4.2), not their colour flags */
}

static struct term *get(int w)
{
    if (!wsp[w]) {
        wsp[w] = kmalloc(sizeof *wsp[w]);
        if (!wsp[w])
            panic("term: out of memory");
        memset(wsp[w], 0, sizeof *wsp[w]);
        wsp[w]->con = (u8)w;
        reset_term(wsp[w]);
    }
    return wsp[w];
}

/* the colour of an erased cell: the default foreground on the current background */
static u8 bce(struct term *t) { return (u8)(((t->colour | t->cor) & 0xF0) | (CO_DEF & 0x0F)); }

/* Each row's first cell in the grids, as a table: indexing [y][x] costs a 32-bit multiply by COLS on every character
 * (gcc, tuned for the 060, makes even shift-and-add one), up to 44 cycles on a 68020 (DESIGN D28 15) */
#define R4(r) (r) * COLS, ((r) + 1) * COLS, ((r) + 2) * COLS, ((r) + 3) * COLS
static const u16 rowx[TROWS_MAX] = { R4(0), R4(4), R4(8), R4(12), R4(16), R4(20), R4(24), R4(28) };
#undef R4
_Static_assert(TROWS_MAX == 32, "rowx has 32 rows");

static void draw(struct term *t, int y, int x)
{
    u32 o = rowx[y] + (u32)x;
    gfx_cell(y, x, ((u8 *)t->ch)[o], ((u8 *)t->at)[o], ((u8 *)t->co)[o]);
}

static void cursor(struct term *t, int on)
{
    if (!t->curvis || t->back)
        return;
    if (on) {
        u32 o = rowx[t->cy] + (u32)t->cx;   /* no multiply by COLS per key echoed (audit 2026-10-29 B2-12) */
        gfx_cell(t->cy, t->cx, ((u8 *)t->ch)[o], ((u8 *)t->at)[o] ^ A_INV, ((u8 *)t->co)[o]);
    } else
        draw(t, t->cy, t->cx);
}

static void redraw_all(struct term *t)
{
    gfx_clear_text(CO_DEF);
    for (int y = 0; y < trows; y++)
        for (int x = 0; x < COLS; x++)
            if (t->ch[y][x] != ' ' || t->at[y][x] || t->co[y][x] != CO_DEF)
                draw(t, y, x);
}

/* ---- scrollback ---- */

static void hist_push(struct term *t, int y)
{
    if (!t->hist) {
        t->hist = kmalloc(2 * HLINES * COLS);
        if (!t->hist)
            return;
        t->hn = t->hhead = 0;
    }
    u8 *c = t->hist[HLINES + t->hhead];
    memcpy(t->hist[t->hhead], t->ch[y], COLS);
    for (int x = 0; x < COLS; x++) {                    /* one colour byte: inverse is kept as swapped colours */
        u8 v = t->co[y][x];
        if (t->at[y][x] & A_INV)
            v = (v & 15) == v >> 4 ? 0x70 : (u8)(v << 4 | v >> 4);
        c[x] = v;
    }
    t->hhead = (t->hhead + 1) % HLINES;
    if (t->hn < HLINES)
        t->hn++;
}

static void show_back(struct term *t)
{
    gfx_clear_text(CO_DEF);
    for (int r = 0; r < trows; r++) {
        int k = t->hn - t->back + r;                    /* in the composite (scrollback then grid), 0 the oldest */
        if (k >= t->hn) {
            int y = k - t->hn;
            for (int x = 0; x < COLS; x++)
                if (t->ch[y][x] != ' ' || t->at[y][x] || t->co[y][x] != CO_DEF)
                    gfx_cell(r, x, t->ch[y][x], t->at[y][x], t->co[y][x]);
            continue;
        }
        int h = (t->hhead - t->hn + k + HLINES * 2) % HLINES;
        const u8 *l = t->hist[h], *c = t->hist[HLINES + h];
        for (int x = 0; x < COLS; x++)
            if (l[x] != ' ' || c[x] != CO_DEF)
                gfx_cell(r, x, l[x], 0, c[x]);
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
    t->changes++;
    t->back = nb;
    if (nb)
        show_back(t);
    else {
        redraw_all(t);
        cursor(t, 1);
    }
}

void term_redraw(void)
{
    struct term *t = get(con_cur);
    t->changes++;                                       /* what the display shows of it changed (CON_READ) */
    gfx_show(t->font, t->rules & CONR_CGAPAL);          /* its font and palette (D20): a switch, a new depth */
    if (t->back)
        show_back(t);
    else {
        redraw_all(t);
        cursor(t, 1);
    }
}


/* the console on the screen asked for cursor-key mode (kbd.c sends ESC O A for the arrows then) */
int term_ckm(void) { return wsp[con_cur] ? wsp[con_cur]->ckm : 0; }

/* ---- scrolling inside the region ---- */

static void blank_rows(struct term *t, int y, int n)
{
    memset(t->ch[y], ' ', (u32)n * COLS);
    memset(t->at[y], 0, (u32)n * COLS);
    memset(t->co[y], bce(t), (u32)n * COLS);
}

static void move_rows(struct term *t, int dst, int src, int n)
{
    memmove(t->ch[dst], t->ch[src], (u32)n * COLS);
    memmove(t->at[dst], t->at[src], (u32)n * COLS);
    memmove(t->co[dst], t->co[src], (u32)n * COLS);
}

/* rows top..bot move up (dir 1) or down (dir 0) by n; the rows that come in are blank with the background colour.
 * On the screen one blitter copy and one fill. */
static void region_move(struct term *t, int vis, int top, int n, int up)
{
    int h = t->bot - top + 1;
    if (n > h) n = h;
    if (n <= 0) return;
    if (up) {
        move_rows(t, top, top + n, h - n);
        blank_rows(t, t->bot - n + 1, n);
    } else {
        move_rows(t, top + n, top, h - n);
        blank_rows(t, top, n);
    }
    if (vis) {
        if (up) {
            gfx_move_rows(top, top + n, h - n);
            gfx_fill_rows(t->bot - n + 1, n, bce(t));
        } else {
            gfx_move_rows(top + n, top, h - n);
            gfx_fill_rows(top, n, bce(t));
        }
    }
}

static void scroll_up(struct term *t, int vis)          /* the region moves up one line */
{
    if (t->top != 0 || t->bot != trows - 1) {
        region_move(t, vis, t->top, 1, 1);
        return;
    }
    hist_push(t, 0);
    move_rows(t, 0, 1, trows - 1);
    blank_rows(t, trows - 1, 1);
    if (vis)
        gfx_scroll(bce(t));                             /* the Copper pointer moves: no copying */
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
    if (x0 == 0 && x1 == COLS) {
        blank_rows(t, y, 1);
        if (vis)
            gfx_fill_rows(y, 1, bce(t));
        return;
    }
    u8 c = bce(t);
    for (int x = x0; x < x1; x++) {
        t->ch[y][x] = ' ';
        t->at[y][x] = 0;
        t->co[y][x] = c;
        if (vis)
            draw(t, y, x);
    }
}

static void erase_screen(struct term *t, int vis, int mode)
{
    if (mode == 2 || mode == 3) {
        blank_rows(t, 0, trows);
        if (vis)
            gfx_clear_text(bce(t));
        return;
    }
    if (mode == 0) {
        clear_cells(t, vis, t->cy, t->cx, COLS);
        if (t->cy + 1 < trows) {
            blank_rows(t, t->cy + 1, trows - t->cy - 1);
            if (vis)
                gfx_fill_rows(t->cy + 1, trows - t->cy - 1, bce(t));
        }
    } else {
        if (t->cy) {
            blank_rows(t, 0, t->cy);
            if (vis)
                gfx_fill_rows(0, t->cy, bce(t));
        }
        clear_cells(t, vis, t->cy, 0, t->cx + 1);
    }
}

static int arg(struct term *t, int i, int def)
{
    return (i < t->nparam && t->param[i] > 0) ? t->param[i] : def;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* the nearest of the 16 colours (gfx_palette, 12 bits) to an 8-bit r, g, b */
static int nearest(int r, int g, int b)
{
    int best = 0;
    u32 bd = ~0u;
    for (int i = 0; i < 16; i++) {
        u16 p = gfx_palette[i];
        int dr = r - (p >> 8 & 15) * 17, dg = g - (p >> 4 & 15) * 17, db = b - (p & 15) * 17;
        u32 d = (u32)(dr * dr + dg * dg + db * db);
        if (d < bd) bd = d, best = i;
    }
    return best;
}

/* xterm's 256 colours: 0-15 the 16, 16-231 a 6x6x6 cube, 232-255 greys */
static int map256(int n)
{
    static const u8 lev[6] = { 0, 95, 135, 175, 215, 255 };
    if (n < 16) return n;
    if (n < 232) { n -= 16; return nearest(lev[n / 36], lev[n / 6 % 6], lev[n % 6]); }
    if (n < 256) { int v = 8 + 10 * (n - 232); return nearest(v, v, v); }
    return -1;
}

/* SGR (CSI ... m): attributes and colours. 38/48 take 5;n (256 colours) or 2;r;g;b (true colour), also with ':'
 * (38:5:n, 38:2::r:g:b), shown as the nearest of the 16. Bold stays bold (it never brightens the colour) unless the
 * console has CONR_BRIGHT; 5 and 6 are the bright background with CONR_ICE. 2 3 5 8 9 and other codes are ignored. */
static void sgr(struct term *t)
{
    int n = t->nparam ? t->nparam : 1;
    if (!t->nparam) t->param[0] = 0;
    for (int i = 0; i < n; i++) {
        int p = t->param[i];
        if (t->sub >> i & 1)
            continue;                                   /* a sub-parameter nobody took */
        if (p == 0) { t->attr = 0; t->colour = CO_DEF; t->cor = 0; }
        else if (p == 1) {
            if (t->rules & CONR_BRIGHT) t->cor |= 0x08;         /* D20: the bright foreground, not bold */
            else t->attr |= A_BOLD;
        }
        else if ((p == 5 || p == 6) && (t->rules & CONR_ICE)) t->cor |= 0x80;     /* iCE: the bright background */
        else if (p == 25) t->cor &= (u8)~0x80;
        else if (p == 4) t->attr = (i + 1 < n && (t->sub >> (i + 1) & 1) && !t->param[i + 1]) ?
                                   (u8)(t->attr & ~A_UND) : (u8)(t->attr | A_UND);     /* 4:0 is "no underline" */
        else if (p == 7) t->attr |= A_INV;
        else if (p == 22) { t->attr &= (u8)~A_BOLD; t->cor &= (u8)~0x08; }
        else if (p == 24) t->attr &= (u8)~A_UND;
        else if (p == 27) t->attr &= (u8)~A_INV;
        else if (p >= 30 && p <= 37) t->colour = (u8)((t->colour & 0xF0) | (p - 30));
        else if (p == 39) t->colour = (u8)((t->colour & 0xF0) | (CO_DEF & 15));
        else if (p >= 40 && p <= 47) t->colour = (u8)((t->colour & 0x0F) | (p - 40) << 4);
        else if (p == 49) t->colour = (u8)((t->colour & 0x0F) | (CO_DEF & 0xF0));
        else if (p >= 90 && p <= 97) t->colour = (u8)((t->colour & 0xF0) | (p - 90 + 8));
        else if (p >= 100 && p <= 107) t->colour = (u8)((t->colour & 0x0F) | (p - 100 + 8) << 4);
        else if (p == 38 || p == 48) {
            int k = i + 1, c = -1, colon = k < n && (t->sub >> k & 1);
            if (k < n && t->param[k] == 5 && k + 1 < n) {
                c = map256(t->param[k + 1]);
                i = k + 1;
            } else if (k < n && t->param[k] == 2) {
                int r = k + 1;
                if (colon && k + 4 < n && (t->sub >> (k + 4) & 1))
                    r++;                                /* 38:2:<colour space>:r:g:b */
                if (r + 2 < n) {
                    c = nearest(t->param[r] > 255 ? 255 : t->param[r], t->param[r + 1] > 255 ? 255 : t->param[r + 1],
                                t->param[r + 2] > 255 ? 255 : t->param[r + 2]);
                    i = r + 2;
                } else
                    i = n;
            }
            while (i + 1 < n && (t->sub >> (i + 1) & 1))
                i++;                                    /* the rest of a ':' group */
            if (c >= 0)
                t->colour = p == 38 ? (u8)((t->colour & 0xF0) | c) : (u8)((t->colour & 0x0F) | c << 4);
        }
    }
}

static __attribute__((noinline)) void csi(struct term *t, int vis, char c)
{
    int n = arg(t, 0, 1);
    if (t->priv && c != 'h' && c != 'l')
        return;                                         /* private sequences (CSI > c, CSI ? 6 n ...): not ours */
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
    case 'J':
        erase_screen(t, vis, t->nparam ? t->param[0] : 0);
        if ((t->rules & CONR_CLRHOME) && t->nparam && t->param[0] == 2)
            t->cx = t->cy = 0;                          /* D20: as ANSI.SYS and BBS terminals */
        break;
    case 'K': {
        int m = t->nparam ? t->param[0] : 0;
        if (m == 0) clear_cells(t, vis, t->cy, t->cx, COLS);
        else if (m == 1) clear_cells(t, vis, t->cy, 0, t->cx + 1);
        else clear_cells(t, vis, t->cy, 0, COLS);
        break;
    }
    case 'L': case 'M':                                 /* insert / delete lines at the cursor */
        if (t->cy < t->top || t->cy > t->bot) break;
        region_move(t, vis, t->cy, n, c == 'M');
        t->cx = 0;
        break;
    case 'P': {                                         /* delete characters */
        if (n > COLS - t->cx) n = COLS - t->cx;
        memmove(&t->ch[t->cy][t->cx], &t->ch[t->cy][t->cx + n], (u32)(COLS - t->cx - n));
        memmove(&t->at[t->cy][t->cx], &t->at[t->cy][t->cx + n], (u32)(COLS - t->cx - n));
        memmove(&t->co[t->cy][t->cx], &t->co[t->cy][t->cx + n], (u32)(COLS - t->cx - n));
        for (int x = COLS - n; x < COLS; x++) { t->ch[t->cy][x] = ' '; t->at[t->cy][x] = 0; t->co[t->cy][x] = bce(t); }
        if (vis) for (int x = t->cx; x < COLS; x++) draw(t, t->cy, x);
        break;
    }
    case '@': {                                         /* insert characters */
        if (n > COLS - t->cx) n = COLS - t->cx;
        memmove(&t->ch[t->cy][t->cx + n], &t->ch[t->cy][t->cx], (u32)(COLS - t->cx - n));
        memmove(&t->at[t->cy][t->cx + n], &t->at[t->cy][t->cx], (u32)(COLS - t->cx - n));
        memmove(&t->co[t->cy][t->cx + n], &t->co[t->cy][t->cx], (u32)(COLS - t->cx - n));
        for (int x = t->cx; x < t->cx + n; x++) { t->ch[t->cy][x] = ' '; t->at[t->cy][x] = 0; t->co[t->cy][x] = bce(t); }
        if (vis) for (int x = t->cx; x < COLS; x++) draw(t, t->cy, x);
        break;
    }
    case 'X':                                           /* erase characters */
        clear_cells(t, vis, t->cy, t->cx, t->cx + n > COLS ? COLS : t->cx + n);
        break;
    case 'S':                                           /* scroll up: the whole screen into the scrollback, as a */
        if (t->top == 0 && t->bot == trows - 1)         /* line feed at the bottom would */
            for (int i = 0; i < n && i < trows; i++) scroll_up(t, vis);
        else
            region_move(t, vis, t->top, n, 1);
        break;
    case 'T': region_move(t, vis, t->top, n, 0); break;
    case 'r': {                                         /* scroll region */
        int tp = arg(t, 0, 1) - 1, bt = arg(t, 1, trows) - 1;
        if (tp >= 0 && bt < trows && tp < bt) { t->top = tp; t->bot = bt; }
        t->cx = t->cy = 0;
        break;
    }
    case 's': t->sx = t->cx; t->sy = t->cy; break;
    case 'u': t->cx = t->sx; t->cy = t->sy; break;
    case 'm': sgr(t); break;
    case 'h': case 'l':
        if (t->priv == '?')
            for (int i = 0; i < t->nparam; i++) {
                if (t->param[i] == 25) t->curvis = (c == 'h');
                else if (t->param[i] == 1) t->ckm = (c == 'h');     /* DECCKM */
            }
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
            tty_inject(t->con, b);          /* the asking program's console, not the one on screen (audit3 B9) */
        }
        break;
    }
}

/* one character; the caller hides and redraws the cursor around a batch. term.c is built -Os (DESIGN D14: room on the
 * RAM floppy); this, every character's path, -O2, with the escape sequences (csi) kept out of it. */
static __attribute__((optimize("O2"))) void put1(struct term *t, int vis, u8 ch)
{
    if (t->esc) {
        if (ch == 0x18 || ch == 0x1A) {                 /* CAN, SUB: the sequence is cancelled */
            t->esc = 0;
            return;
        }
        if (t->esc == E_STR) {                          /* OSC and other strings (window titles): swallowed */
            if (ch == 7)
                t->esc = 0;
            else if (ch == 27)
                t->esc = E_ESC;                         /* ESC \ ends it (the \ is then an unknown final, swallowed) */
            return;
        }
        if (ch < 0x20 && ch != 27)                      /* a control inside a sequence acts and the sequence goes on */
            goto control;
        if (t->esc == E_ESC) {                          /* after ESC */
            t->esc = 0;
            switch (ch) {
            case 27: t->esc = E_ESC; return;
            case '[': t->esc = E_CSI; t->nparam = 0; t->priv = 0; t->inter = 0; t->sub = 0; t->param[0] = 0; return;
            case ']': case 'P': case '_': case '^': case 'X': t->esc = E_STR; return;
            case '7': t->sx = t->cx; t->sy = t->cy; t->sattr = t->attr; t->scolour = t->colour; t->scor = t->cor; return;
            case '8': t->cx = t->sx; t->cy = t->sy; t->attr = t->sattr; t->colour = t->scolour; t->cor = t->scor; t->wrap = 0; return;
            case 'D': linefeed(t, vis); return;
            case 'E': t->cx = 0; linefeed(t, vis); return;
            case 'M':
                if (t->cy == t->top) region_move(t, vis, t->top, 1, 0); else if (t->cy > 0) t->cy--;
                return;
            case 'c': reset_term(t); if (vis) gfx_clear_text(CO_DEF); return;
            }
            if (ch >= 0x20 && ch <= 0x2F)               /* ESC ( B and the like: swallowed up to the final byte */
                t->esc = E_ESCI;
            return;                                     /* any other: an unknown sequence, swallowed */
        }
        if (t->esc == E_ESCI) {
            if (ch == 27)
                t->esc = E_ESC;
            else if (ch >= 0x30)
                t->esc = 0;
            return;
        }
        /* CSI: parameter bytes 0x30-0x3F, intermediate bytes 0x20-0x2F, a final byte 0x40-0x7E */
        if (ch >= '0' && ch <= '9') {
            if (!t->nparam) t->nparam = 1;
            if (t->nparam <= NPAR) {
                u16 *v = &t->param[t->nparam - 1];
                u32 nv = (u32)*v * 10 + (ch - '0');
                *v = (u16)(nv > PMAX ? PMAX : nv);
            }
            return;
        }
        if (ch == ';' || ch == ':') {
            if (!t->nparam) { t->nparam = 1; t->param[0] = 0; }
            if (t->nparam < NPAR) {
                if (ch == ':') t->sub |= (u16)(1u << t->nparam);
                t->param[t->nparam] = 0;
            }
            if (t->nparam <= NPAR) t->nparam++;
            return;
        }
        if (ch >= '<' && ch <= '?') { t->priv = ch; return; }
        if (ch >= 0x20 && ch <= 0x2F) { t->inter = 1; return; }
        if (ch == 27) { t->esc = E_ESC; return; }
        t->esc = 0;
        if (ch >= 0x40 && ch <= 0x7E && !t->inter) {
            if (t->nparam > NPAR) t->nparam = NPAR;
            csi(t, vis, (char)ch);
        }
        return;                                         /* with an intermediate, or a byte that ends it badly: swallowed */
    }
control:
    switch (ch) {
    case 27: t->esc = E_ESC; return;
    case '\n':                                          /* newline = CR+LF on output, as ttys do; LF alone with */
        if (!(t->rules & CONR_LFONLY))                  /* CONR_LFONLY (D20), as xterm and ANSI.SYS */
            t->cx = 0;
        t->wrap = 0;
        linefeed(t, vis);
        return;
    case '\r': t->cx = 0; t->wrap = 0; return;
    case '\b': if (t->cx) t->cx--; t->wrap = 0; return;
    case '\t': t->cx = (t->cx + 8) & ~7; if (t->cx >= COLS) t->cx = COLS - 1; t->wrap = 0; return;
    case 7: beep(880, 80); return;                      /* bell */
    }
    if (ch < 32 && (t->esc || !ch || !(t->rules & CONR_PCCTRL)))
        return;                                         /* CONR_PCCTRL (D20): the other controls are glyphs */
    if (t->wrap) {                                      /* deferred wrap, as terminals do */
        t->cx = 0;
        linefeed(t, vis);
        t->wrap = 0;
    }
    u32 o = rowx[t->cy] + (u32)t->cx;
    ((u8 *)t->ch)[o] = ch;
    ((u8 *)t->at)[o] = t->attr;
    ((u8 *)t->co)[o] = t->colour | t->cor;
    if (vis)
        gfx_cell(t->cy, t->cx, ch, t->attr, ((u8 *)t->co)[o]);
    if (t->cx != COLS - 1)
        t->cx++;
    else if (!(t->rules & CONR_WRAPNOW))
        t->wrap = 1;
    else {                                              /* CONR_WRAPNOW (D20): ANSI.SYS and the Amiga console */
        t->cx = 0;
        linefeed(t, vis);
    }
}

/* Draw s; bounded: stop early once two ticks (10-20 ms) have passed while input waits or a better process is ready,
 * and say how many bytes were taken (tty_write then lets kwork and that process run: D20 section 6) */
static u32 write_(int w, const char *s, u32 n, int bounded)
{
    struct term *t = get(w);
    int vis = (w == con_cur) && gfx_up();
    u32 i = 0, t0 = ticks;
    t->changes++;
    if (t->back)
        leave_back(t, vis);                             /* output brings the live screen back */
    if (vis)
        cursor(t, 0);
    while (i < n) {
        put1(t, vis, (u8)s[i++]);
        if (bounded && (u32)(ticks - t0) >= 2 && (kwork_pending || need_resched))
            break;
    }
    if (vis)
        cursor(t, 1);
    return i;
}

void term_write(int w, const char *s, u32 n) { write_(w, s, n, 0); }

/* a tty write's piece (tty.c): at least one byte, perhaps fewer than n (above) */
u32 term_write_some(int w, const char *s, u32 n) { return write_(w, s, n, 1); }

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
 * that in SYS_wait with WAIT_CONSOLE) - like Linux's autovt. Called in process context only: from kwork, or from
 * SYS_console's CON_INPUT raw keys in their caller's system call (D24 6.3). Neither can run between init's check and
 * its block (the kernel is not preempted, and kwork runs at none of those points), and ev_post's bit catches the rest,
 * so the wakeup cannot be lost. */
void con_switch(int w)
{
    if (w < 1 || w > NCONS)
        return;
    if (!con_in_use(w)) {
        con_want |= (u16)(1u << w);
        for (int i = 1; i < NPROC; i++)
            if (procs[i].wait_con)
                ev_post(&procs[i], EV_CON);     /* D16: init's wait (WAIT_CONSOLE) */
    }
    if (w == con_cur)
        return;
    if (wsp[con_cur])
        wsp[con_cur]->changes++;                        /* no longer shown (CON_READ's flag) */
    con_cur = w;
    if (gfx_up())
        term_redraw();
}

/* ---- DESIGN D20 4.5: a console's font and rules (SYS_console CON_FONT, CON_RULES) ---- */

/* the font dropped: the console drawn again in Topaz when it is shown (its bytes are Latin-1 again) */
static void font_drop(struct term *t)
{
    if (!t->font)
        return;
    struct gfont *f = t->font;
    t->changes++;
    t->font = 0;
    t->fowner = 0;
    if (t->con == con_cur && gfx_up())
        term_redraw();
    gfx_font_free(f);
}

static void rules_set(struct term *t, u8 r)
{
    u8 old = t->rules;
    t->changes++;
    t->rules = r;
    if (!(r & CONR_BRIGHT)) { t->cor &= (u8)~0x08; t->scor &= (u8)~0x08; }
    if (!(r & CONR_ICE)) { t->cor &= (u8)~0x80; t->scor &= (u8)~0x80; }
    if (((old ^ r) & CONR_CGAPAL) && t->con == con_cur)
        gfx_show(t->font, r & CONR_CGAPAL);
}

/* ---- DESIGN D24 6.1-6.3: the remote control's ops (root only; nothing allocated: a console never used is -ENXIO) ---- */

/* CON_READ: the grid (not the scrollback view) and its header into the caller's struct uconread and cell buffer */
static long con_read(struct term *t, struct uconread *r)
{
    u32 k = r->ncells / 3, all = (u32)trows * COLS;
    if (k > all)
        k = all;
    if (k && !user_ok_wr(current, r->cells, k * 3))
        return -EFAULT;
    u8 *o = (u8 *)r->cells;
    for (u32 i = 0; i < k; i++, o += 3) {
        o[0] = ((u8 *)t->ch)[i];
        o[1] = ((u8 *)t->at)[i];
        o[2] = ((u8 *)t->co)[i];
    }
    r->rows = (u16)trows;
    r->cols = COLS;
    r->crow = (u16)t->cy;
    r->ccol = (u16)t->cx;
    r->changes = t->changes;
    r->flags = (u8)((t->con == con_cur ? CONRD_SHOWN : 0) | (t->font ? CONRD_FONT : 0) | (t->back ? CONRD_BACK : 0) |
                    (t->curvis ? CONRD_CURSOR : 0));
    r->csize = 3;
    r->pad = 0;
    r->size = UCONREAD_FIRST;                           /* the bytes of the struct the kernel filled */
    return (long)(k * 3);
}

/* the caller's struct for CON_READ / CON_LINE: its first word its size (0: the first size, which is all there is so
 * far; smaller -EINVAL), the struct writable -> 0 */
static long ustruct(u32 a, u32 first)
{
    if (!user_ok_wr(current, a, 4))
        return -EFAULT;
    u32 n = *(u32 *)a;
    if (n && n < first)
        return -EINVAL;
    return user_ok_wr(current, a, first) ? 0 : -EFAULT;
}

/* op word: op | (fd + 1) << 8. CON_FONT and CON_RULES: the descriptor must be a console that is the caller's
 * controlling tty, with the caller in its foreground (asking the rules excepted), or root's on any console; a display
 * must be there. CON_READ, CON_LINE, CON_INPUT (D24): root's only, on any console's descriptor. */
long term_conop(u32 op, u32 a2, u32 a3)
{
    u32 fd = (op >> CON_FD_SHIFT) - 1;
    op &= (1u << CON_FD_SHIFT) - 1;
    if (op != CON_FONT && op != CON_RULES && (op < CON_READ || op > CON_INPUT))
        return -EINVAL;
    int d24 = op >= CON_READ;
    if (d24 && current->uid != 0)
        return -EPERM;
    struct file *f = fd < NFD ? current->fd[fd] : 0;
    if (!f)
        return -EBADF;
    if (f->type != FT_TTY || f->off < 1 || f->off > NCONS)
        return -ENOTTY;
    int con = (int)f->off, own = con == tty_of(current);
    if (d24) {
        if (op == CON_INPUT) {
            u32 n = a3 & 0xFFFF, kind = a3 >> 16;
            if (kind > CONI_TEXT || n > CONI_MAX)
                return -EINVAL;
            if (n && !user_ok_rd(current, a2, n))
                return -EFAULT;
            if (kind == CONI_RAW)                       /* the keyboard: the shown console's (any console names it) */
                return kbd_inject((const u8 *)a2, (int)n);
            return wsp[con] ? tty_type(con, (const u8 *)a2, (int)n) : -ENXIO;
        }
        if (op == CON_LINE && !gfx_up())
            return -ENODEV;
        if (!wsp[con] || (op == CON_LINE && con != con_cur))
            return -ENXIO;
        long e = ustruct(op == CON_READ ? a2 : a3, op == CON_READ ? UCONREAD_FIRST : UCONLINE_FIRST);
        if (e)
            return e;
        if (op == CON_READ)
            return a3 ? -EINVAL : con_read(wsp[con], (struct uconread *)a2);
        return gfx_conline(a2, (struct uconline *)a3);
    }
    if (!own && current->uid != 0)
        return -EPERM;
    if (own && !(op == CON_RULES && !a3) && !tty_in_fg(con, current))
        return -EPERM;                                  /* a background job never changes the screen */
    if (!gfx_up())
        return -ENODEV;
    struct term *t = get(con);
    if (op == CON_RULES) {
        if ((a2 | a3) & ~(u32)CONR_ALL)
            return -EINVAL;                             /* unknown bits: later bits keep their meaning */
        u8 old = t->rules;
        if (a3) {
            rules_set(t, (u8)((old & ~a3) | (a2 & a3)));
            t->rowner = t->rules ? current->pid : 0;
        }
        return old;
    }
    if (!a2) {
        font_drop(t);
        return 0;
    }
    if (a3 != 2052)                                     /* PSF1: 4 bytes of header, 256 glyphs of 8 bytes */
        return -EINVAL;
    if (!user_ok_rd(current, a2, a3))
        return -EFAULT;
    const u8 *p = (const u8 *)a2;
    if (p[0] != 0x36 || p[1] != 0x04 || p[2] != 0 || p[3] != 8)
        return -EINVAL;                                 /* magic, mode 0 (256 glyphs, no Unicode table), height 8 */
    struct gfont *nf = gfx_font_new(p + 4);
    if (!nf)
        return -ENOMEM;
    struct gfont *of = t->font;
    t->font = nf;
    t->fowner = current->pid;                           /* one owner: the last setter */
    t->changes++;
    if (con == con_cur)
        term_redraw();                                  /* the grid holds bytes: everything in the new font */
    gfx_font_free(of);
    return 0;
}

void term_release(int pid)
{
    for (int i = 1; i <= NCONS; i++) {
        struct term *t = wsp[i];
        if (!t)
            continue;
        if (t->rowner == pid) {
            rules_set(t, 0);
            t->rowner = 0;
        }
        if (t->fowner == pid)
            font_drop(t);
    }
}

/* a panic draws into the shown console: Topaz and no rules, whatever a board set (D20 4.2) */
void term_panic(void)
{
    struct term *t = wsp[con_cur];
    if (t) {
        t->rules = 0;
        t->cor = 0;
        t->wrap = 0;
    }
    gfx_show(0, 0);
}
