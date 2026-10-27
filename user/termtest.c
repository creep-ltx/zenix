/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* Terminal test: ANSI features, then a raw-mode key reader (q quits). For the screenshot tests (DESIGN D14):
 * `termtest columns` numbers the 80 columns in binary (the display's fetch window); `termtest colours` shows the 16
 * colours as blocks and as text on black and on white, with bold, underline and inverse; `termtest scroll` writes
 * coloured lines that scroll the screen several times, with background-colour erases, a scroll region, inserted and
 * deleted lines, then ends. */
#define ESC "\033"
static void at(int r, int c, const char *s) { printf(ESC "[%d;%dH%s", r, c, s); }

/* wait for q in raw mode, the cursor hidden or not */
static void keys(int show)
{
    sys_ttymode(TTY_RAW);
    for (;;) {
        char b[8];
        int n = sys_read(0, b, sizeof b);
        if (n <= 0 || b[0] == 'q') break;
        if (show) {
            printf("key:");
            for (int i = 0; i < n; i++) printf(" %02x", (unsigned char)b[i]);
            printf("\n");
        }
    }
    sys_ttymode(0);
}

/* rows 1-7: column c lit (white, every plane) where bit (row - 1) of c is set; row 8 lit across all 80 columns */
static void columns(void)
{
    printf(ESC "[?25l" ESC "[2J");
    for (int k = 0; k < 8; k++) {
        printf(ESC "[%d;1H", k + 1);
        for (int c = 0; c < 80; c++)
            printf("%s", k == 7 || (c >> k & 1) ? ESC "[7;97m " ESC "[m" : " ");
    }
    printf(ESC "[10;1Hcolumns 0-79 in binary, row 1 the lowest bit; q quits");
    keys(0);
    printf(ESC "[?25h" ESC "[2J" ESC "[H");
}

/* rows 3-4: the 16 colours as background blocks, 5 columns each (colour i from column 5i); rows 6-9: the 16 as text
 * on black and on white, then bold, underline and inverse */
static void colours(void)
{
    printf(ESC "[?25l" ESC "[2J" ESC "[1;1Hthe 16 colours (ESC[40-47m, ESC[100-107m), as text on black and on white");
    for (int r = 3; r <= 4; r++) {
        printf(ESC "[%d;1H", r);
        for (int i = 0; i < 16; i++) printf(ESC "[%dm     ", i < 8 ? 40 + i : 100 + i - 8);
        printf(ESC "[m");
    }
    for (int b = 0; b < 2; b++) {
        printf(ESC "[%d;1H" ESC "[%dm", 6 + b, b ? 107 : 40);
        for (int i = 0; i < 16; i++) printf(ESC "[%dmAa%-2d ", i < 8 ? 30 + i : 90 + i - 8, i);
        printf(ESC "[m");
    }
    printf(ESC "[8;1H" ESC "[1;31mbold red" ESC "[m " ESC "[4;32munderlined green" ESC "[m " ESC "[7;34minverse blue"
           ESC "[m " ESC "[1;4;7;33mall three yellow" ESC "[m " ESC "[38;5;208m256: 208" ESC "[m "
           ESC "[38;2;0;200;200mtrue colour" ESC "[m");
    printf(ESC "[10;1Hq quits");
    keys(0);
    printf(ESC "[?25h" ESC "[m" ESC "[2J" ESC "[H");
}

/* line n in colour (n % 7) + 1, the line's rest erased in a background colour every 5th line */
static void scroll(void)
{
    printf(ESC "[m" ESC "[2J" ESC "[H");
    for (int n = 1; n <= 100; n++) {
        printf(ESC "[%dmline %d " ESC "[7minverse" ESC "[27m", 30 + n % 7 + 1, n);
        if (n % 5 == 0) printf(ESC "[4%dm" ESC "[K", n / 5 % 8);
        printf(ESC "[m\n");
    }
    printf(ESC "[5;20r" ESC "[20;1H");                          /* a region: scroll inside it */
    for (int n = 1; n <= 30; n++) printf("\n" ESC "[3%dm" ESC "[44mregion %d" ESC "[K" ESC "[m", n % 7 + 1, n);
    printf(ESC "[8;1H" ESC "[45m" ESC "[3L" ESC "[12;1H" ESC "[2M" ESC "[5;1H" ESC "[43m" ESC "M" ESC "[2S" ESC "[m");
    printf(ESC "[r" ESC "[31;1H" ESC "[42m" ESC "[J" ESC "[m");
}

/* DESIGN D20 step 1: a console's loaded font and the BBS rules (tools/colour-test.sh). FILE is a PSF1 font; without
 * one a test font made here: glyph b shows its two hex digits as bits two pixels wide (lines 1-3 and 4-6), so a
 * screenshot names every cell's glyph. On fd 1 (the console): the font, then every rule but LFONLY; drawn (rows 1-based):
 *   1      'H' after a 'Z' at 5,5 and ESC[2J (CONR_CLRHOME: the cursor went home)
 *   3-18   bytes 16r .. 16r + 15 in columns 1-16 (CONR_PCCTRL: the controls are glyphs; BEL BS HT LF CR ESC and NUL,
 *          which act, as spaces)
 *   20-21  the 16 colours as blocks of 5 columns (ESC[40-47m, ESC[100-107m: CONR_CGAPAL's palette)
 *   23     ESC[1;31m R, ESC[0;31m r (CONR_BRIGHT: R bright red, not bold); ESC[5;44m and two spaces, ESC[0;44m and two
 *          (CONR_ICE: a bright blue background, then the plain one), at columns 1-6
 *   24-26  80 W, then CR LF and X (CONR_WRAPNOW: row 25 stays empty, X at 26,1)
 *   27-28  with CONR_LFONLY: A, LF, B (B at 28,2)
 * Keys: q cleans up (the screen cleared, the font and rules dropped) and ends; x ends at once (the kernel drops them);
 * 2 4 8 6 switch the display to 2, 4, 8, 16 colours (root). */
static unsigned char *psf_file(const char *path)
{
    static unsigned char b[2052];
    if (!path) {
        b[0] = 0x36; b[1] = 0x04; b[2] = 0; b[3] = 8;
        static const unsigned char wide[16] = { 0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
                                                0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF };
        for (int c = 0; c < 256; c++) {         /* lines 1-3 the high nibble, 4-6 the low one, each bit 2 pixels */
            unsigned char *g = b + 4 + 8 * c;
            g[1] = g[2] = g[3] = wide[c >> 4];
            g[4] = g[5] = g[6] = wide[c & 15];
        }
        return b;
    }
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) { fprintf(2, "termtest: %s: %s\n", path, zstrerror(fd)); return 0; }
    int n = sys_read(fd, b, sizeof b);
    sys_close(fd);
    if (n != (int)sizeof b) { fprintf(2, "termtest: %s: not a 2,052-byte PSF1 font\n", path); return 0; }
    return b;
}

static int pcfont(const char *path)
{
    unsigned char *f = psf_file(path);
    if (!f) return 1;
    int r = sys_confont(1, f, 2052);
    if (r < 0) { fprintf(2, "termtest: CON_FONT: %s\n", zstrerror(r)); return 1; }
    u32 all = CONR_PCCTRL | CONR_WRAPNOW | CONR_CLRHOME | CONR_BRIGHT | CONR_ICE | CONR_CGAPAL;
    r = sys_conrules(1, all, all | CONR_LFONLY);
    if (r < 0) { fprintf(2, "termtest: CON_RULES: %s\n", zstrerror(r)); sys_confont(1, 0, 0); return 1; }
    printf(ESC "[?25l" ESC "[m" ESC "[5;5HZ" ESC "[2JH");
    for (int row = 0; row < 16; row++) {
        unsigned char l[16];
        printf(ESC "[%d;1H", row + 3);
        for (int c = 16 * row; c < 16 * row + 16; c++) {
            int acts = c == 0 || c == 7 || c == 8 || c == 9 || c == 10 || c == 13 || c == 27;
            l[c - 16 * row] = acts ? ' ' : (unsigned char)c;
        }
        sys_write(1, l, sizeof l);
    }
    for (int r2 = 20; r2 <= 21; r2++) {
        printf(ESC "[%d;1H", r2);
        for (int i = 0; i < 16; i++) printf(ESC "[%dm     ", i < 8 ? 40 + i : 100 + i - 8);
        printf(ESC "[m");
    }
    printf(ESC "[23;1H" ESC "[1;31mR" ESC "[0;31mr" ESC "[5;44m  " ESC "[0;44m  " ESC "[m");
    printf(ESC "[24;1H%s\r\nX", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
    sys_conrules(1, CONR_LFONLY, CONR_LFONLY);
    printf(ESC "[27;1HA\nB");
    sys_conrules(1, 0, CONR_LFONLY);
    printf(ESC "[30;1H");
    sys_ttymode(TTY_RAW);
    for (;;) {
        char b[8];
        int n = sys_read(0, b, sizeof b);
        if (n <= 0 || b[0] == 'q') break;
        if (b[0] == 'x') return 0;              /* the kernel drops the font and the rules (D20 4.2) */
        int c = b[0] == '2' ? 2 : b[0] == '4' ? 4 : b[0] == '8' ? 8 : b[0] == '6' ? 16 : 0;
        if (c && (r = sys_console(CON_COLOURS, c)) < 0)
            fprintf(2, "termtest: colours %d: %s\n", c, zstrerror(r));
    }
    sys_ttymode(0);
    printf(ESC "[m" ESC "[2J" ESC "[H" ESC "[?25h");       /* cleared before the font goes (D20 4.2) */
    sys_conrules(1, 0, CONR_PCCTRL | CONR_WRAPNOW | CONR_CLRHOME | CONR_BRIGHT | CONR_ICE | CONR_CGAPAL | CONR_LFONLY);
    sys_confont(1, 0, 0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "pcfont")) return pcfont(argc > 2 ? argv[2] : 0);
    if (argc > 1 && !strcmp(argv[1], "columns")) { columns(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "colours")) { colours(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "scroll")) { scroll(); return 0; }
    if (argc > 1) { fprintf(2, "usage: termtest [columns|colours|scroll|pcfont [FILE]]\n"); return 2; }
    int sz = sys_ttyinfo();
    printf(ESC "[2J" ESC "[H");
    printf(ESC "[7m Zenix terminal test " ESC "[0m  size %dx%d\n", sz & 0xFFFF, sz >> 16);
    printf("normal " ESC "[1mbold" ESC "[0m " ESC "[4munderline" ESC "[0m " ESC "[7minverse" ESC "[0m "
           ESC "[1;4;7mall three" ESC "[0m\n");
    /* the box is 36 columns wide: its longest line (27 characters from column 5) keeps a margin */
    at(4, 1, "+----------------------------------+");
    for (int r = 5; r < 9; r++) { at(r, 1, "|"); at(r, 36, "|"); }
    at(9, 1, "+----------------------------------+");
    at(6, 5, "cursor addressing works");
    at(7, 5, "ESC[row;colH, erase, scroll");
    /* scroll region rows 11..14: print 8 numbers, only that area scrolls */
    at(10, 1, "scroll region (rows 11-14):");
    printf(ESC "[11;14r");
    for (int i = 1; i <= 8; i++) printf(ESC "[14;1H\nline %d", i);
    printf(ESC "[r");
    at(16, 1, "erase line demo: ");
    printf("XXXXXXXXXXXX" ESC "[6D" ESC "[K|end\n");
    at(17, 1, "insert/delete line: A");
    at(18, 1, "B");
    at(19, 1, "C");
    printf(ESC "[18;1H" ESC "[1L" "inserted");
    at(20, 1, "keys: press arrows/F-keys/Del, q quits");
    at(22, 1, "");
    keys(1);
    printf("done\n");
    return 0;
}
