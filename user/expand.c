// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* expand [-t N] [file...]: replace tabs by spaces (tab stops every N columns, default 8) */
int main(int argc, char **argv)
{
    int tab = 8, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strncmp(argv[i], "-t", 2)) tab = argv[i][2] ? atoi(argv[i] + 2) : (i + 1 < argc ? atoi(argv[++i]) : 8);
        else { dprintf(2, "expand: unknown option %s\n", argv[i]); return 2; }
    }
    if (tab < 1) tab = 8;
    for (int a = i; a < argc || a == i; a++) {
        int fd = 0;
        if (a < argc) { fd = sys_open(argv[a], O_RDONLY); if (fd < 0) { dprintf(2, "expand: %s: %s\n", argv[a], zwhy("cannot open")); rc = 1; continue; } }
        struct rd r; rd_init(&r, fd); int c, col = 0;
        while ((c = rd_getc(&r)) >= 0) {
            if (c == '\t') { int n = tab - col % tab; while (n--) { oputc(' '); col++; } }
            else { oputc(c); col = c == '\n' ? 0 : c == '\b' && col ? col - 1 : col + 1; }
        }
        if (fd) sys_close(fd);
        if (a >= argc) break;
    }
    return rc;
}
