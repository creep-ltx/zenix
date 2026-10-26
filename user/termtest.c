/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* Terminal test: ANSI features, then a raw-mode key reader (q quits). `termtest columns`: a pattern that numbers the
 * 80 columns in binary (the screenshot test of the display's fetch window, DESIGN D14). */
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

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "columns")) { columns(); return 0; }
    if (argc > 1) { fprintf(2, "usage: termtest [columns]\n"); return 2; }
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
