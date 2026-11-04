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
 * already holds the bright foreground or background, so redraws and console switches stay right.
 *
 * UTF-8 (DESIGN D28, docs/d28-console-text.md): the bytes written are UTF-8, decoded here, in put1: a pending
 * character is one more parser state (E_UTF), so ASCII pays one test of a byte it already holds. Nothing is decoded
 * inside an escape sequence (a window title's UTF-8 is swallowed with it). A byte that cannot be part of a character
 * is one cell: its Latin-1 glyph (0xA0-0xFF; old Amiga text stays readable), else the box (0x80-0x9F, stored as the
 * byte); never a control; C1 controls written as UTF-8 are ignored. A cell is a 16-bit value: today's byte is its low
 * byte, and a cell above U+00FF has A_HI in `at` and its high byte in `hi`, a plane allocated the first time the
 * console stores such a character (2,560 bytes; the scrollback keeps an 80-byte high row only for the lines that
 * need one, from a pool). 0xFFFE is a character beyond U+FFFF (its width kept, drawn as the box), 0xFFFF the right
 * half (tail) of a wide character. Widths are the kernel's (confont.c, the C library's own tables). In 8-bit mode (a
 * CON_FONT, or the rule CONR_8BIT) nothing is decoded and a cell above U+00FF draws as '?'. */

#define HLINES 200                      /* scrollback lines per console */

/* the escape parser's states (ECMA-48): after ESC; inside CSI; inside a control string (OSC, DCS, APC, PM, SOS:
 * swallowed up to BEL or ESC); after ESC and an intermediate byte (ESC ( B: swallowed up to the final byte) */
#define E_ESC 1
#define E_CSI 2
#define E_STR 3
#define E_ESCI 4
#define E_UTF 5                         /* D28: inside a UTF-8 character (the decoder's state below) */
#define NPAR 16                         /* CSI parameters kept; more are read and ignored */
#define PMAX 9999                       /* a parameter's largest value: more digits keep it there */

#define A_INV 1
#define A_UND 2
#define A_BOLD 4
#define A_HI 8                          /* D28: the cell's high byte is in hi (a value above 0xFF) */
#define V_BIG 0xFFFE                    /* D28: a character beyond U+FFFF */
#define V_TAIL 0xFFFF                   /* D28: the right half of a wide character */
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
    u8 *hi;                             /* D28: [TROWS_MAX][COLS] high bytes of A_HI cells, allocated on first use */
    struct hpool *hp;                   /* D28: the scrollback's high rows */
    struct udec {                       /* D28: the decoder (8 bytes): the bytes held (a lead and its */
        u8 ub[4];                       /* continuations), how many, how many still wanted, the next byte's */
        u8 un, uneed, ulo, uhi;         /* allowed range */
    } u;
};

/* D28: the scrollback's high rows. For each of its lines a slot: 0 none, else an 80-byte row of a pool that grows
 * in chunks of HCHUNK rows (a line pushed with any A_HI cell takes one; its other cells get 0), given back when the
 * line leaves the scrollback. */
#define HCHUNK 16
#define HNCH ((HLINES + HCHUNK - 1) / HCHUNK)
struct hpool {
    u8 slot[HLINES];
    u8 used[(HLINES + 7) / 8];          /* bit r - 1: row r is taken */
    u8 nch;
    u8 *ch[HNCH];
};

static struct term *wsp[NCONS + 1];
int con_cur = 1;
u16 con_want;                           /* bit n: console n was shown with nobody on it: init starts a login there */
static u8 panicked;                     /* D28: a panic prints: nothing is allocated any more */
static u8 nomem_say;                    /* D28: a high plane or row could not be had ('?' shown): said once, after */
static u8 nomem_said;

/* D28: 8-bit mode (a CON_FONT, or CONR_8BIT): no decoding, the keys Latin-1, cells above 0xFF drawn as '?' */
static inline int eight(const struct term *t) { return t->font || (t->rules & CONR_8BIT); }
int term_eight(int con) { return con >= 1 && con <= NCONS && wsp[con] && eight(wsp[con]); }

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

/* D28: a cell above 0xFF, drawn from the stored cells (never from today's widths, so a font change cannot orphan a
 * half): a tail is its lead's right half (if the lead is a cell above 0xFF), else blank; a lead whose next cell is a
 * tail is the left half; in 8-bit mode '?' (a tail blank) */
static void udraw(int row, int x, u32 v, u32 prev, u32 next, u8 a, u8 co, int eb)
{
    a &= (u8)~A_HI;
    if (eb)
        gfx_cell(row, x, v == V_TAIL ? ' ' : '?', a, co);
    else if (v == V_TAIL) {
        if (prev > 0xFF && prev != V_TAIL)
            gfx_ucell(row, x, (u16)prev, a, co, 2);
        else
            gfx_cell(row, x, ' ', a, co);
    } else
        gfx_ucell(row, x, (u16)v, a, co, next == V_TAIL);
}

static inline u32 cellv(const struct term *t, u32 o)       /* a cell's value */
{
    u32 c = ((const u8 *)t->ch)[o];
    return (((const u8 *)t->at)[o] & A_HI) ? (u32)t->hi[o] << 8 | c : c;
}

static __attribute__((noinline)) void draw_hi(struct term *t, int y, int x, u32 o, u8 a)
{
    udraw(y, x, cellv(t, o), x ? cellv(t, o - 1) : 0, x < COLS - 1 ? cellv(t, o + 1) : 0, a, ((u8 *)t->co)[o],
          eight(t));
}

static void drawx(struct term *t, int y, int x, u8 axor)
{
    u32 o = rowx[y] + (u32)x;
    u8 a = ((u8 *)t->at)[o];
    if (a & A_HI)
        draw_hi(t, y, x, o, a ^ axor);
    else
        gfx_cell(y, x, ((u8 *)t->ch)[o], a ^ axor, ((u8 *)t->co)[o]);
}

static void draw(struct term *t, int y, int x) { drawx(t, y, x, 0); }

static void cursor(struct term *t, int on)
{
    if (!t->curvis || t->back)
        return;
    drawx(t, t->cy, t->cx, on ? A_INV : 0);    /* no multiply by COLS per key echoed (audit 2026-10-29 B2-12) */
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

/* D28: the high row of scrollback line h (0: none) */
static u8 *hrow(const struct hpool *hp, int h)
{
    u32 r = hp ? hp->slot[h] : 0;
    return r ? hp->ch[(r - 1) / HCHUNK] + (r - 1) % HCHUNK * COLS : 0;
}

/* a free row of the pool (its number), growing it by a chunk when it is full; 0: no memory. The bits are scanned a
 * byte at a time (audit B3-17: a full byte is one test, not eight) */
_Static_assert(HCHUNK % 8 == 0 && HLINES % 8 == 0, "the pool's bits come in whole bytes");
static u32 hrow_take(struct term *t)
{
    struct hpool *hp = t->hp;
    if (!hp) {
        if (panicked || !(hp = kmalloc(sizeof *hp)))
            return 0;
        memset(hp, 0, sizeof *hp);
        t->hp = hp;
    }
    u32 nb = (u32)hp->nch * (HCHUNK / 8);
    if (nb > sizeof hp->used)
        nb = sizeof hp->used;
    for (u32 i = 0; i < nb; i++)
        if (hp->used[i] != 0xFF) {
            u32 b = 0;
            while (hp->used[i] >> b & 1)
                b++;
            hp->used[i] |= (u8)(1u << b);
            return i * 8 + b + 1;
        }
    if (hp->nch >= HNCH || panicked || !(hp->ch[hp->nch] = kmalloc(HCHUNK * COLS)))
        return 0;
    u32 r = (u32)hp->nch++ * HCHUNK;
    hp->used[r >> 3] |= (u8)(1u << (r & 7));
    return r + 1;
}

/* D28: line hhead of the scrollback, row y's copy (l), gives back its old high row and takes one if it has A_HI cells;
 * a line with high cells takes over the leaving line's row as it is (audit B3-17: no search once the scrollback is
 * full of such lines, as box-drawing output fills it) */
static __attribute__((noinline)) void hist_high(struct term *t, int y, u8 *l, int high)
{
    struct hpool *hp = t->hp;
    u32 r = 0;
    if (hp && hp->slot[t->hhead]) {                     /* the line leaving the scrollback had a row */
        r = hp->slot[t->hhead];
        hp->slot[t->hhead] = 0;
        if (!high) {                                    /* given back */
            r--;
            hp->used[r >> 3] &= (u8)~(1u << (r & 7));
            return;
        }
    }
    if (!high)
        return;
    if (!r)
        r = hrow_take(t);
    if (r) {
        t->hp->slot[t->hhead] = (u8)r;
        u8 *h = hrow(t->hp, t->hhead);
        for (int x = 0; x < COLS; x++)
            h[x] = (t->at[y][x] & A_HI) ? t->hi[rowx[y] + (u32)x] : 0;
    } else {                                            /* no row: the line's high cells are kept as '?' */
        for (int x = 0; x < COLS; x++)
            if (t->at[y][x] & A_HI)
                l[x] = t->ch[y][x] == 0xFF && t->hi[rowx[y] + (u32)x] == 0xFF ? ' ' : '?';
        nomem_say = 1;
    }
}

static void hist_push(struct term *t, int y)
{
    if (!t->hist) {
        if (panicked)
            return;                                     /* nothing allocated while a panic prints */
        t->hist = kmalloc(2 * HLINES * COLS);
        if (!t->hist)
            return;
        t->hn = t->hhead = 0;
    }
    u8 *c = t->hist[HLINES + t->hhead];
    u8 *l = t->hist[t->hhead];
    memcpy(l, t->ch[y], COLS);
    int high = 0;
    for (int x = 0; x < COLS; x++) {                    /* one colour byte: inverse is kept as swapped colours */
        u8 v = t->co[y][x];
        if (t->at[y][x] & A_INV)
            v = (v & 15) == v >> 4 ? 0x70 : (u8)(v << 4 | v >> 4);
        c[x] = v;
        high |= t->at[y][x] & A_HI;
    }
    if (t->hp || high)
        hist_high(t, y, l, high);
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
            for (int x = 0; x < COLS; x++) {
                u32 o = rowx[y] + (u32)x;
                if (t->at[y][x] & A_HI)                 /* D28: a cell above 0xFF, drawn at display row r */
                    udraw(r, x, cellv(t, o), x ? cellv(t, o - 1) : 0, x < COLS - 1 ? cellv(t, o + 1) : 0, t->at[y][x],
                          t->co[y][x], eight(t));
                else if (t->ch[y][x] != ' ' || t->at[y][x] || t->co[y][x] != CO_DEF)
                    gfx_cell(r, x, t->ch[y][x], t->at[y][x], t->co[y][x]);
            }
            continue;
        }
        int h = (t->hhead - t->hn + k + HLINES * 2) % HLINES;
        const u8 *l = t->hist[h], *c = t->hist[HLINES + h], *hr = hrow(t->hp, h);
        for (int x = 0; x < COLS; x++) {
            if (hr && hr[x]) {                          /* D28: a high cell (its row byte is never 0) */
                u32 v = (u32)hr[x] << 8 | l[x];
                u32 pv = x && hr[x - 1] ? (u32)hr[x - 1] << 8 | l[x - 1] : 0;
                u32 nv = x < COLS - 1 && hr[x + 1] ? (u32)hr[x + 1] << 8 | l[x + 1] : 0;
                udraw(r, x, v, pv, nv, 0, c[x], eight(t));
            } else if (l[x] != ' ' || c[x] != CO_DEF)
                gfx_cell(r, x, l[x], 0, c[x]);
        }
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
    if (!gfx_up())
        return;                                         /* no display, or a display server has it (D38) */
    struct term *t = get(con_cur);
    t->changes++;                                       /* what the display shows of it changed (CON_READ) */
    gfx_show(t->font, t->rules & CONR_CGAPAL, eight(t));    /* its font, palette (D20), 8-bit mode (D28) */
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
    if (t->hi)
        memmove(t->hi + rowx[dst], t->hi + rowx[src], (u32)n * COLS);
}

/* D28: is cell x of row y the right half of a wide character? */
static int is_tail(const struct term *t, int y, int x)
{
    u32 o = rowx[y] + (u32)x;
    return (((const u8 *)t->at)[o] & A_HI) && ((const u8 *)t->ch)[o] == 0xFF && t->hi[o] == 0xFF;
}

static void blank1(struct term *t, int vis, int y, int x)  /* a half blanked: a space, its colour kept */
{
    t->ch[y][x] = ' ';
    t->at[y][x] = 0;
    if (vis)
        draw(t, y, x);
}

/* D28: a cut between columns x - 1 and x of row y (a write, an erase or a move begins or ends there): a wide
 * character spanning it loses both halves, as xterm and VTE do */
static __attribute__((noinline)) void cut(struct term *t, int vis, int y, int x)
{
    if (x > 0 && x < COLS && is_tail(t, y, x)) {
        blank1(t, vis, y, x - 1);
        blank1(t, vis, y, x);
    }
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
    if (t->hi) {                                        /* D28: the orphaned halves at the edges */
        cut(t, vis, y, x0);
        cut(t, vis, y, x1);
    }
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
        if (t->hi) {                                    /* D28: the halves cut at both edges, then the high bytes */
            cut(t, vis, t->cy, t->cx);
            cut(t, vis, t->cy, t->cx + n);
            memmove(t->hi + rowx[t->cy] + t->cx, t->hi + rowx[t->cy] + t->cx + n, (u32)(COLS - t->cx - n));
        }
        memmove(&t->ch[t->cy][t->cx], &t->ch[t->cy][t->cx + n], (u32)(COLS - t->cx - n));
        memmove(&t->at[t->cy][t->cx], &t->at[t->cy][t->cx + n], (u32)(COLS - t->cx - n));
        memmove(&t->co[t->cy][t->cx], &t->co[t->cy][t->cx + n], (u32)(COLS - t->cx - n));
        for (int x = COLS - n; x < COLS; x++) { t->ch[t->cy][x] = ' '; t->at[t->cy][x] = 0; t->co[t->cy][x] = bce(t); }
        if (vis) for (int x = t->cx; x < COLS; x++) draw(t, t->cy, x);
        break;
    }
    case '@': {                                         /* insert characters */
        if (n > COLS - t->cx) n = COLS - t->cx;
        if (t->hi) {                                    /* D28: a character split at the cursor, one pushed off */
            cut(t, vis, t->cy, t->cx);
            cut(t, vis, t->cy, COLS - n);
            memmove(t->hi + rowx[t->cy] + t->cx + n, t->hi + rowx[t->cy] + t->cx, (u32)(COLS - t->cx - n));
        }
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

static void put1(struct term *t, int vis, u8 ch);
static void utf_start(struct term *t, int vis, u8 ch);
static void utf_more(struct term *t, int vis, u8 ch);

/* D28: the cell at the cursor is taken by a character of w columns (the deferred wrap done): a wide character it
 * cuts loses both halves */
static __attribute__((noinline)) void unhalf(struct term *t, int vis, int w)
{
    cut(t, vis, t->cy, t->cx);
    cut(t, vis, t->cy, t->cx + w);
}

/* the cursor after a character of w columns: the last column defers the wrap (or wraps at once: CONR_WRAPNOW) */
static void advance(struct term *t, int vis, int w)
{
    if (t->cx + w <= COLS - 1)
        t->cx += w;
    else if (!(t->rules & CONR_WRAPNOW)) {
        t->cx = COLS - 1;
        t->wrap = 1;
    } else {                                            /* CONR_WRAPNOW (D20): ANSI.SYS and the Amiga console */
        t->cx = 0;
        linefeed(t, vis);
    }
}

/* a byte as a cell of its own (D28: a byte that is not part of a character: its Latin-1 glyph, or the box) */
static __attribute__((noinline)) void cell8(struct term *t, int vis, u8 ch)
{
    if (t->wrap) {
        t->cx = 0;
        linefeed(t, vis);
        t->wrap = 0;
    }
    u32 o = rowx[t->cy] + (u32)t->cx;
    if (((u8 *)t->at)[o] & A_HI)
        unhalf(t, vis, 1);
    ((u8 *)t->ch)[o] = ch;
    ((u8 *)t->at)[o] = t->attr;
    ((u8 *)t->co)[o] = t->colour | t->cor;
    if (vis)
        gfx_cell(t->cy, t->cx, ch, t->attr, ((u8 *)t->co)[o]);
    advance(t, vis, 1);
}

/* D28: the cell at grid offset o is the right half of a wide character (the plane exists) */
static inline int tail_at(const struct term *t, u32 o)
{
    return (((const u8 *)t->at)[o] & A_HI) && ((const u8 *)t->ch)[o] == 0xFF && t->hi[o] == 0xFF;
}

/* D28: the high plane, on first use (never while a panic prints); 0: no memory (said once, after the write) */
static int hi_get(struct term *t)
{
    if (t->hi)
        return 1;
    if (!panicked && (t->hi = kmalloc(TROWS_MAX * COLS)))
        return 1;
    nomem_say = 1;
    return 0;
}

/* D28: a decoded character (section 5's putcp): C1 ignored; Latin-1 a byte cell; else its width (0: not stored),
 * the deferred wrap (a wide one at the last column wraps first), the halves it cuts, the store, the draw */
static __attribute__((noinline)) void putcp(struct term *t, int vis, u32 cp)
{
    if (cp < 0xA0)
        return;                                         /* C1 controls written as UTF-8: ignored */
    if (cp <= 0xFF) {
        cell8(t, vis, (u8)cp);
        return;
    }
    int w = con_cwidth(cp);
    if (w <= 0)
        return;                                         /* zero-width: not stored (the program counted 0 too) */
    u32 v = cp > 0xFFFF ? V_BIG : cp >= 0xFFFE ? 0xFFFD : cp;      /* U+FFFE/FFFF are noncharacters: the box */
    if (!hi_get(t)) {                                   /* no memory: '?' (a wide one '?' and a space) */
        cell8(t, vis, '?');
        if (w == 2)
            cell8(t, vis, ' ');
        return;
    }
    if (t->wrap) {
        t->cx = 0;
        linefeed(t, vis);
        t->wrap = 0;
    }
    if (w == 2 && t->cx == COLS - 1) {                  /* a wide character at the last column wraps first */
        t->cx = 0;
        linefeed(t, vis);
    }
    u32 o = rowx[t->cy] + (u32)t->cx, ow = o + (t->cx + w < COLS ? (u32)w : 0);
    if (tail_at(t, o) || tail_at(t, ow))
        unhalf(t, vis, w);                              /* a wide character it cuts loses both halves */
    u8 a = t->attr | A_HI, c = t->colour | t->cor;
    ((u8 *)t->ch)[o] = (u8)v;
    t->hi[o] = (u8)(v >> 8);
    ((u8 *)t->at)[o] = a;
    ((u8 *)t->co)[o] = c;
    if (w == 2) {
        ((u8 *)t->ch)[o + 1] = 0xFF;
        t->hi[o + 1] = 0xFF;
        ((u8 *)t->at)[o + 1] = a;
        ((u8 *)t->co)[o + 1] = c;
    }
    if (vis) {                                          /* (the cells after it hold no tail of it: drawn directly) */
        gfx_ucell(t->cy, t->cx, (u16)v, t->attr, c, w == 2);     /* (never in 8-bit mode: nothing is decoded) */
        if (w == 2)
            draw(t, t->cy, t->cx + 1);
    }
    advance(t, vis, w);
}

/* D28 4: a byte of 0x80 or more outside a sequence, not in 8-bit mode: a lead starts a character (its next byte's
 * range by Unicode's Table 3-7: no overlong forms, no surrogates, nothing above U+10FFFF); any other is a cell */
static __attribute__((noinline)) void utf_start(struct term *t, int vis, u8 ch)
{
    if (ch < 0xC2 || ch > 0xF4) {
        cell8(t, vis, ch);                              /* a lone continuation, C0, C1, F5-FF */
        return;
    }
    t->u.ub[0] = ch;
    t->u.un = 1;
    t->u.uneed = ch < 0xE0 ? 1 : ch < 0xF0 ? 2 : 3;
    t->u.ulo = ch == 0xE0 ? 0xA0 : ch == 0xF0 ? 0x90 : 0x80;
    t->u.uhi = ch == 0xED ? 0x9F : ch == 0xF4 ? 0x8F : 0xBF;
    t->esc = E_UTF;
}

/* D28 4: the next byte of a pending character: in range, held (the last one finishes it); else each byte held is a
 * cell of its own and the breaking byte is taken afresh (a control or ESC acts after those cells) */
static __attribute__((noinline, optimize("O2"))) void utf_more(struct term *t, int vis, u8 ch)
{
    if (ch >= t->u.ulo && ch <= t->u.uhi) {
        t->u.ub[t->u.un++] = ch;
        t->u.ulo = 0x80;
        t->u.uhi = 0xBF;
        if (--t->u.uneed)
            return;
        t->esc = 0;
        const u8 *b = t->u.ub;
        u32 cp = t->u.un == 2 ? (u32)(b[0] & 0x1F) << 6 | (b[1] & 0x3F) :
                 t->u.un == 3 ? (u32)(b[0] & 0x0F) << 12 | (u32)(b[1] & 0x3F) << 6 | (b[2] & 0x3F) :
                 (u32)(b[0] & 0x07) << 18 | (u32)(b[1] & 0x3F) << 12 | (u32)(b[2] & 0x3F) << 6 | (b[3] & 0x3F);
        putcp(t, vis, cp);
        return;
    }
    t->esc = 0;
    for (u32 i = 0; i < t->u.un; i++)
        cell8(t, vis, t->u.ub[i]);
    put1(t, vis, ch);
}

/* the rest of put1: the bytes of an escape sequence and the controls (put1 sends UTF-8 and the other cells straight to
 * their own paths; the same tests stay here, so any byte is right here too) */
static __attribute__((noinline, optimize("O2"))) void put1_more(struct term *t, int vis, u8 ch)
{
    if (t->esc) {
        if (t->esc == E_UTF) {                          /* D28: inside a character (before CAN/SUB: they break it) */
            utf_more(t, vis, ch);
            return;
        }
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
    if (ch >= 0x80 && !eight(t)) {                      /* D28: UTF-8 (in 8-bit mode the byte is the cell) */
        utf_start(t, vis, ch);
        return;
    }
    cell8(t, vis, ch);
}

/* one character; the caller hides and redraws the cursor around a batch. term.c is built -Os (DESIGN D14: room on the
 * RAM floppy); this, every character's path, -O2: a printable ASCII byte outside a sequence (0x20-0x7F) is stored and
 * drawn here, everything else (a deferred wrap, half of a wide character, UTF-8, a sequence) one step away, so the
 * common path keeps its few registers (audit B3-18: about 4% fewer instructions per character) */
_Static_assert(__builtin_offsetof(struct term, ch) == 0 &&
               __builtin_offsetof(struct term, at) == sizeof ((struct term *)0)->ch &&
               __builtin_offsetof(struct term, co) == 2 * sizeof ((struct term *)0)->ch, "put1: ch first, at and co after it");
static __attribute__((optimize("O2"))) void put1(struct term *t, int vis, u8 ch)
{
    if (__builtin_expect(t->esc || (s8)ch < 32 || t->wrap, 0)) {   /* (a deferred wrap, as terminals do: cell8) */
        if (t->esc == E_UTF)                            /* D28: inside a character (before CAN/SUB: they break it) */
            utf_more(t, vis, ch);
        else if (t->esc || ch < 32)
            put1_more(t, vis, ch);
        else if (ch >= 0x80 && !eight(t))               /* D28: UTF-8 (in 8-bit mode the byte is the cell) */
            utf_start(t, vis, ch);
        else
            cell8(t, vis, ch);
        return;
    }
    u8 *c = (u8 *)t + rowx[t->cy] + (u32)t->cx;        /* the cell's byte (ch is first); its attributes and colour
                                                         * follow, a grid apart */
    if (c[sizeof t->ch] & A_HI) {                       /* D28: over half of a wide character: cell8 (unhalf) */
        cell8(t, vis, ch);
        return;
    }
    c[0] = ch;
    c[sizeof t->ch] = t->attr;
    u8 co = t->colour | t->cor;
    c[2 * sizeof t->ch] = co;
    if (vis)
        gfx_cell(t->cy, t->cx, ch, t->attr, co);
    if (t->cx != COLS - 1)
        t->cx++;
    else
        advance(t, vis, 1);                             /* the last column: the wrap deferred, or CONR_WRAPNOW's */
}

/* Draw s; bounded: stop early once two ticks (10-20 ms) have passed while input waits or a better process is ready,
 * and say how many bytes were taken (tty_write then lets kwork and that process run: D20 section 6). D28 4: a piece
 * never ends inside a UTF-8 character: the early stop waits for its end, and a piece of n bytes that ends inside one
 * takes up to max bytes (at most 3 more), so kwork's echo between two pieces cannot break it. */
static u32 write_(int w, const char *s, u32 n, u32 max, int bounded)
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
        if (bounded && (u32)(ticks - t0) >= 2 && (kwork_pending || need_resched) && t->esc != E_UTF)
            break;
    }
    while (i < max && t->esc == E_UTF)
        put1(t, vis, (u8)s[i++]);
    if (vis)
        cursor(t, 1);
    if (nomem_say) {                                    /* D28: said once, after the write (kprintf draws too) */
        nomem_say = 0;
        if (!nomem_said++)
            kprintf("term: no memory for the cells above U+00FF: shown as '?' (said once)\n");
    }
    return i;
}

void term_write(int w, const char *s, u32 n) { write_(w, s, n, n, 0); }

/* a tty write's piece (tty.c): at least one byte, perhaps fewer than n, perhaps up to max (above) */
u32 term_write_some(int w, const char *s, u32 n, u32 max) { return write_(w, s, n, max < n ? n : max, 1); }

/* kernel log text onto the visible console (kprintf mirror during boot). D28: a program's pending UTF-8 character
 * is put aside and back, so a log line in its middle does not break it. */
void term_log(const char *s, u32 n)
{
    struct term *t = wsp[con_cur];
    if (!t && panicked)
        return;                                         /* a console never used: nothing allocated in a panic */
    if (t && t->esc == E_UTF) {
        struct udec u = t->u;
        t->esc = 0;
        term_write(con_cur, s, n);
        if (t->esc == 0) {                              /* the log ended outside any sequence: the character goes on */
            t->u = u;
            t->esc = E_UTF;
        }
        return;
    }
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
    if (gcon_away(w))
        return;                                         /* D38: the display server is asked to let go first */
    con_show(w);
}

/* show console w now: con_switch's end, or (gcon.c) once the display server has let go. The console in graphics
 * mode is its server's to draw (D38 step 2) */
void con_show(int w)
{
    if (w < 1 || w > NCONS)
        return;
    if (wsp[con_cur])
        wsp[con_cur]->changes++;                        /* no longer shown (CON_READ's flag) */
    con_cur = w;
    if (gcon_shown(w))
        return;
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
    if (t->esc == E_UTF)
        t->esc = 0;                                     /* D28: a pending character is dropped with the mode */
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
    if (((old ^ r) & CONR_8BIT) && t->esc == E_UTF)
        t->esc = 0;                                     /* D28: the 8-bit mode set or cleared: nothing pending */
    if (t->con == con_cur && gfx_up()) {
        if (((old ^ r) & CONR_8BIT) && !t->font)
            term_redraw();                              /* D28: cells above 0xFF and 0x80-0x9F look different now */
        else if ((old ^ r) & CONR_CGAPAL)
            gfx_show(t->font, r & CONR_CGAPAL, eight(t));
    }
}

/* ---- DESIGN D24 6.1-6.3: the remote control's ops (root only; nothing allocated: a console never used is -ENXIO) ---- */

/* CON_READ: the grid (not the scrollback view) and its header into the caller's struct uconread and cell buffer;
 * cells of 3 bytes (the stored byte, the attributes, the colour), or (D28, CONRF_CELL4) of 4: the 16-bit value
 * big-endian first */
static long con_read(struct term *t, struct uconread *r, u32 cs)
{
    u32 k = r->ncells / cs, all = (u32)trows * COLS;
    if (k > all)
        k = all;
    if (k && !user_ok_wr(current, r->cells, k * cs))
        return -EFAULT;
    u8 *o = (u8 *)r->cells;
    for (u32 i = 0; i < k; i++, o += cs) {
        u8 a = ((u8 *)t->at)[i], *c = o;
        if (cs == 4)
            *c++ = (a & A_HI) ? t->hi[i] : 0;
        c[0] = ((u8 *)t->ch)[i];
        c[1] = a & (A_INV | A_UND | A_BOLD);
        c[2] = ((u8 *)t->co)[i];
    }
    r->rows = (u16)trows;
    r->cols = COLS;
    r->crow = (u16)t->cy;
    r->ccol = (u16)t->cx;
    r->changes = t->changes;
    r->flags = (u8)((t->con == con_cur ? CONRD_SHOWN : 0) | (t->font ? CONRD_FONT : 0) | (t->back ? CONRD_BACK : 0) |
                    (t->curvis ? CONRD_CURSOR : 0));
    r->csize = (u8)cs;
    r->pad = 0;
    r->size = UCONREAD_FIRST;                           /* the bytes of the struct the kernel filled */
    return (long)(k * cs);
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
    if (op != CON_FONT && op != CON_RULES && (op < CON_READ || op > CON_GACK))
        return -EINVAL;
    if (op == CON_GACK) {                               /* D38: the grab's own descriptor */
        struct file *g = fd < NFD ? current->fd[fd] : 0;
        return g ? gcon_ack(g, a2) : -EBADF;
    }
    int d24 = op >= CON_READ;
    if (d24 && current->uid != 0)
        return -EPERM;
    struct file *f = fd < NFD ? current->fd[fd] : 0;
    if (!f)
        return -EBADF;
    if (f->type != FT_TTY || f->off < 1 || f->off > NCONS)
        return -ENOTTY;
    int con = (int)f->off, own = con == tty_of(current);
    if (op == CON_GRAB)
        return gcon_grab(con, a2);                      /* D38 step 2: root (gcon.c checks) */
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
            return a3 > CONRF_CELL4 ? -EINVAL : con_read(wsp[con], (struct uconread *)a2, a3 == CONRF_CELL4 ? 4 : 3);
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
    if (t->esc == E_UTF)
        t->esc = 0;                                     /* D28: 8-bit mode from now on: nothing pending */
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

/* a panic draws into the shown console: Topaz and no rules, whatever a board set (D20 4.2); no pending UTF-8
 * character, and nothing allocated from now on (D28) */
void term_panic(void)
{
    struct term *t = wsp[con_cur];
    panicked = 1;
    if (t) {
        t->rules = 0;
        t->cor = 0;
        t->wrap = 0;
        if (t->esc == E_UTF)
            t->esc = 0;
    }
    gfx_show(0, 0, 0);
}

/* DESIGN D28 7: SYS_console CON_UFONT, the console's Unicode font for every console (no descriptor; root). The size is
 * bounded before anything is allocated; the checks run on the kernel's own copy (confont.c); one pointer store
 * switches to it; then the old copy is freed, every console's pending character cleared and the shown console drawn
 * again. a2 0 drops the font (every character above U+00FF the box; widths do not change). Kept when its setter
 * ends: it is the system's (term_release leaves it). */
long term_ufont(u32 a2, u32 a3)
{
    if (current->uid != 0)
        return -EPERM;
    if (!gfx_up())
        return -ENODEV;
    u8 *c = 0;
    if (a2) {
        if (a3 > CONF_MAXSIZE || a3 < CONF_HDR + CONF_INDEX)
            return -EINVAL;
        if (!user_ok_rd(current, a2, a3))
            return -EFAULT;
        if (!(c = kmalloc(a3)))
            return -ENOMEM;
        memcpy(c, (const void *)a2, a3);
        if (confont_check(c, a3)) {
            kfree(c);
            return -EINVAL;
        }
    }
    kfree(confont_set(c));
    for (int i = 1; i <= NCONS; i++)
        if (wsp[i]) {
            if (wsp[i]->esc == E_UTF)
                wsp[i]->esc = 0;
            wsp[i]->changes++;
        }
    term_redraw();
    return 0;
}
