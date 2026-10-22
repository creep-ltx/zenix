#include "libz.h"
/* cmp file1 file2: the first byte where they differ */
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(2, "usage: cmp file1 file2\n"); return 2; }
    int a = sys_open(argv[1], O_RDONLY), b = sys_open(argv[2], O_RDONLY);
    if (a < 0 || b < 0) { fprintf(2, "cmp: cannot open %s\n", a < 0 ? argv[1] : argv[2]); return 2; }
    struct rd ra, rb; rd_init(&ra, a); rd_init(&rb, b);
    u32 byte = 1, line = 1;
    for (;; byte++) {
        int x = rd_getc(&ra), y = rd_getc(&rb);
        if (x < 0 && y < 0) return 0;
        if (x < 0 || y < 0) { printf("cmp: EOF on %s after byte %u\n", x < 0 ? argv[1] : argv[2], byte - 1); return 1; }
        if (x != y) { printf("%s %s differ: byte %u, line %u\n", argv[1], argv[2], byte, line); return 1; }
        if (x == '\n') line++;
    }
}
