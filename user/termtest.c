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

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "columns")) { columns(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "colours")) { colours(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "scroll")) { scroll(); return 0; }
    if (argc > 1) { fprintf(2, "usage: termtest [columns|colours|scroll]\n"); return 2; }
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
