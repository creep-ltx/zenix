/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the terminal (kernel/term.c) against a model of the display: what term.c draws lands in a cell grid
 * here (the gfx_* calls), so every check compares the grid with what a terminal must show. The escape parser
 * (ECMA-48: unknown sequences, OSC, private markers, ':' sub-parameters, long parameter lists are swallowed without
 * printing anything); the colours (SGR, 256 and true colour mapped to the 16, background-colour erase), region
 * scrolls drawn by blitter moves and fills, the scrollback's colour byte, ESC 7/8, DECCKM; a fuzz after which the
 * screen must always show the grid.
 * Build and run: tools/term-test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../kernel/term.c"

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
    cells++;
    ticks += tick_per_cell;             /* drawing takes time (term_write_some's bound) */
}
/* a filled cell: blank, the background colour (its foreground cannot be seen: kept as the default) */
static void fill(int row, int n, u8 colour)
{
    memset(scr_ch[row], ' ', (size_t)n * COLS);
    memset(scr_at[row], 0, (size_t)n * COLS);
    memset(scr_co[row], (colour & 0xF0) | 7, (size_t)n * COLS);
}
void gfx_scroll(u8 colour)
{
    memmove(scr_ch[0], scr_ch[1], (size_t)(trows - 1) * COLS);
    memmove(scr_at[0], scr_at[1], (size_t)(trows - 1) * COLS);
    memmove(scr_co[0], scr_co[1], (size_t)(trows - 1) * COLS);
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
void *kmalloc(u32 n) { return malloc(n); }
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
void gfx_show(const struct gfont *f, int cga)
{
    if (f && f->freed) { printf("FAIL: a freed font shown\n"); exit(1); }
    shown_font = f; shown_cga = cga != 0; shows++;
}
static struct proc me;
struct proc *current = &me;
static int fg_ok = 1, uok = 1;
int tty_of(const struct proc *p) { return p->ctty; }
int tty_in_fg(int tty, const struct proc *p) { (void)tty; (void)p; return fg_ok; }
int user_ok_rd(const struct proc *p, u32 ptr, u32 len) { (void)p; (void)ptr; (void)len; return uok; }

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
static int screen_is_grid(void)
{
    struct term *t = T();
    for (int y = 0; y < trows; y++)
        for (int x = 0; x < COLS; x++) {
            int cur = t->curvis && y == t->cy && x == t->cx;
            u8 at = (u8)(t->at[y][x] ^ (cur ? A_INV : 0));
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
    #define OP(op, fd) ((u32)(op) | (u32)((fd) + 1) << CON_FD_SHIFT)
    u8 *psf = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);   /* a u32 address, */
    if (psf == MAP_FAILED) { printf("FAIL: mmap\n"); return 1; }                                    /* as on the Amiga */
    psf[0] = 0x36; psf[1] = 0x04; psf[2] = 0; psf[3] = 8;
    #define PSF(id) (psf[4 + 8 * 'A'] = (id), (u32)(unsigned long)psf)
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
    EXPECT_TRUE(term_conop(OP(CON_RULES, 1), 128, 128) == -EINVAL && term_conop(OP(CON_RULES, 1), 0, 256) == -EINVAL &&
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
    u32 got = term_write_some(1, line, 256);
    EXPECT_TRUE(got == 256, "a piece with nobody waiting: all 256 bytes drawn");
    reset();
    kwork_pending = 1;
    got = term_write_some(1, line, 256);
    EXPECT_TRUE(got >= 1 && got <= 2, "input waiting: the piece ends after two ticks of drawing (a byte or two here)");
    EXPECT_TRUE(screen_is_grid(), "... the screen is consistent there (the cursor drawn)");
    kwork_pending = 0;
    need_resched = 1;
    u32 got2 = term_write_some(1, line + got, 254);
    EXPECT_TRUE(got2 >= 1 && got2 <= 2, "a better process ready: the same");
    need_resched = 0;
    term_write_some(1, line + got + got2, 299 - got - got2);
    EXPECT_TRUE(!strncmp(row(0), line, 80) && !strncmp(row(3), line + 240, 59) && screen_is_grid(),
                "the rest drawn later: the screen as from one write");
    tick_per_cell = 0;

    if (bad) { printf("term-test: %d FAILED\n", bad); return 1; }
    printf("term-test: all passed\n");
    return 0;
}
