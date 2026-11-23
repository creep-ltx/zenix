/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
static const char USAGE[] = "usage: cmp file1 file2\n";
/* cmp file1 file2: the first byte where they differ */
int main(int argc, char **argv)
{
    zoperands(&argc, &argv, USAGE);
    if (argc < 3) zusage(USAGE);
    int a = !strcmp(argv[1], "-") ? 0 : sys_open(argv[1], O_RDONLY), b = !strcmp(argv[2], "-") ? 0 : sys_open(argv[2], O_RDONLY);
    if (a < 0 || b < 0) { dprintf(2, "cmp: cannot open %s\n", a < 0 ? argv[1] : argv[2]); return 2; }
    struct rd ra, rb; rd_init(&ra, a); rd_init(&rb, b);
    u32 byte = 1, line = 1;
    for (;; byte++) {
        int x = rd_getc(&ra), y = rd_getc(&rb);
        if (x < 0 && y < 0) return 0;
        if (x < 0 || y < 0) { dprintf(1, "cmp: EOF on %s after byte %u\n", x < 0 ? argv[1] : argv[2], byte - 1); return 1; }
        if (x != y) { dprintf(1, "%s %s differ: byte %u, line %u\n", argv[1], argv[2], byte, line); return 1; }
        if (x == '\n') line++;
    }
}
