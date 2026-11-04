/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the terminal (kernel/term.c) against a model of the display: what term.c draws lands in a cell grid
 * here (the gfx_* calls), so every check compares the grid with what a terminal must show. The escape parser
 * (ECMA-48: unknown sequences, OSC, private markers, ':' sub-parameters, long parameter lists are swallowed without
 * printing anything); the colours (SGR, 256 and true colour mapped to the 16, background-colour erase), region
 * scrolls drawn by blitter moves and fills, the scrollback's colour byte, ESC 7/8, DECCKM; a fuzz after which the
 * screen must always show the grid. DESIGN D28: the UTF-8 decoder against a table of cases, the cells (A_HI, the
 * high plane, the scrollback's high rows), wide and zero-width characters, the 8-bit mode both ways, CON_UFONT's
 * checks (the kernel's own confont.c, against the font tools/mkconfont.py builds), CON_READ's 4-byte cells, a fuzz.
 * Build and run: tools/term-test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../kernel/term.c"
#include "../kernel/confont.c"
#include "../net/telnet/telnet.h"

/* ---- the model display: a cell per position (character, attributes, colours), as the planes would show it ---- */
int trows = 32;
int gfx_depth = 4;
/* a copy of kernel/gfx.c's palette (the nearest-colour checks depend on it); read_palette compares the two */
const u16 gfx_palette[16] = { 0x000, 0xC22, 0x2B2, 0xBB2, 0x45E, 0xB3B, 0x2BB, 0xBBB, 0x666, 0xF66, 0x6F6, 0xFF6, 0x88F, 0xF7F, 0x6FF, 0xFFF };
static int up = 1;
static u8 scr_ch[TROWS_MAX][COLS], scr_at[TROWS_MAX][COLS], scr_co[TROWS_MAX][COLS];
struct gfont { int id; int freed; };    /* DESIGN D20: a loaded font, as an id (see gfx_font_new below) */
static const struct gfont *shown_font;
static u8 scr_font[TROWS_MAX][COLS];    /* the font each cell was drawn in (0: Topaz) */
static u16 scr_v[TROWS_MAX][COLS];      /* D28: what was drawn: the byte (gfx_cell) or the value (gfx_ucell) */
static u8 scr_k[TROWS_MAX][COLS];       /* D28: 0 gfx_cell, 1 gfx_ucell one cell, 2 its left half, 3 its right half */
static int shown_eight;                 /* D28: gfx_show's 8-bit mode */
static long blits, cells;
volatile u32 ticks;                     /* the kernel's tick, input waiting, a better process ready (term_write_some) */
volatile int kwork_pending;
volatile u8 need_resched;
static u32 tick_per_cell;
int gfx_up(void) { return up; }
static void inside(int row, int n, const char *what)
{
    if (row < 0 || n < 0 || row + n > trows) { printf("FAIL: %s outside the screen: rows %d..%d\n", what, row, row + n - 1); exit(1); }
}
void gfx_cell(int row, int col, u8 ch, u8 attr, u8 colour)
{
    inside(row, 1, "gfx_cell");
    if (col < 0 || col >= COLS) { printf("FAIL: gfx_cell outside the screen %d,%d\n", row, col); exit(1); }
    scr_ch[row][col] = ch;
    scr_at[row][col] = attr;
    scr_co[row][col] = colour;
    scr_font[row][col] = (u8)(shown_font ? shown_font->id : 0);
    scr_v[row][col] = ch;
    scr_k[row][col] = 0;
    cells++;
    ticks += tick_per_cell;             /* drawing takes time (term_write_some's bound) */
}
void gfx_ucell(int row, int col, u16 v, u8 attr, u8 colour, int part)
{
    inside(row, 1, "gfx_ucell");
    if (col < 0 || col >= COLS || part < 0 || part > 2) { printf("FAIL: gfx_ucell %d,%d part %d\n", row, col, part); exit(1); }
    if (v <= 0xFF || v == 0xFFFF) { printf("FAIL: gfx_ucell with the value %x\n", v); exit(1); }
    if (attr & A_HI) { printf("FAIL: gfx_ucell given A_HI\n"); exit(1); }
    scr_ch[row][col] = part == 2 ? '>' : '<';
    scr_at[row][col] = attr;
    scr_co[row][col] = colour;
    scr_font[row][col] = 0;
    scr_v[row][col] = v;
    scr_k[row][col] = (u8)(1 + part);
    cells++;
}
/* a filled cell: blank, the background colour (its foreground cannot be seen: kept as the default) */
static void fill(int row, int n, u8 colour)
{
    memset(scr_ch[row], ' ', (size_t)n * COLS);
    memset(scr_at[row], 0, (size_t)n * COLS);
    memset(scr_co[row], (colour & 0xF0) | 7, (size_t)n * COLS);
    for (int r = row; r < row + n; r++)
        for (int x = 0; x < COLS; x++) scr_v[r][x] = ' ', scr_k[r][x] = 0;
}
void gfx_scroll(u8 colour)
{
    memmove(scr_ch[0], scr_ch[1], (size_t)(trows - 1) * COLS);
    memmove(scr_at[0], scr_at[1], (size_t)(trows - 1) * COLS);
    memmove(scr_co[0], scr_co[1], (size_t)(trows - 1) * COLS);
    memmove(scr_v[0], scr_v[1], (size_t)(trows - 1) * sizeof scr_v[0]);
    memmove(scr_k[0], scr_k[1], (size_t)(trows - 1) * COLS);
    fill(trows - 1, 1, colour);
}
void gfx_clear_text(u8 colour) { fill(0, trows, colour); }
void gfx_fill_rows(int row, int n, u8 colour) { inside(row, n, "gfx_fill_rows"); if (n) fill(row, n, colour); blits++; }
void gfx_move_rows(int dst, int src, int n)
{
    inside(dst, n, "gfx_move_rows (to)"); inside(src, n, "gfx_move_rows (from)");
    memmove(scr_ch[dst], scr_ch[src], (size_t)n * COLS);
    memmove(scr_at[dst], scr_at[src], (size_t)n * COLS);
    memmove(scr_co[dst], scr_co[src], (size_t)n * COLS);
    memmove(scr_v[dst], scr_v[src], (size_t)n * sizeof scr_v[0]);
    memmove(scr_k[dst], scr_k[src], (size_t)n * COLS);
    blits++;
}
static void read_palette(void)
{
    FILE *f = fopen("kernel/gfx.c", "r");
    char line[256]; int n = 0, in = 0;
    while (f && fgets(line, sizeof line, f) && n < 16) {
        if (strstr(line, "gfx_palette[16] = {")) { in = 1; continue; }
        for (char *p = line; in && n < 16 && (p = strstr(p, "0x")); p += 2)
            if (gfx_palette[n++] != (u16)strtol(p, 0, 16)) { printf("FAIL: kernel/gfx.c's palette changed: update tools/term-test.c's copy\n"); exit(1); }
    }
    if (f) fclose(f);
    if (n != 16) { printf("FAIL: could not read gfx_palette from kernel/gfx.c\n"); exit(1); }
}

/* ---- the rest of the kernel that term.c calls ---- */
struct proc procs[NPROC];
void ready(struct proc *p) { (void)p; }
void ev_post(struct proc *p, u32 bits) { (void)p; (void)bits; }
static int kmalloc_fail, kmallocs;    /* D28: the next allocations fail (the high plane, the pool, CON_UFONT) */
void *kmalloc(u32 n) { kmallocs++; return kmalloc_fail ? 0 : malloc(n); }
void kfree(void *p) { free(p); }
static char klog[256];
void kprintf(const char *f, ...) { strncat(klog, f, sizeof klog - strlen(klog) - 1); }
void panic(const char *why) { printf("FAIL: panic %s\n", why); exit(1); }
static int bells;
void beep(u32 hz, u32 ms) { (void)hz; (void)ms; bells++; }
static char injected[64];
static int injected_to;
void tty_inject(int tty, const char *s) { injected_to = tty; strncat(injected, s, sizeof injected - strlen(injected) - 1); }

/* DESIGN D20: the fonts (an id each, so a drawn cell says in whose glyphs), the shown font and palette, the caller */
static struct gfont fontpool[64];
static int nfonts, font_nomem, live_fonts;
static int shown_cga, shows;
struct gfont *gfx_font_new(const u8 *g)
{
    if (font_nomem || nfonts >= 64) return 0;
    struct gfont *f = &fontpool[nfonts];
    f->id = g[8 * 'A'] ? g[8 * 'A'] : nfonts + 1;       /* the test's fonts carry their id in glyph A's first line */
    nfonts++;
    live_fonts++;
    return f;
}
void gfx_font_free(struct gfont *f)
{
    if (!f) return;
    if (f->freed) { printf("FAIL: a font freed twice\n"); exit(1); }
    if (shown_font == f) { printf("FAIL: the shown font freed\n"); exit(1); }
    f->freed = 1;
    live_fonts--;
}
void gfx_show(const struct gfont *f, int cga, int eight)
{
    if (f && f->freed) { printf("FAIL: a freed font shown\n"); exit(1); }
    shown_font = f; shown_cga = cga != 0; shown_eight = eight != 0; shows++;
}
static struct proc me;
struct proc *current = &me;
static int fg_ok = 1, uok = 1;
int tty_of(const struct proc *p) { return p->ctty; }
int tty_in_fg(int tty, const struct proc *p) { (void)tty; (void)p; return fg_ok; }
int user_ok_rd(const struct proc *p, u32 ptr, u32 len) { (void)p; (void)ptr; (void)len; return uok; }
int user_ok_wr(const struct proc *p, u32 ptr, u32 len) { (void)p; (void)ptr; (void)len; return uok; }
/* D24 6.2-6.3's other halves (gfx.c, kbd.c, tty.c), as models: what term_conop hands them */
static int raw_n, text_con, text_n, line_n;
int kbd_inject(const u8 *c, int n) { (void)c; raw_n = n; return n; }
int tty_type(int con, const u8 *b, int n) { (void)b; text_con = con; text_n = n; return n; }
int input_inject(const u8 *r, int n) { (void)r; (void)n; return -EINVAL; }   /* D38: the mouse (CONI_MOUSE) */
/* DESIGN D38 step 2 (gcon.c): no console is ever grabbed here */
int gcon_away(int to) { (void)to; return 0; }
int gcon_shown(int con) { (void)con; return 0; }
long gcon_grab(int con, u32 a2) { (void)con; (void)a2; return -EPERM; }
long gcon_ack(struct file *f, u32 seq) { (void)f; (void)seq; return -EBADF; }
long gfx_conline(u32 line, struct uconline *d) { (void)d; line_n = (int)line; return 640; }

static int bad;
#define EXPECT_TRUE(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); bad++; } else printf("ok:   %s\n", msg); } while (0)

static void out(const char *s) { term_write(1, s, (u32)strlen(s)); }
static void outn(const char *s, u32 n) { term_write(1, s, n); }
/* the screen's row r as text, trailing blanks removed */
static const char *row(int r)
{
    static char b[COLS + 1];
    memcpy(b, scr_ch[r], COLS); b[COLS] = 0;
    for (int i = COLS - 1; i >= 0 && b[i] == ' '; i--) b[i] = 0;
    return b;
}
static void reset(void) { out("\033c"); gfx_clear_text(7); }
static struct term *T(void) { return wsp[1]; }
/* the screen shows the grid: every cell as the grid has it (a blank cell may be a fill: then only its background
 * counts), the cursor's cell drawn inverse */
static u32 gv(int y, int x) { struct term *t = T(); return (t->at[y][x] & A_HI) ? (u32)t->hi[y * COLS + x] << 8 | t->ch[y][x] : t->ch[y][x]; }
/* D28: a cell above 0xFF as it must be drawn: (kind, value) from the grid (section 5's drawing rules) */
static int hi_ok(int y, int x, u8 at)
{
    struct term *t = T();
    u32 v = gv(y, x), k = scr_k[y][x], d = scr_v[y][x];
    at &= (u8)~A_HI;
    if (scr_at[y][x] != at || scr_co[y][x] != t->co[y][x]) { printf("      cell %d,%d (U+%04X): attr %x colour %02x, the grid %x %02x\n", y, x, v, scr_at[y][x], scr_co[y][x], at, t->co[y][x]); return 0; }
    if (eight(t)) {
        if (k != 0 || d != (v == 0xFFFF ? ' ' : '?')) { printf("      cell %d,%d (U+%04X) in 8-bit mode: kind %u value %x\n", y, x, v, k, d); return 0; }
        return 1;
    }
    if (v == 0xFFFF) {
        u32 p = x ? gv(y, x - 1) : 0;
        int lead = p > 0xFF && p != 0xFFFF;
        if (lead ? k != 3 || d != p : k != 0 || d != ' ') { printf("      cell %d,%d: a tail drawn as kind %u value %x (lead %x)\n", y, x, k, d, p); return 0; }
        return 1;
    }
    u32 want = x < COLS - 1 && gv(y, x + 1) == 0xFFFF ? 2 : 1;
    if (k != want || d != v) { printf("      cell %d,%d: U+%04X drawn as kind %u value %x, wanted kind %u\n", y, x, v, k, d, want); return 0; }
    return 1;
}
static int screen_is_grid(void)
{
    struct term *t = T();
    for (int y = 0; y < trows; y++)
        for (int x = 0; x < COLS; x++) {
            int cur = t->curvis && y == t->cy && x == t->cx;
            u8 at = (u8)(t->at[y][x] ^ (cur ? A_INV : 0));
            if (t->at[y][x] & A_HI) {
                if (!hi_ok(y, x, at)) return 0;
                continue;
            }
            if (scr_k[y][x]) { printf("      cell %d,%d: drawn as U+%04X, the grid has the byte %02x\n", y, x, scr_v[y][x], t->ch[y][x]); return 0; }
            if (scr_ch[y][x] != t->ch[y][x]) { printf("      cell %d,%d: '%c', the grid has '%c'\n", y, x, scr_ch[y][x], t->ch[y][x]); return 0; }
            if (t->ch[y][x] == ' ' && !at && !scr_at[y][x]) {
                if ((scr_co[y][x] & 0xF0) != (t->co[y][x] & 0xF0)) { printf("      cell %d,%d: background %x, the grid %x\n", y, x, scr_co[y][x] >> 4, t->co[y][x] >> 4); return 0; }
                continue;
            }
            if (scr_at[y][x] != at || scr_co[y][x] != t->co[y][x]) { printf("      cell %d,%d: attr %x colour %02x, the grid %x %02x\n", y, x, scr_at[y][x], scr_co[y][x], at, t->co[y][x]); return 0; }
        }
    return 1;
}
static u8 co_at(int y, int x) { return T()->co[y][x]; }
/* does the screen show only `want` on row 0 and nothing else (the cursor is drawn inverse: that is allowed) */
static int only(const char *want)
{
    if (strcmp(row(0), want)) { printf("      row 0: '%s', wanted '%s'\n", row(0), want); return 0; }
    for (int r = 1; r < trows; r++) if (row(r)[0]) { printf("      row %d: '%s'\n", r, row(r)); return 0; }
    return 1;
}

/* ---- DESIGN D28 ---- */
static long sys_like_op4_desc(void) { return term_conop(CON_UFONT | 2u << CON_FD_SHIFT, 0, 0); }
#define OP(op, fd) ((u32)(op) | (u32)((fd) + 1) << CON_FD_SHIFT)
static u8 *psf;
#define PSF(id) (psf[4 + 8 * 'A'] = (id), (u32)(unsigned long)psf)
/* write in, then row 0's cells from column 0 must be want[0..n) and the cursor at column n (unless n < 0) */
static int cells_are(const char *in, int len, const u32 *want, int n, const char *what)
{
    reset();
    outn(in, (u32)len);
    int ok = 1;
    for (int i = 0; i < (n < 0 ? -n : n); i++)
        if (gv(0, i) != want[i]) { printf("      %s: cell %d is %x, wanted %x\n", what, i, gv(0, i), want[i]); ok = 0; }
    if (n >= 0 && (T()->cy != 0 || T()->cx != n)) { printf("      %s: the cursor at %d,%d\n", what, T()->cy, T()->cx); ok = 0; }
    if (!screen_is_grid()) ok = 0;
    EXPECT_TRUE(ok, what);
    return ok;
}
#define CASE(in, what, ...) do { static const u32 w_[] = { __VA_ARGS__ }; cells_are(in, (int)sizeof(in) - 1, w_, (int)(sizeof w_ / sizeof w_[0]), what); } while (0)
static int no_orphan(void)
{
    for (int y = 0; y < trows; y++)
        for (int x = 0; x < COLS; x++)
            if (gv(y, x) == 0xFFFF && (x == 0 || gv(y, x - 1) <= 0xFF || gv(y, x - 1) == 0xFFFF)) {
                printf("      a tail without its lead at %d,%d\n", y, x); return 0;
            }
    return 1;
}
static u8 *map32(size_t n)
{
    u8 *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) { printf("FAIL: mmap\n"); exit(1); }
    return p;
}
static void d28(void)
{
    panicked = 0;                       /* the D20 tests' term_panic */
    /* the decoder (section 4) */
    CASE("\xc2\xa0x", "U+00A0 (2 bytes): a Latin-1 cell", 0xA0, 'x');
    CASE("\xc2\x80\xc2\x9b" "x", "U+0080 and U+009B (C1 written as UTF-8): ignored", 'x');
    CASE("\xdf\xbf", "U+07FF: the last 2-byte character", 0x7FF);
    CASE("\xe0\xa0\x80", "U+0800: the first 3-byte character", 0x800);
    CASE("\xef\xbf\xbd", "U+FFFD", 0xFFFD);
    CASE("\xef\xbf\xbe\xef\xbf\xbf", "U+FFFE and U+FFFF (noncharacters): stored as U+FFFD", 0xFFFD, 0xFFFD);
    CASE("\xf0\x90\x80\x80", "U+10000 (4 bytes, width 1): one cell beyond the BMP", 0xFFFE);
    CASE("\xf4\x8f\xbf\xbf", "U+10FFFF: the last character", 0xFFFE);
    CASE("\xf0\x9f\x98\x80", "U+1F600 (an emoji, wide): two cells, the value beyond the BMP and a tail", 0xFFFE, 0xFFFF);
    CASE("\x80\xbf\xc0\xc1\xf5\xff", "lone continuations, C0, C1, F5-FF: one cell each", 0x80, 0xBF, 0xC0, 0xC1, 0xF5, 0xFF);
    CASE("\xc0\x80", "C0 80 (overlong): two cells", 0xC0, 0x80);
    CASE("\xe0\x80\x80", "E0 80 (overlong): three cells", 0xE0, 0x80, 0x80);
    CASE("\xed\xa0\x80", "ED A0 (a surrogate): three cells", 0xED, 0xA0, 0x80);
    CASE("\xf0\x80\x80\x80", "F0 80 (overlong): four cells", 0xF0, 0x80, 0x80, 0x80);
    CASE("\xf4\x90\x80\x80", "F4 90 (above U+10FFFF): four cells", 0xF4, 0x90, 0x80, 0x80);
    CASE("\xc0\x9b", "C0 9B: two cells (never a control)", 0xC0, 0x9B);
    CASE("\xe2\x94\x41", "a sequence broken by ASCII: its bytes as cells, then the letter", 0xE2, 0x94, 'A');
    CASE("\xe5\xe4\xf6" "x", "Latin-1 text (\xe5\xe4\xf6 as bytes): readable, one cell a byte", 0xE5, 0xE4, 0xF6, 'x');
    CASE("\xe2\x94\x80\xe2\x94\x82", "U+2500 U+2502", 0x2500, 0x2502);
    reset(); outn("\xe2\x94", 2); outn("\x80", 1);
    EXPECT_TRUE(gv(0, 0) == 0x2500 && T()->cx == 1, "a character split between two writes is finished by the next");
    reset(); outn("\xe2\nA", 3);
    EXPECT_TRUE(gv(0, 0) == 0xE2 && T()->cy == 1 && gv(1, 0) == 'A', "a sequence broken by a control: the cell, then the control acts");
    reset(); outn("\xe2\033[31mX", 7);
    EXPECT_TRUE(gv(0, 0) == 0xE2 && gv(0, 1) == 'X' && co_at(0, 1) == 0x01, "a sequence broken by ESC: the cell, then the escape sequence acts");
    reset(); out("\033]0;\xe2\x9c\xb3 Claude Code\007A\033]2;\xc3\xa5\033\\B");
    EXPECT_TRUE(only("AB") && T()->esc == 0, "UTF-8 inside an OSC title: swallowed with it");
    reset(); out("\033[3\xe2\x94\x80mA");
    EXPECT_TRUE(gv(0, 0) == 0x94 && gv(0, 1) == 0x80 && gv(0, 2) == 'm' && gv(0, 3) == 'A' && T()->attr == 0,
                "a byte of 0x80 or more ends a CSI and is swallowed, as before (the bytes after it are cells)");
    reset(); outn("\xe2\x94", 2); term_log("log\n", 4); outn("\x80", 1);
    EXPECT_TRUE(!strcmp(row(0), "log") && gv(1, 0) == 0x2500, "the kernel log in the middle of a character does not break it");
    reset(); outn("\xe2\x94", 2); out("\033c");
    EXPECT_TRUE(T()->esc == 0, "ESC c clears a pending character");
    reset(); out("e\xcc\x81\xe2\x80\x8bx");
    EXPECT_TRUE(gv(0, 0) == 'e' && gv(0, 1) == 'x' && T()->cx == 2, "zero-width characters (U+0301, U+200B): not stored, no column");
    /* term_write_some: a piece never ends inside a character */
    reset(); tick_per_cell = 5; kwork_pending = 1;
    u32 got = term_write_some(1, "a\xe2\x94\x80" "bc", 1, 6);
    EXPECT_TRUE(got == 1, "an early stop before a character: one byte");
    got = term_write_some(1, "\xe2\x94\x80" "bc", 1, 5);
    EXPECT_TRUE(got == 3 && gv(0, 1) == 0x2500, "a piece that ends inside a character takes its rest (up to max)");
    kwork_pending = 0; tick_per_cell = 0;
    reset(); got = term_write_some(1, "x\xe2\x94", 3, 3);
    EXPECT_TRUE(got == 3 && T()->esc == E_UTF, "... a write that ends inside one leaves it pending (the next write finishes it)");
    out("\x80");
    /* cells (section 5) */
    reset(); out("\xe2\x94\xa0\xe2\x80\xa0");                 /* U+2520 low byte 0x20, U+2020 */
    con_switch(2); con_switch(1);
    EXPECT_TRUE(gv(0, 0) == 0x2520 && gv(0, 1) == 0x2020 && scr_k[0][0] == 1 && scr_v[0][0] == 0x2520 && screen_is_grid(),
                "U+2520 and U+2020 (low bytes 0x20) survive a console switch's redraw");
    for (int i = 0; i < 40; i++) out("\n");
    term_scroll_view(9);                                /* 40 line feeds from row 0: 9 lines went to the scrollback */
    EXPECT_TRUE(scr_k[0][0] == 1 && scr_v[0][0] == 0x2520 && scr_v[0][1] == 0x2020, "... and the scrollback");
    term_scroll_view(-9);
    reset(); out("\033[5;1H\xe2\x94\x80\xe4\xb8\x80");
    term_scroll_view(1);                                /* one line back: rows 1.. show the live grid's rows 0.. */
    EXPECT_TRUE(scr_k[5][0] == 1 && scr_v[5][0] == 0x2500 && scr_k[5][1] == 2 && scr_k[5][2] == 3,
                "the scrollback view's live rows draw cells above 0xFF and wide halves (review finding 1)");
    term_scroll_view(-1);
    reset(); out("\xe2\x94\x80\xe4\xb8\x80"); out("\033c");
    EXPECT_TRUE(!(T()->at[0][0] & A_HI) && gv(0, 0) == ' ' && screen_is_grid(), "ESC c after Unicode: no stale cell");
    /* the plane on first use; the '?' when it cannot be had */
    struct term *t = T();
    free(t->hi); t->hi = 0;
    reset(); out("abc\xc3\xa5");
    EXPECT_TRUE(!t->hi, "Latin-1 text allocates no high plane");
    kmalloc_fail = 1; klog[0] = 0;
    out("\xe2\x94\x80\xe4\xb8\x80z");
    EXPECT_TRUE(!t->hi && gv(0, 4) == '?' && gv(0, 5) == '?' && gv(0, 6) == ' ' && gv(0, 7) == 'z' && strstr(klog, "no memory"),
                "no memory for the high plane: '?' (a wide one '?' and a space), said on the log");
    kmalloc_fail = 0;
    out("\xe2\x94\x80");
    EXPECT_TRUE(t->hi && gv(0, 8) == 0x2500, "... the plane comes with the next character that needs it");
    /* wide characters (section 5) */
    CASE("\xe4\xb8\x80", "a wide character (U+4E00) at column 0: two cells, the cursor two on", 0x4E00, 0xFFFF);
    reset(); out("\033[1;79H\xe4\xb8\x80");
    EXPECT_TRUE(gv(0, 78) == 0x4E00 && gv(0, 79) == 0xFFFF && T()->cx == 79 && T()->wrap && screen_is_grid(),
                "a wide character at column 78 fills 78-79, the wrap pending");
    out("x"); EXPECT_TRUE(gv(1, 0) == 'x', "... the next character on the next row");
    reset(); out("\033[1;80Hq\033[1;80H\xe4\xb8\x80");
    EXPECT_TRUE(gv(0, 79) == 'q' && gv(1, 0) == 0x4E00 && gv(1, 1) == 0xFFFF && T()->cy == 1 && T()->cx == 2,
                "at column 79 a wide character wraps first (cell 79 left as it is)");
    reset(); term_conop(OP(CON_RULES, 1), CONR_WRAPNOW, CONR_ALL);
    out("\033[1;79H\xe4\xb8\x80");
    EXPECT_TRUE(gv(0, 78) == 0x4E00 && T()->cy == 1 && T()->cx == 0, "CONR_WRAPNOW: a wide character at column 78 wraps at once");
    out("\033[1;80H\xe4\xb8\x80");
    EXPECT_TRUE(gv(1, 0) == 0x4E00 && T()->cy == 1 && T()->cx == 2, "CONR_WRAPNOW: at column 79 it goes to the next row");
    term_conop(OP(CON_RULES, 1), 0, CONR_ALL);
    reset(); out("\xe4\xb8\x80\033[1;2Ha");
    EXPECT_TRUE(gv(0, 0) == ' ' && gv(0, 1) == 'a' && screen_is_grid() && no_orphan(), "writing over the right half blanks the left");
    reset(); out("\xe4\xb8\x80\033[1;1Hb");
    EXPECT_TRUE(gv(0, 0) == 'b' && gv(0, 1) == ' ' && no_orphan(), "writing over the left half blanks the right");
    reset(); out("a\xe4\xb8\x80\033[1;2H\xe4\xb8\x81");
    EXPECT_TRUE(gv(0, 1) == 0x4E01 && gv(0, 2) == 0xFFFF && no_orphan(), "a wide character over a wide one");
    reset(); out("\xe4\xb8\x80\xe4\xb8\x80\033[1;2H\xe4\xb8\x81");
    EXPECT_TRUE(gv(0, 0) == ' ' && gv(0, 1) == 0x4E01 && gv(0, 2) == 0xFFFF && gv(0, 3) == ' ' && no_orphan() && screen_is_grid(),
                "a wide character over two halves blanks both orphans");
    reset(); out("\xe4\xb8\x80\033[1;2H\033[K");
    EXPECT_TRUE(gv(0, 0) == ' ' && no_orphan(), "CSI K from the right half blanks the left");
    reset(); out("\xe4\xb8\x80\033[1;1H\033[X");
    EXPECT_TRUE(gv(0, 1) == ' ' && no_orphan(), "CSI X over the left half blanks the right");
    reset(); out("ab\xe4\xb8\x80\033[1;1H\033[1J");
    EXPECT_TRUE(gv(0, 2) == 0x4E00 && no_orphan(), "CSI 1 J up to column 0 leaves a character after it");
    reset(); out("ab\xe4\xb8\x80\033[1;3H\033[1K");
    EXPECT_TRUE(gv(0, 3) == ' ' && no_orphan(), "CSI 1 K up to the left half blanks the right");
    reset(); out("a\xe4\xb8\x80z\033[1;3H\033[@");
    EXPECT_TRUE(gv(0, 1) == ' ' && gv(0, 2) == ' ' && gv(0, 3) == ' ' && gv(0, 4) == 'z' && no_orphan() && screen_is_grid(),
                "CSI @ at the right half: the character split, both halves blank");
    reset(); out("a\xe4\xb8\x80z\033[1;2H\033[@");
    EXPECT_TRUE(gv(0, 2) == 0x4E00 && gv(0, 3) == 0xFFFF && gv(0, 4) == 'z' && no_orphan() && screen_is_grid(),
                "CSI @ at the left half moves the whole character");
    reset(); out("\033[1;79H\xe4\xb8\x80\033[1;1H\033[@");
    EXPECT_TRUE(gv(0, 78) == ' ' && gv(0, 79) == ' ' && no_orphan(), "CSI @ pushing a tail off the row blanks its lead");
    reset(); out("a\xe4\xb8\x80z\033[1;3H\033[P");
    EXPECT_TRUE(gv(0, 1) == ' ' && gv(0, 2) == 'z' && no_orphan() && screen_is_grid(), "CSI P at the right half: both halves go");
    reset(); out("ab\xe4\xb8\x80z\033[1;2H\033[2P");
    EXPECT_TRUE(gv(0, 1) == ' ' && gv(0, 2) == 'z' && no_orphan(), "CSI 2 P ending on a left half: the orphaned tail blank");
    reset(); out("\033[3;5r\033[5;1H\xe4\xb8\x80\n\n");
    EXPECT_TRUE(gv(2, 0) == 0x4E00 && gv(2, 1) == 0xFFFF && no_orphan() && screen_is_grid(), "region scrolls keep a row's cells together");
    out("\033[r");
    reset(); out("\xe4\xb8\x80");
    EXPECT_TRUE(scr_k[0][0] == 2 && scr_k[0][1] == 3 && scr_v[0][1] == 0x4E00, "drawing: the lead the left half, the tail the right half (from the stored tail)");
    out("\033[1;2H");
    EXPECT_TRUE(scr_k[0][1] == 3 && (scr_at[0][1] & A_INV), "the cursor on a tail is drawn there");
    /* the scrollback's pool */
    reset();
    struct hpool *hp0 = t->hp;
    for (int i = 0; i < 200; i++) out(i % 3 ? "plain\n" : "\xe2\x94\x80 box\n");
    int rows = 0; for (int i = 0; i < 25; i++) rows += __builtin_popcount(t->hp->used[i]);
    EXPECT_TRUE(t->hp && rows >= 50 && rows <= 70, "the scrollback: a high row only for the lines with such characters");
    for (int i = 0; i < 400; i++) out("plain\n");
    rows = 0; for (int i = 0; i < 25; i++) rows += __builtin_popcount(t->hp->used[i]);
    EXPECT_TRUE(rows == 0, "... given back when the lines leave the scrollback");
    for (int i = 0; i < 240; i++) out("\xe2\x94\x80\n");      /* the screen's 31 plain rows first */
    rows = 0; for (int i = 0; i < 25; i++) rows += __builtin_popcount(t->hp->used[i]);
    EXPECT_TRUE(rows == 200 && t->hp->nch == HNCH, "... at most 200 rows (the worst case)");
    (void)hp0;
    /* audit B3-17: with the scrollback full of such lines each new one takes over the leaving line's row: still 200
     * rows, no two lines sharing one, and every line's own characters (U+2500 + i % 64, oldest to newest in order) */
    for (int i = 0; i < 500; i++) { char b[8] = { (char)0xe2, (char)0x94, (char)(0x80 + i % 64), '\n', 0 }; out(b); }
    {
        int ok = t->hn == HLINES, prev = -1;
        u8 seen[HLINES + 1] = { 0 };
        rows = 0; for (int i = 0; i < 25; i++) rows += __builtin_popcount(t->hp->used[i]);
        for (int k = 0; k < HLINES && ok; k++) {
            int h = (t->hhead - t->hn + k + 2 * HLINES) % HLINES, r = t->hp->slot[h];
            const u8 *hr = hrow(t->hp, h);
            ok = r && !seen[r]++ && hr && hr[0] == 0x25 && t->hist[h][0] < 64 &&
                 (prev < 0 || t->hist[h][0] == (prev + 1) % 64);
            prev = t->hist[h][0];
        }
        EXPECT_TRUE(ok && rows == 200, "... a full scrollback hands each leaving row to the new line, each line its own");
    }
    /* one line in three (3 does not divide 200: the leaving lines' rows are given back and taken again by a search) */
    for (int i = 0; i < 1000; i++) out(i % 3 ? "plain\n" : "\xe2\x94\x82\n");
    {
        int used = 0, ok = 1;
        u8 seen[HLINES + 1] = { 0 };
        for (int h = 0; h < HLINES; h++) {
            int r = t->hp->slot[h];
            if (r) { used++; ok &= !seen[r]++ && (t->hp->used[(r - 1) >> 3] >> ((r - 1) & 7) & 1); }
        }
        rows = 0; for (int i = 0; i < 25; i++) rows += __builtin_popcount(t->hp->used[i]);
        EXPECT_TRUE(ok && used == rows && rows >= 60 && rows <= 74, "... rows given back are found again (a byte at a time), none shared");
    }
    for (int i = 0; i < 40; i++) out("\n");
    for (int i = 0; i < 200; i++) out("\n");
    { int n = t->hp->nch; for (int i = 0; i < n; i++) { free(t->hp->ch[i]); t->hp->ch[i] = 0; } t->hp->nch = 0; memset(t->hp->used, 0, sizeof t->hp->used);
      memset(t->hp->slot, 0, sizeof t->hp->slot); }
    kmalloc_fail = 1; klog[0] = 0; nomem_said = 0;
    reset(); out("\xe2\x94\x80\xe4\xb8\x80x\n"); for (int i = 0; i < 31; i++) out("\n");   /* one line pushed */
    kmalloc_fail = 0;
    term_scroll_view(1);
    EXPECT_TRUE(!strncmp(row(0), "?? x", 4) && strstr(klog, "no memory"), "no row for a scrollback line: its high cells kept as '?'");
    term_scroll_view(-1);
    /* the 8-bit mode (section 7): CONR_8BIT and CON_FONT */
    reset(); out("\xe2\x94\x80\xe4\xb8\x80");
    term_conop(OP(CON_RULES, 1), CONR_8BIT, CONR_ALL);
    EXPECT_TRUE(shown_eight && screen_is_grid() && scr_v[0][0] == '?' && scr_v[0][2] == ' ', "CONR_8BIT: cells above 0xFF drawn as '?', unchanged");
    out("\xe2\x94\x80\x85");
    EXPECT_TRUE(gv(0, 3) == 0xE2 && gv(0, 4) == 0x94 && gv(0, 5) == 0x80 && gv(0, 6) == 0x85, "CONR_8BIT: no decoding, every byte a cell");
    term_release(50);
    EXPECT_TRUE(!(T()->rules & CONR_8BIT) && !shown_eight && gv(0, 0) == 0x2500 && screen_is_grid(),
                "CONR_8BIT dropped at its setter's end: the characters shown again");
    psf[0] = 0x36; psf[1] = 0x04; psf[2] = 0; psf[3] = 8;     /* (the D24 tests used the buffer) */
    outn("\xe2", 1);
    term_conop(OP(CON_FONT, 1), PSF(16), 2052);
    EXPECT_TRUE(T()->esc == 0 && shown_eight && screen_is_grid() && scr_v[0][0] == '?',
                "a CON_FONT is 8-bit mode too (a pending character cleared)");
    outn("\xc3\xa5", 2);
    EXPECT_TRUE(gv(0, 7) == 0xC3 && gv(0, 8) == 0xA5 && T()->esc == 0, "... no decoding");
    term_conop(OP(CON_FONT, 1), 0, 0);
    EXPECT_TRUE(!shown_eight && screen_is_grid() && scr_k[0][0] == 1 && scr_v[0][0] == 0x2500, "dropping the font: Unicode again");
    outn("\xe2", 1); term_conop(OP(CON_RULES, 1), CONR_8BIT, CONR_ALL);
    EXPECT_TRUE(T()->esc == 0, "setting CONR_8BIT clears a pending character");
    term_release(50);
    /* CON_UFONT (section 7): the font tools/mkconfont.py builds, each check */
    FILE *ff = fopen("build/zenix-8x8.zcf", "rb");
    u8 *zf = map32(CONF_MAXSIZE + 64);
    long zn = ff ? (long)fread(zf, 1, CONF_MAXSIZE + 64, ff) : 0;
    if (ff) fclose(ff);
    EXPECT_TRUE(zn > 544, "the default font (build/zenix-8x8.zcf) read");
    u8 *zc = map32(CONF_MAXSIZE + 64);
    #define ZOK(n) (memcpy(zc, zf, (size_t)zn), term_ufont((u32)(unsigned long)zc, (u32)(n)))
    me.uid = 1000;
    EXPECT_TRUE(ZOK(zn) == -EPERM && !cf_index, "CON_UFONT: not root -EPERM");
    me.uid = 0;
    up = 0; EXPECT_TRUE(ZOK(zn) == -ENODEV, "CON_UFONT: no display -ENODEV"); up = 1;
    int k0 = kmallocs;
    EXPECT_TRUE(term_ufont((u32)(unsigned long)zc, CONF_MAXSIZE + 1) == -EINVAL && kmallocs == k0,
                "CON_UFONT: a size above 98,848: -EINVAL before any allocation");
    uok = 0; EXPECT_TRUE(ZOK(zn) == -EFAULT && !cf_index, "CON_UFONT: the data outside the caller -EFAULT"); uok = 1;
    kmalloc_fail = 1; EXPECT_TRUE(ZOK(zn) == -ENOMEM && !cf_index, "CON_UFONT: no memory -ENOMEM, nothing changes"); kmalloc_fail = 0;
    EXPECT_TRUE(sys_like_op4_desc() == -EINVAL, "op 4 with a descriptor in the op word: -EINVAL");
    static const struct { int off; u8 val; const char *what; } mut[] = {
        { 0, 'X', "magic" }, { 5, 31, "header size" }, { 7, 1, "flags" }, { 8, 9, "glyph width" }, { 9, 16, "glyph height" },
        { 10, 0, "glyph count" }, { 15, 1, "reserved" }, { 19, 0, "total size" }, { 20, 1, "reserved bytes" },
        { 31, 1, "reserved byte 31" }, { 32, 1, "index byte 0" }, { 33, 1, "index byte 1" }, { 40, 250, "an index byte above the count" } };
    for (unsigned i = 0; i < sizeof mut / sizeof mut[0]; i++) {
        memcpy(zc, zf, (size_t)zn);
        zc[mut[i].off] = mut[i].val;
        if (mut[i].off == 10) zc[11] = 2;               /* 2 glyphs */
        char m[96]; snprintf(m, sizeof m, "CON_UFONT: a bad %s: -EINVAL", mut[i].what);
        EXPECT_TRUE(term_ufont((u32)(unsigned long)zc, (u32)zn) == -EINVAL && !cf_index, m);
    }
    memcpy(zc, zf, (size_t)zn); zc[12] = 1;             /* 256 half-pages */
    EXPECT_TRUE(term_ufont((u32)(unsigned long)zc, (u32)zn) == -EINVAL, "CON_UFONT: 256 half-pages: -EINVAL");
    memcpy(zc, zf, (size_t)zn); zc[CONF_HDR + CONF_INDEX + 1] = 5; zc[CONF_HDR + CONF_INDEX] = 0;
    EXPECT_TRUE(term_ufont((u32)(unsigned long)zc, (u32)zn) == -EINVAL, "CON_UFONT: an entry 1-31: -EINVAL");
    memcpy(zc, zf, (size_t)zn); zc[CONF_HDR + CONF_INDEX] = 0x7F; zc[CONF_HDR + CONF_INDEX + 1] = 0xFF;
    EXPECT_TRUE(term_ufont((u32)(unsigned long)zc, (u32)zn) == -EINVAL, "CON_UFONT: an entry past the glyphs: -EINVAL");
    EXPECT_TRUE(ZOK(zn - 8) == -EINVAL, "CON_UFONT: a size that is not the file's: -EINVAL");
    {
        int used = -1, free_ = -1;
        for (int i = 2; i < 512; i++) { if (zf[CONF_HDR + i] && used < 0) used = i; if (!zf[CONF_HDR + i] && free_ < 0) free_ = i; }
        memcpy(zc, zf, (size_t)zn); zc[CONF_HDR + free_] = zc[CONF_HDR + used];
        EXPECT_TRUE(term_ufont((u32)(unsigned long)zc, (u32)zn) == -EINVAL && !cf_index,
                    "CON_UFONT: two index bytes naming one half-page: -EINVAL (review finding 2)");
        memcpy(zc, zf, (size_t)zn); zc[CONF_HDR + CONF_INDEX] = 0; zc[CONF_HDR + CONF_INDEX + 1] = 0x85;
        EXPECT_TRUE(term_ufont((u32)(unsigned long)zc, (u32)zn) == -EINVAL, "CON_UFONT: an entry 0x7F-0x9F (the box's bytes): -EINVAL");
    }
    reset(); out("\xe2\x94\x80\xe2\x8f\xba\xc5\x82\xe4\xb8\x80");
    outn("\xe2", 1);
    EXPECT_TRUE(ZOK(zn) == 0 && cf_index && T()->esc == 0 && screen_is_grid(), "CON_UFONT: loaded, nothing pending, the screen redrawn");
    EXPECT_TRUE(con_cwidth(0x2500) == 1 && con_cwidth(0x23FA) == 1 && con_cwidth(0x23F3) == 2 && con_cwidth(0x0301) == 0 &&
                con_cwidth(0x4E00) == 2 && con_cwidth(0xFF21) == 2, "the width classes in the entries agree with the widths");
    int wbad = 0;
    for (u32 c = 0x100; c < 0x10000 && wbad < 3; c++)
        if (!(c >= 0xD800 && c <= 0xDFFF) && con_cwidth(c) != con_width(c)) { printf("      U+%04X: %d, %d\n", c, con_cwidth(c), con_width(c)); wbad++; }
    EXPECT_TRUE(!wbad, "... for every code point of the BMP");
    term_release(0); term_release(50);
    EXPECT_TRUE(cf_index != 0, "the font is kept at its setter's end");
    EXPECT_TRUE(term_ufont(0, 0) == 0 && !cf_index && screen_is_grid() && con_cwidth(0x4E00) == 2, "a null pointer drops it; widths stay");
    EXPECT_TRUE(ZOK(zn) == 0, "loaded again");
    /* CON_READ's 4-byte cells */
    {
        struct uconread *r = (struct uconread *)zc;
        u8 *cells4 = map32(16384);
        reset(); out("a\xe2\x94\x80\xe4\xb8\x80\xc3\xa5\033[7mb");
        memset(r, 0, sizeof *r); r->size = sizeof *r; r->cells = (u32)(unsigned long)cells4; r->ncells = 16384;
        long n = term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, CONRF_CELL4);
        const u8 *c = cells4;
        EXPECT_TRUE(n == trows * COLS * 4 && r->csize == 4 && c[0] == 0 && c[1] == 'a' && c[4] == 0x25 && c[5] == 0x00 &&
                    c[6] == 0 && c[8] == 0x4E && c[9] == 0 && c[12] == 0xFF && c[13] == 0xFF && c[16] == 0 && c[17] == 0xE5 &&
                    c[21] == 'b' && c[22] == CONA_INV, "CON_READ a3 1: 4-byte cells (the value big-endian, CONA_*, colour)");
        r->ncells = 16384;
        n = term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, 0);
        EXPECT_TRUE(n == trows * COLS * 3 && cells4[3] == 0x00 && cells4[4] == 0 && cells4[6] == 0x00 && cells4[7] == 0,
                    "CON_READ a3 0: 3-byte cells as before (the low byte; A_HI never shown)");
    }
    me.uid = 1000;
    /* audit B3-18: put1's printable path and the cells it sends one step away */
    reset(); out("\033[1;80Hq\x7f");
    EXPECT_TRUE(gv(0, 79) == 'q' && gv(1, 0) == 0x7F && T()->cy == 1 && T()->cx == 1 && screen_is_grid(),
                "ASCII at the last column defers the wrap; DEL is a cell");
    reset(); term_conop(OP(CON_RULES, 1), CONR_8BIT, CONR_ALL); out("\033[1;80H\xe5\xe4x");
    EXPECT_TRUE(gv(0, 79) == 0xE5 && gv(1, 0) == 0xE4 && gv(1, 1) == 'x' && screen_is_grid(),
                "8-bit mode: a byte above 0x7F at the last column defers the wrap like ASCII");
    term_conop(OP(CON_RULES, 1), 0, CONR_ALL);
    reset(); out("\xe4\xb8\x80\033[1;2Hz");
    EXPECT_TRUE(gv(0, 0) == ' ' && gv(0, 1) == 'z' && no_orphan() && screen_is_grid(), "ASCII over a right half: both halves go");
    /* a fuzz: random bytes split anywhere, font loads and drops, rules on and off */
    reset();
    srand(28);
    int fuzz_ok = 1;
    static const char *const ub[] = { "\xe2\x94\x80", "\xe4\xb8\x80", "\xf0\x9f\x98\x80", "\xcc\x81", "\xc3\xa5", "\033[@",
        "\033[P", "\033[2P", "\033[3@", "\033[K", "\033[1K", "\033[X", "\033[2X", "\033[J", "\033[1;79H", "\033[1;80H",
        "\033[5;1H", "\033[L", "\033[M", "\033[S", "\033[T", "\033[3;9r", "\033[r", "\b", "\r", "\n", "a", "\xe2\x8f\xba" };
    for (int i = 0; i < 300000 && fuzz_ok; i++) {
        int k = rand() % 40;
        if (k < 28) out(ub[k]);
        else if (k < 36) { u8 b = (u8)rand(); outn((const char *)&b, 1); }
        else if (k == 36 && i % 50 == 0) { me.uid = 0; if (rand() & 1) ZOK(zn); else term_ufont(0, 0); me.uid = 1000; }
        else if (k == 37 && i % 30 == 0) term_conop(OP(CON_RULES, 1), (rand() & 1) ? CONR_8BIT : 0, CONR_8BIT);
        else if (k == 38 && i % 40 == 0) term_conop(OP(CON_RULES, 1), (rand() & 1) ? CONR_WRAPNOW : 0, CONR_WRAPNOW);
        else if (k == 39 && i % 70 == 0) { term_scroll_view(rand() % 30); term_scroll_view(-200); }
        if (i % 61 == 0 && T()->esc != E_UTF && !T()->esc && (!screen_is_grid() || !no_orphan())) { fuzz_ok = 0; printf("      after piece %d\n", i); }
    }
    EXPECT_TRUE(fuzz_ok, "300000 random pieces of UTF-8, bad bytes, edits, font and mode changes: the screen shows the grid, no tail without its lead");
    term_release(50);
}

int main(void)
{
    read_palette();
    gfx_clear_text(7);
    reset(); out("ab\033[999;999Hc\033[Hx");
    EXPECT_TRUE(scr_ch[0][0] == 'x' && scr_ch[trows - 1][COLS - 1] == 'c', "cursor addressing, clamped to the screen");

    reset(); out("A\033[>4;1mB\033[?1049hC\033[=5uD\033[<0;1;2MZ");
    EXPECT_TRUE(only("ABCDZ"), "private markers > ? = <: swallowed whole, nothing printed");
    EXPECT_TRUE(wsp[1]->attr == 0, "... and CSI > 4;1 m is not SGR (no bold)");

    reset(); out("A\033]0;a window title\007B\033]2;title\033\\C\033]8;;http://x\033\\D");
    EXPECT_TRUE(only("ABCD"), "OSC ended by BEL or ESC \\: swallowed");

    reset(); out("A\033[1 qB\033[!pC\033[2$pD\033[0\"qE");
    EXPECT_TRUE(only("ABCDE"), "sequences with intermediate bytes: swallowed");

    reset(); out("A\033[38:5:196mB\033[38:2::255:0:0mC\033[4:3mD");
    EXPECT_TRUE(only("ABCD"), "':' sub-parameters: accepted, nothing printed");

    reset(); out("A\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25mB");
    EXPECT_TRUE(only("AB"), "25 parameters: the sequence is read whole");
    reset(); out("A\033[99999999999999999999999999CB");
    EXPECT_TRUE(scr_ch[0][0] == 'A' && scr_ch[0][COLS - 1] == 'B', "a huge parameter is clamped (no overflow): CUF goes to the edge");

    reset(); out("A\033(B\033)0\033#8\033%GB\033=\033>C");
    EXPECT_TRUE(only("ABC"), "ESC ( B, ESC # 8, ESC = and other short escapes: swallowed");

    reset(); out("A\033[1;2\030B\033]0;x\032C");
    EXPECT_TRUE(only("ABC"), "CAN and SUB cancel a sequence");

    reset(); out("AXY\033[\r2CB");
    EXPECT_TRUE(only("AXB"), "a control inside CSI acts (CR) and the sequence goes on (CUF 2)");

    reset(); out("A\033[5XB\033[ZC\033[1;2;3yD");
    EXPECT_TRUE(scr_ch[0][0] == 'A' && scr_ch[0][1] == 'B' && strchr(row(0), 'C') && strchr(row(0), 'D') &&
                !strchr(row(0), 'Z') && !strchr(row(0), 'y'), "unknown finals (Z, y): swallowed");

    reset(); { char b[3] = { 'A', 7, 'B' }; int b0 = bells; outn(b, 3); EXPECT_TRUE(only("AB") && bells == b0 + 1, "BEL rings, prints nothing"); }

    reset(); out("\033[6n");
    EXPECT_TRUE(!strcmp(injected, "\033[1;1R") && injected_to == 1, "ESC[6n answers the cursor position");
    injected[0] = 0; injected_to = 0;
    term_write(3, "ab\033[6n", 6);                    /* a program on console 3 asks while console 1 is shown */
    EXPECT_TRUE(!strcmp(injected, "\033[1;3R") && injected_to == 3,
                "... a program on a background console gets the answer on its own console, not the one on screen (audit3 B9)");
    injected[0] = 0; out("\033[?6n");
    EXPECT_TRUE(!injected[0], "... ESC[?6n (private) does not");


    /* ---- a cell stored again as it is is not drawn again (owner 2026-11-04: flicker while typing) ---- */
    reset(); out("\033[32mzenix\033[0m:/# ls -l");
    { long c0 = cells, b0 = blits;
      out("\r\033[32mzenix\033[0m:/# ls -l\033[K");
      EXPECT_TRUE(cells - c0 == 2 && blits == b0, "a line written again as it is, ESC[K over blank cells: only the cursor drawn (off, on)");
      c0 = cells; out("\r\033[32mzenix\033[0m:/# ls -x\033[K");
      EXPECT_TRUE(cells - c0 == 3 && strstr(row(0), "ls -x"), "... one character changed: that cell and the cursor");
      c0 = cells; out("\r\033[31mzenix");
      EXPECT_TRUE(cells - c0 == 7 && co_at(0, 0) == 0x01, "... the same characters in another colour: drawn");
      out("\033[m\033[3;1H"); b0 = blits; out("\033[2K\033[2K");
      EXPECT_TRUE(blits == b0, "ESC[2K on a row already blank in that colour: no fill");
      out("\033[44m\033[2K");
      EXPECT_TRUE(blits == b0 + 1 && co_at(2, 5) == 0x47, "... in another background colour: filled"); }

    /* ---- colours (DESIGN D14 step 2) ---- */
    reset(); out("\033[31mA\033[42mB\033[91mC\033[104mD\033[39mE\033[49mF\033[0mG");
    EXPECT_TRUE(co_at(0, 0) == 0x01 && co_at(0, 1) == 0x21 && co_at(0, 2) == 0x29 && co_at(0, 3) == 0xC9 &&
                co_at(0, 4) == 0xC7 && co_at(0, 5) == 0x07 && co_at(0, 6) == 0x07, "SGR 30-37 40-47 90-97 100-107 39 49 0");
    out("\033[33;44mH\033[mI");
    EXPECT_TRUE(co_at(0, 7) == 0x43 && co_at(0, 8) == 0x07 && T()->attr == 0, "a bare ESC[m resets colours and attributes");
    out("\033[1;35mJ"); EXPECT_TRUE(co_at(0, 9) == 0x05 && (T()->at[0][9] & A_BOLD), "1 is bold and leaves the colour alone");
    out("\033[22;2;3;5;8;9mK"); EXPECT_TRUE(co_at(0, 10) == 0x05 && !(T()->at[0][10] & A_BOLD), "22 ends bold; 2 3 5 8 9 are ignored");
    out("\033c\033[38;5;196mA\033[38;5;46mB\033[48;5;21mC\033[38;5;244mD\033[38;5;9mE\033[48;5;255mF");
    EXPECT_TRUE(co_at(0, 0) == 0x01 && co_at(0, 1) == 0x02 && (co_at(0, 2) >> 4) == 4 && (co_at(0, 3) & 15) == 8 &&
                (co_at(0, 4) & 15) == 9 && (co_at(0, 5) >> 4) == 15, "38;5;n and 48;5;n: the 256 colours map to the nearest of the 16");
    out("\033c\033[38;2;255;0;0mA\033[48;2;0;0;0;38;2;255;255;255mB\033[38:2::0:255:0mC\033[38:5:196mD\033[38:2:0:0:255mE");
    EXPECT_TRUE(co_at(0, 0) == 0x01 && co_at(0, 1) == 0x0F && (co_at(0, 2) & 15) == 2 && (co_at(0, 3) & 15) == 1 &&
                (co_at(0, 4) & 15) == 4, "true colour 38;2;r;g;b, 48;2, the ':' forms (with and without a colour space)");
    out("\033c\033[38;5;300mA\033[38;2;1mB\033[38;5mC\033[38;7;31mD");
    EXPECT_TRUE(only("ABCD") && co_at(0, 0) == 0x07, "bad or short 38 sequences change nothing and print nothing");
    out("\033c\033[38;5;196;1mA"); EXPECT_TRUE(co_at(0, 0) == 0x01 && (T()->at[0][0] & A_BOLD), "parameters after 38;5;n still count");
    out("\033c\033[4:3mA\033[4:0mB"); EXPECT_TRUE((T()->at[0][0] & A_UND) && !(T()->at[0][1] & A_UND), "4:3 underlines, 4:0 does not");

    reset(); out("\033[32;45m\0337\033[0m\033[5;5H\0338X");
    EXPECT_TRUE(scr_ch[0][0] == 'X' && co_at(0, 0) == 0x52, "ESC 7 / ESC 8 save and restore the colours with the cursor");

    reset(); out("abc\033[44m\033[2J");
    EXPECT_TRUE(co_at(5, 5) == 0x47 && co_at(0, 0) == 0x47 && screen_is_grid(), "ESC[2J erases with the background colour");
    out("\033[H\033[41mxyz\033[2D\033[K");
    EXPECT_TRUE(co_at(0, 0) == 0x17 && co_at(0, 1) == 0x17 && co_at(0, 79) == 0x17 &&
                scr_ch[0][1] == ' ' && screen_is_grid(), "ESC[K erases with the background colour (BCE)");
    out("\033[46m\033[32;1H\nq");
    EXPECT_TRUE(co_at(31, 40) == 0x67 && co_at(30, 79) == 0x47 && screen_is_grid(), "a scroll's new line takes the background colour");

    reset();
    for (int i = 0; i < 32; i++) { char b[40]; snprintf(b, sizeof b, "\033[%d;1H\033[3%dmrow %d", i + 1, i % 8, i); out(b); }
    out("\033[0m\033[5;12r\033[12;1H\n\n");
    EXPECT_TRUE(!strcmp(row(4), "row 6") && !strcmp(row(9), "row 11") && !strcmp(row(10), "") && !strcmp(row(12), "row 12") &&
                screen_is_grid(), "a scroll region scrolls only its rows (rows 5-12, two line feeds)");
    long b0 = blits, c0 = cells;
    out("\033[44m\033[2S");
    EXPECT_TRUE(!strcmp(row(4), "row 8") && co_at(11, 0) == 0x47 && co_at(10, 0) == 0x47 && screen_is_grid() &&
                blits - b0 == 2 && cells - c0 < 10, "CSI 2 S in a region: one blitter copy and one fill, nothing redrawn by the CPU");
    out("\033[6;1H\033[3L");
    EXPECT_TRUE(!strcmp(row(5), "") && !strcmp(row(8), "row 9") && co_at(6, 3) == 0x47 && screen_is_grid(), "CSI 3 L inserts lines inside the region");
    out("\033[6;1H\033[2M");
    EXPECT_TRUE(!strcmp(row(6), "row 9") && screen_is_grid(), "CSI 2 M deletes lines inside the region");
    out("\033[5;1H\033M\033M");
    EXPECT_TRUE(!strcmp(row(4), "") && !strcmp(row(5), "") && screen_is_grid(), "ESC M at the region's top scrolls it down");
    out("\033[T"); EXPECT_TRUE(screen_is_grid() && !strcmp(row(13), "row 13"), "CSI T scrolls the region down; rows outside stay");
    out("\033[r\033[0m");

    /* the scrollback keeps one colour byte per cell, inverse folded in */
    reset();
    out("\033[31mred\033[7mINV\033[0m\n");
    for (int i = 0; i < 40; i++) out("filler\n");
    term_scroll_view(10);                               /* 41 lines on 32 rows: the first 10 went to the scrollback */
    EXPECT_TRUE(!strcmp(row(0), "redINV") && scr_co[0][0] == 0x01 && scr_co[0][3] == 0x10 && scr_at[0][3] == 0,
                "scrollback: colours kept, inverse as swapped colours");
    term_scroll_view(-10);
    EXPECT_TRUE(screen_is_grid(), "... and back to the live screen");

    reset(); out("\033[?1h"); EXPECT_TRUE(term_ckm() == 1, "DECCKM: ESC[?1h asks for ESC O A arrows");
    out("\033[?1l"); EXPECT_TRUE(term_ckm() == 0, "... ESC[?1l back to ESC [ A");
    out("\033[?1;25h"); EXPECT_TRUE(term_ckm() == 1, "... also in a list (ESC[?1;25h)");
    out("\033c"); EXPECT_TRUE(term_ckm() == 0, "... ESC c resets it");
    out("\033[1h"); EXPECT_TRUE(term_ckm() == 0, "... ESC[1h (no ?) is not DECCKM");

    /* ---- DESIGN D20 step 1: the console rules, a console's font, the ops ---- */
    static struct file ttyf[NCONS + 1], filef;
    for (int i = 1; i <= NCONS; i++) { ttyf[i].type = FT_TTY; ttyf[i].off = (u32)i; }
    filef.type = FT_INODE;
    me.pid = 50; me.ctty = 1; me.uid = 1000;
    me.fd[1] = &ttyf[1]; me.fd[2] = &filef; me.fd[3] = &ttyf[2];
    psf = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);   /* a u32 address, */
    if (psf == MAP_FAILED) { printf("FAIL: mmap\n"); return 1; }                                    /* as on the Amiga */
    psf[0] = 0x36; psf[1] = 0x04; psf[2] = 0; psf[3] = 8;
    reset();
    EXPECT_TRUE(term_conop(CON_RULES, 1, 1) == -EBADF && term_conop(CON_FONT, PSF(1), 2052) == -EBADF,
                "the font and rules ops without a descriptor in the op word: -EBADF");
    EXPECT_TRUE(term_conop(OP(CON_RULES, 2), 1, 1) == -ENOTTY && term_conop(OP(CON_FONT, 2), PSF(1), 2052) == -ENOTTY &&
                term_conop(OP(CON_RULES, 9), 0, 0) == -EBADF, "a descriptor that is a file: -ENOTTY; none open: -EBADF");
    EXPECT_TRUE(term_conop(OP(4, 1), 0, 0) == -EINVAL && term_conop(OP(CON_COLOURS, 1), 0, 0) == -EINVAL,
                "an unknown op, and CON_COLOURS with a descriptor: -EINVAL");
    EXPECT_TRUE(term_conop(OP(CON_RULES, 3), 1, 1) == -EPERM && term_conop(OP(CON_FONT, 3), PSF(1), 2052) == -EPERM,
                "another console than the caller's own (not root): -EPERM");
    fg_ok = 0;
    EXPECT_TRUE(term_conop(OP(CON_RULES, 1), 1, 1) == -EPERM && term_conop(OP(CON_FONT, 1), PSF(1), 2052) == -EPERM &&
                term_conop(OP(CON_RULES, 1), 0, 0) == 0, "a background job: -EPERM (it may only ask the rules)");
    fg_ok = 1;
    EXPECT_TRUE(term_conop(OP(CON_RULES, 1), 256, 256) == -EINVAL && term_conop(OP(CON_RULES, 1), 0, 512) == -EINVAL &&
                T()->rules == 0, "unknown rule bits: -EINVAL, nothing changes");
    up = 0;
    EXPECT_TRUE(term_conop(OP(CON_RULES, 1), 1, 1) == -ENODEV, "no display: -ENODEV");
    up = 1;
    EXPECT_TRUE(term_conop(OP(CON_FONT, 1), PSF(1), 2051) == -EINVAL, "a font of 2,051 bytes: -EINVAL");
    psf[0] = 0x37; EXPECT_TRUE(term_conop(OP(CON_FONT, 1), PSF(1), 2052) == -EINVAL, "a font with the wrong magic: -EINVAL"); psf[0] = 0x36;
    psf[2] = 1; EXPECT_TRUE(term_conop(OP(CON_FONT, 1), PSF(1), 2052) == -EINVAL, "PSF1 mode 1 (512 glyphs): -EINVAL"); psf[2] = 0;
    psf[3] = 16; EXPECT_TRUE(term_conop(OP(CON_FONT, 1), PSF(1), 2052) == -EINVAL, "height 16: -EINVAL"); psf[3] = 8;
    uok = 0; EXPECT_TRUE(term_conop(OP(CON_FONT, 1), PSF(1), 2052) == -EFAULT, "the font outside the caller's memory: -EFAULT"); uok = 1;
    font_nomem = 1; EXPECT_TRUE(term_conop(OP(CON_FONT, 1), PSF(1), 2052) == -ENOMEM && !T()->font, "no memory for the font: -ENOMEM, nothing changes"); font_nomem = 0;
    EXPECT_TRUE(nfonts == 0, "... and no font was made for any refused call");

    /* the font: loaded, the screen redrawn in it; replaced (last setter wins); its owner's end drops it */
    out("abc\033[31mdef");
    int s0 = shows;
    EXPECT_TRUE(term_conop(OP(CON_FONT, 1), PSF(11), 2052) == 0 && shown_font && shown_font->id == 11 && shows > s0 &&
                scr_font[0][0] == 11 && scr_font[0][5] == 11 && screen_is_grid(), "CON_FONT: the console redrawn in the new font");
    out("g");
    EXPECT_TRUE(scr_font[0][6] == 11, "... and new text drawn in it");
    me.pid = 51;
    EXPECT_TRUE(term_conop(OP(CON_FONT, 1), PSF(12), 2052) == 0 && shown_font->id == 12 && live_fonts == 1 && T()->fowner == 51,
                "a second CON_FONT replaces it (the old one freed) and its caller is the owner now");
    term_release(50);
    EXPECT_TRUE(T()->font && shown_font->id == 12, "the former owner's end leaves the font");
    con_switch(2);
    EXPECT_TRUE(!shown_font, "console 2 shown: Topaz");
    con_switch(1);
    EXPECT_TRUE(shown_font && shown_font->id == 12 && scr_font[0][0] == 12, "console 1 again: its font, everything redrawn in it");
    term_write(2, "x", 1);
    EXPECT_TRUE(shown_font->id == 12, "output on console 2 meanwhile changes nothing on screen");
    out("\033c");
    EXPECT_TRUE(T()->font && shown_font->id == 12, "ESC c keeps the font");
    out("ab");
    term_release(51);
    EXPECT_TRUE(!T()->font && !shown_font && live_fonts == 0 && scr_font[0][0] == 0 && screen_is_grid(),
                "its owner's end drops it: the console redrawn in Topaz");
    me.pid = 50;
    EXPECT_TRUE(term_conop(OP(CON_FONT, 1), PSF(13), 2052) == 0 && term_conop(OP(CON_FONT, 1), 0, 0) == 0 && !shown_font &&
                live_fonts == 0, "CON_FONT with a null pointer drops it");
    me.uid = 0; me.ctty = 2;
    EXPECT_TRUE(term_conop(OP(CON_FONT, 1), PSF(14), 2052) == 0 && shown_font && shown_font->id == 14,
                "root sets the font of a console that is not its own");
    me.uid = 1000; me.ctty = 1;
    term_release(50);

    /* the rules: asked, set with a mask, the bits before answered; dropped at the owner's end */
    reset();
    EXPECT_TRUE(term_conop(OP(CON_RULES, 1), 0, 0) == 0, "CON_RULES with mask 0 asks: none set");
    EXPECT_TRUE(term_conop(OP(CON_RULES, 1), CONR_PCCTRL | CONR_LFONLY, CONR_ALL) == 0 &&
                term_conop(OP(CON_RULES, 1), CONR_WRAPNOW, CONR_WRAPNOW | CONR_LFONLY) == (CONR_PCCTRL | CONR_LFONLY) &&
                T()->rules == (CONR_PCCTRL | CONR_WRAPNOW), "the mask changes only its bits; the answer is the bits before");
    { u8 b[] = { 'A', 1, 3, 0, 0x0E, 0x1F, 7, 'B', 0x1A, 'C' }; int b0 = bells; outn((const char *)b, sizeof b);
      EXPECT_TRUE(scr_ch[0][0] == 'A' && scr_ch[0][1] == 1 && scr_ch[0][2] == 3 && scr_ch[0][3] == 0x0E && scr_ch[0][4] == 0x1F &&
                  scr_ch[0][5] == 'B' && scr_ch[0][6] == 0x1A && scr_ch[0][7] == 'C' && bells == b0 + 1 && screen_is_grid(),
                  "CONR_PCCTRL: 0x01-0x1F stored and drawn as glyphs (SUB too, outside a sequence); NUL ignored, BEL rings"); }
    out("\r\033[1;2\030X\033[\001m");
    EXPECT_TRUE(scr_ch[0][0] == 'X' && scr_ch[0][1] == 1, "... CAN still cancels a sequence; a control inside one is not stored");
    reset(); term_conop(OP(CON_RULES, 1), CONR_WRAPNOW, CONR_ALL);
    for (int i = 0; i < COLS; i++) out("W");
    out("\r\nX");
    EXPECT_TRUE(T()->cy == 2 && scr_ch[2][0] == 'X' && !row(1)[0] && screen_is_grid(), "CONR_WRAPNOW: a full row wraps at once (CR LF then leaves a blank row)");
    reset(); term_conop(OP(CON_RULES, 1), 0, CONR_ALL);
    for (int i = 0; i < COLS; i++) out("W");
    out("\r\nX");
    EXPECT_TRUE(T()->cy == 1 && scr_ch[1][0] == 'X', "... without it the wrap waits (CR LF goes to the next row)");
    reset(); out("\033[5;5HZ\033[2JH");
    EXPECT_TRUE(scr_ch[4][5] == 'H', "ESC[2J leaves the cursor (xterm)");
    reset(); term_conop(OP(CON_RULES, 1), CONR_CLRHOME, CONR_ALL); out("\033[5;5HZ\033[2JH\033[5;5H\033[JQ");
    EXPECT_TRUE(scr_ch[0][0] == 'H' && scr_ch[4][4] == 'Q', "CONR_CLRHOME: ESC[2J homes the cursor (ESC[J does not)");
    reset(); term_conop(OP(CON_RULES, 1), CONR_BRIGHT, CONR_ALL);
    out("\033[1;31mA\033[22mB\033[1mC\033[0;30;1mD\033[39mE\033[0;93;1mF\033[mG");
    EXPECT_TRUE(co_at(0, 0) == 0x09 && !(T()->at[0][0] & A_BOLD) && co_at(0, 1) == 0x01 && co_at(0, 2) == 0x09 &&
                co_at(0, 3) == 0x08 && co_at(0, 4) == 0x0F && co_at(0, 5) == 0x0B && co_at(0, 6) == 0x07 && screen_is_grid(),
                "CONR_BRIGHT: SGR 1 stores the bright foreground (1;30 is dark grey), never bold; 22 and 0 end it");
    out("\033[1;32m\0337\033[0m\0338H");
    EXPECT_TRUE(co_at(0, 7) == 0x0A, "... ESC 7 / ESC 8 keep the bright flag");
    reset(); term_conop(OP(CON_RULES, 1), CONR_ICE, CONR_ALL);
    out("\033[5;44mA\033[25mB\033[6;41mC\033[0mD\033[5;46m\033[K");
    EXPECT_TRUE(co_at(0, 0) == 0xC7 && co_at(0, 1) == 0x47 && co_at(0, 2) == 0x97 && co_at(0, 3) == 0x07 &&
                co_at(0, 79) == 0xE7 && screen_is_grid(), "CONR_ICE: SGR 5 and 6 store the bright background (erases too), 25 and 0 end it");
    reset(); term_conop(OP(CON_RULES, 1), 0, CONR_ALL);
    out("\033[5;44mA\033[1;31mB");
    EXPECT_TRUE(co_at(0, 0) == 0x47 && co_at(0, 1) == 0x41 && (T()->at[0][1] & A_BOLD), "... without the rules: 5 ignored, 1 bold (D14)");
    reset(); term_conop(OP(CON_RULES, 1), CONR_LFONLY, CONR_ALL);
    out("AB\nC\r\nD");
    EXPECT_TRUE(scr_ch[1][2] == 'C' && scr_ch[2][0] == 'D' && screen_is_grid(), "CONR_LFONLY: LF moves down in the same column");
    reset(); term_conop(OP(CON_RULES, 1), 0, CONR_ALL);
    out("AB\nC");
    EXPECT_TRUE(scr_ch[1][0] == 'C', "... without it LF is CR LF (the Amiga console's, Zenix's)");
    /* the colours are stored: the scrollback and a redraw keep them after the rule is gone */
    reset(); term_conop(OP(CON_RULES, 1), CONR_BRIGHT | CONR_ICE, CONR_ALL);
    out("\033[1;5;31;44mbright\n");
    term_conop(OP(CON_RULES, 1), 0, CONR_ALL);
    EXPECT_TRUE(co_at(0, 0) == 0xC9 && T()->cor == 0, "dropping the rules changes no cell, and ends the bright flags");
    for (int i = 0; i < 40; i++) out("filler\n");
    term_scroll_view(10);                               /* 42 lines on 32 rows: "bright" is the 10th line back */
    EXPECT_TRUE(!strcmp(row(0), "bright") && scr_co[0][0] == 0xC9, "the scrollback keeps the bright colours");
    term_scroll_view(-10);
    /* the palette: CONR_CGAPAL asks for it while its console is shown */
    reset();
    term_conop(OP(CON_RULES, 1), CONR_CGAPAL, CONR_ALL);
    EXPECT_TRUE(shown_cga == 1, "CONR_CGAPAL: the CGA palette at once on the shown console");
    con_switch(2); EXPECT_TRUE(shown_cga == 0, "console 2 shown: Zenix's palette");
    me.uid = 0;
    term_conop(OP(CON_RULES, 3), CONR_CGAPAL, CONR_CGAPAL);
    EXPECT_TRUE(shown_cga == 1, "root sets CONR_CGAPAL on the shown console 2 (not its own): the palette follows");
    term_conop(OP(CON_RULES, 3), 0, CONR_CGAPAL);
    me.uid = 1000;
    con_switch(1); EXPECT_TRUE(shown_cga == 1, "console 1 again: CGA's");
    out("\033c"); EXPECT_TRUE(T()->rules == CONR_CGAPAL, "ESC c keeps the rules");
    term_release(50);
    EXPECT_TRUE(T()->rules == 0 && shown_cga == 0 && !T()->rowner, "the setter's end drops the rules: Zenix's palette again");
    /* a panic: Topaz and no rules on the shown console */
    term_conop(OP(CON_FONT, 1), PSF(15), 2052);
    term_conop(OP(CON_RULES, 1), CONR_ALL, CONR_ALL);
    term_panic();
    EXPECT_TRUE(!shown_font && !shown_cga && T()->rules == 0, "term_panic: Topaz, Zenix's palette, no rules (the panic text is readable)");
    out("x\n");
    EXPECT_TRUE(scr_font[T()->cy - 1][0] == 0, "... and the text after it is drawn in Topaz");
    term_release(50);
    EXPECT_TRUE(live_fonts == 0, "the font itself is dropped when its owner ends");

    /* a fuzz: random sequences and text, after each piece the screen must show the grid */
    reset();
    static const char *const bits[] = { "\033[", "\033]", "\033", ";", ":", "1", "2", "5", "38", "48", "7", "?", ">",
        "m", "H", "J", "K", "L", "M", "S", "T", "r", "@", "P", "X", "h", "l", "A", "B", "C", "D", "\n", "\r", "\t", "\b",
        "x", "hello", "\0337", "\0338", "\033M", "\033D", "\033E", "\033c", "\007", "\030", " ", "\\", "31", "44", "107" };
    srand(14);
    int fuzz_ok = 1;
    for (int i = 0; i < 200000 && fuzz_ok; i++) {
        const char *p = bits[rand() % (int)(sizeof bits / sizeof bits[0])];
        out(p);
        if (i % 97 == 0 && !T()->esc && !screen_is_grid()) { fuzz_ok = 0; printf("      after piece %d\n", i); }
    }
    EXPECT_TRUE(fuzz_ok, "200000 random pieces of sequences and text: the screen always shows the grid");
    trows = 25;                                         /* NTSC */
    reset();
    for (int i = 0; i < 100000 && fuzz_ok; i++) {
        out(bits[rand() % (int)(sizeof bits / sizeof bits[0])]);
        if (i % 89 == 0 && !T()->esc && !screen_is_grid()) { fuzz_ok = 0; printf("      after piece %d\n", i); }
    }
    EXPECT_TRUE(fuzz_ok, "... and 100000 on an NTSC screen (25 rows)");
    trows = 32;
    reset();
    term_conop(OP(CON_RULES, 1), CONR_ALL, CONR_ALL);   /* D20: every rule set */
    static const char *const bbs[] = { "\001", "\x0e", "\x1a", "\x1f", "\x7f", "\xb0", "\033[1m", "\033[5m", "\033[6m",
        "\033[25m", "\033[22m", "\033[2J", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW" };
    for (int i = 0; i < 200000 && fuzz_ok; i++) {
        int k = rand() % (int)(sizeof bits / sizeof bits[0] + sizeof bbs / sizeof bbs[0]);
        out(k < (int)(sizeof bits / sizeof bits[0]) ? bits[k] : bbs[k - (int)(sizeof bits / sizeof bits[0])]);
        if (i % 97 == 0 && !T()->esc && !screen_is_grid()) { fuzz_ok = 0; printf("      after piece %d\n", i); }
    }
    EXPECT_TRUE(fuzz_ok, "... and 200000 with every console rule set (D20)");

    /* D20 section 6: a tty write's piece (term_write_some) ends early once two ticks of drawing have passed while
     * input waits or a better process is ready; the screen is then the same as after one whole write */
    trows = 32;
    reset();
    term_conop(OP(CON_RULES, 1), 0, CONR_ALL);
    static char line[300];
    for (int i = 0; i < 299; i++) line[i] = (char)('a' + i % 26);
    tick_per_cell = 1;
    u32 got = term_write_some(1, line, 256, 256);
    EXPECT_TRUE(got == 256, "a piece with nobody waiting: all 256 bytes drawn");
    reset();
    kwork_pending = 1;
    got = term_write_some(1, line, 256, 256);
    EXPECT_TRUE(got >= 1 && got <= 2, "input waiting: the piece ends after two ticks of drawing (a byte or two here)");
    EXPECT_TRUE(screen_is_grid(), "... the screen is consistent there (the cursor drawn)");
    kwork_pending = 0;
    need_resched = 1;
    u32 got2 = term_write_some(1, line + got, 254, 254);
    EXPECT_TRUE(got2 >= 1 && got2 <= 2, "a better process ready: the same");
    need_resched = 0;
    term_write_some(1, line + got + got2, 299 - got - got2, 299 - got - got2);
    EXPECT_TRUE(!strncmp(row(0), line, 80) && !strncmp(row(3), line + 240, 59) && screen_is_grid(),
                "the rest drawn later: the screen as from one write");
    tick_per_cell = 0;

    /* audit 2026-10-28 S12: what telnet writes at a call's end undoes what a board left: an open OSC string, black on
     * black, a two-row scroll region, a hidden cursor, cursor-key mode; the cursor stays where it was */
    reset();
    out("\033[5;7H\033[30;40m\033[?25l\033[?1h\033[3;4r\033[5;7H\033]0;title never ended");
    {
        struct term *t = T();
        int x0 = t->cx, y0 = t->cy;
        out(TN_TERM_RESET);
        out("ok");
        EXPECT_TRUE(t->esc == 0 && t->attr == 0 && t->colour == CO_DEF && t->curvis && !t->ckm && t->top == 0 &&
                    t->bot == trows - 1 && !strcmp(row(y0) + x0, "ok"),
                    "telnet's reset after a hostile board: string ended, colours, scroll region, cursor, keys; in place");
    }

    /* ---- DESIGN D24 6.1-6.3: CON_READ against the model screen, the ops' checks ---- */
    me.uid = 0; me.fd[4] = &ttyf[9];
    reset();
    out("\033[1;4;7;31;42mxy\033[m\033[3;71H\345\033[44m\033[K\033[m\033[6;11H");
    for (int i = 0; i < 300; i++) { char c = (char)(" abc\033[;1234567890mHJKLM@P\n\r\t"[(i * 7 + i / 13) % 29]); outn(&c, 1); }
    {
        struct uconread *r = (struct uconread *)psf;            /* a u32 address, as on the Amiga */
        u8 *cells = mmap(0, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (cells == MAP_FAILED) { printf("FAIL: mmap\n"); return 1; }
        memset(r, 0, sizeof *r);
        r->size = sizeof *r; r->cells = (u32)(unsigned long)cells; r->ncells = 16384;
        long n = term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, 0);
        int same = n == trows * COLS * 3;
        for (int y = 0; y < trows && same; y++)
            for (int x = 0; x < COLS && same; x++) {
                const u8 *c = cells + 3 * (y * COLS + x);
                same = c[0] == T()->ch[y][x] && c[1] == T()->at[y][x] && c[2] == T()->co[y][x] &&
                       (c[0] == scr_ch[y][x] || !screen_is_grid());
            }
        EXPECT_TRUE(same && screen_is_grid(), "CON_READ: every cell as the grid holds it (and the screen shows it)");
        EXPECT_TRUE(r->rows == trows && r->cols == COLS && r->crow == T()->cy && r->ccol == T()->cx && r->csize == 3 &&
                    r->changes == T()->changes && (r->flags & CONRD_SHOWN), "CON_READ: the header");
        u32 ch0 = r->changes; out("x");
        term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, 0);
        EXPECT_TRUE(r->changes == ch0 + 1, "CON_READ: a write raises the change counter");
        r->ncells = 8; cells[6] = 0x5A;
        EXPECT_TRUE(term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, 0) == 6 && cells[6] == 0x5A, "CON_READ: whole cells only");
        r->size = 27;
        EXPECT_TRUE(term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, 0) == -EINVAL, "CON_READ: a size below the first: -EINVAL");
        r->size = 0; r->ncells = 0;
        EXPECT_TRUE(term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, 0) == 0 && r->rows == trows, "CON_READ: size 0 is the first size");
        EXPECT_TRUE(term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, 2) == -EINVAL, "CON_READ: a3 not 0 or 1: -EINVAL");
        uok = 0; EXPECT_TRUE(term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, 0) == -EFAULT, "CON_READ: a struct outside the caller: -EFAULT"); uok = 1;
        EXPECT_TRUE(!wsp[9] && term_conop(OP(CON_READ, 4), (u32)(unsigned long)r, 0) == -ENXIO &&
                    term_conop(OP(CON_LINE, 4), 0, (u32)(unsigned long)r) == -ENXIO &&
                    term_conop(OP(CON_INPUT, 4), (u32)(unsigned long)psf, 1 | CONI_TEXT << 16) == -ENXIO && !wsp[9],
                    "a console never used: -ENXIO for all three, nothing allocated");
        EXPECT_TRUE(term_conop(OP(CON_READ, 2), (u32)(unsigned long)r, 0) == -ENOTTY &&
                    term_conop(OP(CON_READ, 9), (u32)(unsigned long)r, 0) == -EBADF, "CON_READ: a file -ENOTTY, none -EBADF");
        r->size = 0; EXPECT_TRUE(term_conop(OP(CON_LINE, 1), 17, (u32)(unsigned long)r) == 640 && line_n == 17, "CON_LINE: the shown console's line to gfx");
        EXPECT_TRUE(term_conop(OP(CON_LINE, 3), 0, (u32)(unsigned long)r) == -ENXIO, "CON_LINE: a console not shown: -ENXIO");
        up = 0; EXPECT_TRUE(term_conop(OP(CON_LINE, 1), 0, (u32)(unsigned long)r) == -ENODEV &&
                            term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, 0) == 0, "no display: CON_LINE -ENODEV, CON_READ works"); up = 1;
        EXPECT_TRUE(term_conop(OP(CON_INPUT, 3), (u32)(unsigned long)psf, 5 | CONI_TEXT << 16) == 5 && text_con == 2 && text_n == 5,
                    "CON_INPUT text: into the descriptor's console");
        EXPECT_TRUE(term_conop(OP(CON_INPUT, 4), (u32)(unsigned long)psf, 256) == 256 && raw_n == 256,
                    "CON_INPUT raw: to the keyboard whichever console names it, 256 bytes");
        EXPECT_TRUE(term_conop(OP(CON_INPUT, 1), (u32)(unsigned long)psf, 257) == -EINVAL &&
                    term_conop(OP(CON_INPUT, 1), (u32)(unsigned long)psf, 1 | 2 << 16) == -EINVAL &&
                    term_conop(OP(CON_INPUT, 1), (u32)(unsigned long)psf, 1 | 1u << 24) == -EINVAL, "CON_INPUT: 257 bytes, kind 2, high bits: -EINVAL");
        uok = 0; EXPECT_TRUE(term_conop(OP(CON_INPUT, 1), (u32)(unsigned long)psf, 1) == -EFAULT, "CON_INPUT: bytes outside the caller: -EFAULT"); uok = 1;
        me.uid = 1000;
        EXPECT_TRUE(term_conop(OP(CON_READ, 1), (u32)(unsigned long)r, 0) == -EPERM && term_conop(OP(CON_INPUT, 9), 0, 0) == -EPERM &&
                    term_conop(OP(CON_LINE, 1), 0, 0) == -EPERM, "not root: -EPERM, before the descriptor's checks");
        EXPECT_TRUE(term_conop(OP(10, 1), 0, 0) == -EINVAL, "op 10: -EINVAL (8 and 9 are D38's)");
        EXPECT_TRUE(term_conop(OP(CON_GRAB, 1), 0, 0) == -EPERM, "CON_GRAB, not root: -EPERM");
    }

    /* ---- DESIGN D28: UTF-8, the cells, wide characters, the 8-bit mode, CON_UFONT ---- */
    me.uid = 1000; me.pid = 50; me.ctty = 1;
    term_release(50);
    reset();
    d28();

    if (bad) { printf("term-test: %d FAILED\n", bad); return 1; }
    printf("term-test: all passed\n");
    return 0;
}
