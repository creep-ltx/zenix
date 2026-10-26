/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the terminal (kernel/term.c) against a model of the display: what term.c draws lands in a cell grid
 * here (the gfx_* calls), so every check compares the grid with what a terminal must show. The escape parser
 * (ECMA-48: unknown sequences, OSC, private markers, ':' sub-parameters, long parameter lists are swallowed without
 * printing anything).
 * Build and run: tools/term-test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/term.c"

/* ---- the model display ---- */
int trows = 32;
static int up = 1;
static u8 scr_ch[TROWS_MAX][COLS], scr_at[TROWS_MAX][COLS];
int gfx_up(void) { return up; }
void gfx_cell(int row, int col, u8 ch, u8 attr)
{
    if (row < 0 || row >= trows || col < 0 || col >= COLS) { printf("FAIL: gfx_cell outside the screen %d,%d\n", row, col); exit(1); }
    scr_ch[row][col] = ch;
    scr_at[row][col] = attr;
}
void gfx_scroll(void)
{
    memmove(scr_ch[0], scr_ch[1], (size_t)(trows - 1) * COLS);
    memmove(scr_at[0], scr_at[1], (size_t)(trows - 1) * COLS);
    memset(scr_ch[trows - 1], ' ', COLS);
    memset(scr_at[trows - 1], 0, COLS);
}
void gfx_clear_text(void)
{
    memset(scr_ch, ' ', sizeof scr_ch);
    memset(scr_at, 0, sizeof scr_at);
}

/* ---- the rest of the kernel that term.c calls ---- */
struct proc procs[NPROC];
void ready(struct proc *p) { (void)p; }
void *kmalloc(u32 n) { return malloc(n); }
void panic(const char *why) { printf("FAIL: panic %s\n", why); exit(1); }
static int bells;
void beep(u32 hz, u32 ms) { (void)hz; (void)ms; bells++; }
static char injected[64];
void tty_inject(int tty, const char *s) { (void)tty; strncat(injected, s, sizeof injected - strlen(injected) - 1); }

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
static void reset(void) { out("\033c"); gfx_clear_text(); }
/* does the screen show only `want` on row 0 and nothing else (the cursor is drawn inverse: that is allowed) */
static int only(const char *want)
{
    if (strcmp(row(0), want)) { printf("      row 0: '%s', wanted '%s'\n", row(0), want); return 0; }
    for (int r = 1; r < trows; r++) if (row(r)[0]) { printf("      row %d: '%s'\n", r, row(r)); return 0; }
    return 1;
}

int main(void)
{
    gfx_clear_text();
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
    EXPECT_TRUE(!strcmp(injected, "\033[1;1R"), "ESC[6n answers the cursor position");
    injected[0] = 0; out("\033[?6n");
    EXPECT_TRUE(!injected[0], "... ESC[?6n (private) does not");

    if (bad) { printf("term-test: %d FAILED\n", bad); return 1; }
    printf("term-test: all passed\n");
    return 0;
}
