/* SPDX-License-Identifier: GPL-2.0-only */
/* term's cell model (DESIGN D38 5, "one parser, two users"): the console's own kernel/term.c and its widths
 * (kernel/confont.c), compiled here unchanged, as tools/term-test.c compiles them on the PC. term.c draws through the
 * gfx_* calls; here they become drawing orders (term.c's other file, display/term/term.c, sends them through
 * libdisplay), and the few kernel calls it makes are answered by the stand-ins below. This file sees only the
 * kernel's headers; term.c (the program) only libz's: the two meet through model.h. */
int m_cols = 80;                        /* the window's columns: the model's width in use (its rows stay 80 long) */
#define WCOLS m_cols
#include "../../kernel/term.c"
#include "../../kernel/confont.c"
#include "model.h"

int trows = 24;
int gfx_depth = 4;
const u16 gfx_palette[16] = { 0x000, 0xC22, 0x2B2, 0xBB2, 0x45E, 0xB3B, 0x2BB, 0xBBB,
                              0x666, 0xF66, 0x6F6, 0xFF6, 0x88F, 0xF7F, 0x6FF, 0xFFF };   /* kernel/gfx.c's */
volatile u32 ticks;
volatile int kwork_pending;
volatile u8 need_resched;
struct proc procs[NPROC];
static struct proc me;
struct proc *current = &me;

int gfx_up(void) { return 1; }
/* the cells, a run at a time: cells next to each other on a row, in one colour and attribute, are one text order */
void gfx_cell(int row, int col, u8 ch, u8 attr, u8 colour) { m_cell(row, col, ch, attr & 7, colour); }
void gfx_ucell(int row, int col, u16 v, u8 attr, u8 colour, int part)
{
    m_cell(row, col, part == 2 ? ' ' : v, attr & 7, colour);   /* a wide character's halves: its glyph, then blank */
}
/* D28 21: the screen up k rows (term.c fills and draws the rows that come in); what was drawn is shown at once */
int gfx_jump(int k) { if (k > trows) k = trows; if (k < trows) m_move(0, k, trows - k); return k; }
void gfx_done(void) { }
/* a whole row (term.c draws a row whole after a scroll or an erase): a fill in its last cell's background, then the
 * cells up to the last one that is not blank in it, as before D28 21 (one fill order, not a run of spaces) */
void gfx_cells(int row, int x0, int x1, const u8 *ch, const u8 *at, const u8 *co)
{
    if (x0 == 0 && x1 == m_cols) {
        u8 c = co[x1 - 1];
        while (x1 > 0 && ch[x1 - 1] == ' ' && !at[x1 - 1] && !((co[x1 - 1] ^ c) & 0xF0))
            x1--;
        m_fill(row, 1, c);
    }
    for (int x = x0; x < x1; x++)
        m_cell(row, x, ch[x - x0], at[x - x0] & 7, co[x - x0]);
}
void gfx_clear_text(u8 colour) { m_fill(0, trows, colour); }
void gfx_fill_rows(int row, int n, u8 colour) { m_fill(row, n, colour); }
void gfx_move_rows(int dst, int src, int n) { m_move(dst, src, n); }
void gfx_show(const struct gfont *f, int cga, int eight) { (void)f; (void)cga; (void)eight; }
struct gfont *gfx_font_new(const u8 *g) { (void)g; return 0; }
void gfx_font_free(struct gfont *f) { (void)f; }
long gfx_conline(u32 line, struct uconline *d) { (void)line; (void)d; return -ENODEV; }
void *kmalloc(u32 n) { return m_alloc(n); }
void kfree(void *p) { m_free(p); }
void mem_totals(u32 *c, u32 *f) { *c = 0; *f = 1; }   /* a window keeps term.c's 200 lines of scrollback */
void kprintf(const char *f, ...) { (void)f; }
void panic(const char *why) { (void)why; for (;;) ; }
void beep(u32 hz, u32 ms) { (void)hz; (void)ms; m_bell(); }
void tty_inject(int tty, const char *s) { (void)tty; m_answer(s); }   /* ESC[6n's answer: back to the program */
void ev_post(struct proc *p, u32 bits) { (void)p; (void)bits; }
int tty_of(const struct proc *p) { (void)p; return 1; }
int tty_in_fg(int tty, const struct proc *p) { (void)tty; (void)p; return 1; }
int user_ok_rd(const struct proc *p, u32 ptr, u32 len) { (void)p; (void)ptr; (void)len; return 0; }
int user_ok_wr(const struct proc *p, u32 ptr, u32 len) { (void)p; (void)ptr; (void)len; return 0; }
int kbd_inject(const u8 *c, int n) { (void)c; (void)n; return -EINVAL; }
int tty_type(int con, const u8 *b, int n) { (void)con; (void)b; (void)n; return -ENXIO; }
int input_inject(const u8 *r, int n) { (void)r; (void)n; return -EINVAL; }
int gcon_away(int to) { (void)to; return 0; }
int gcon_shown(int con) { (void)con; return 0; }
long gcon_grab(int con, u32 a2) { (void)con; (void)a2; return -EPERM; }
long gcon_ack(struct file *f, u32 op, u32 seq, u32 m) { (void)f; (void)op; (void)seq; (void)m; return -EBADF; }

/* the program's side */
void m_write(const char *s, unsigned n) { term_write(1, s, n); }
void m_redraw(void) { term_redraw(); }
void m_rows(int rows)
{
    if (rows < 4) rows = 4;
    if (rows > TROWS_MAX) rows = TROWS_MAX;
    struct term *t = wsp[1];
    trows = rows;
    if (t) {
        t->top = 0;
        t->bot = rows - 1;
        if (t->cy >= rows) t->cy = rows - 1;
        if (t->sy >= rows) t->sy = rows - 1;
    }
}
void m_setcols(int cols)
{
    if (cols < 8) cols = 8;
    if (cols > COLS) cols = COLS;
    m_cols = cols;
    struct term *t = wsp[1];
    if (t && t->cx >= cols) { t->cx = cols - 1; t->wrap = 0; }
    if (t && t->sx >= cols) t->sx = cols - 1;   /* the saved cursor too (ESC 8, CSI u would put it past the row) */
}
/* the cells x0..x1-1 of display row y as the model has them now (the scrollback view: its rows), the cursor in
 * them inverted: term's presenting (term.c) */
void m_draw_cells(int y, int x0, int x1)
{
    struct term *t = wsp[1];
    if (!t || y < 0 || y >= trows)
        return;
    if (x1 > m_cols) x1 = m_cols;
    if (t->back) {                      /* the scrollback is shown: its own redraw (all of it, rarely) */
        if (y == 0 && x0 == 0) show_back(t);
        return;
    }
    for (int x = x0; x < x1; x++)
        drawx(t, y, x, t->curvis && y == t->cy && x == t->cx ? A_INV : 0);
}
int m_back(void) { return wsp[1] && wsp[1]->back; }
int m_ckm(void) { return term_ckm(); }
void m_scrollback(int d) { term_scroll_view(d); }
