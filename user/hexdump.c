/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* hexdump -C [file]: canonical hex + ASCII dump (the only format) */
int main(int argc, char **argv)
{
    int i = 1; if (i < argc && !strcmp(argv[i], "-C")) i++;
    if (i < argc && !strcmp(argv[i], "--")) i++;
    else if (i < argc && argv[i][0] == '-' && argv[i][1]) { dprintf(2, "hexdump: unknown option %s\nusage: hexdump [-C] [file]\n", argv[i]); return 2; }
    int fd = 0; if (i < argc) { fd = sys_open(argv[i], O_RDONLY); if (fd < 0) { dprintf(2, "hexdump: %s: %s\n", argv[i], zwhy("cannot open")); return 1; } }
    unsigned char b[16]; u32 off = 0; int n;
    struct rd r; rd_init(&r, fd);
    for (;;) {
        n = 0; int c;
        while (n < 16 && (c = rd_getc(&r)) >= 0) b[n++] = (unsigned char)c;
        if (!n) break;
        oprintf("%08x  ", off);
        for (int k = 0; k < 16; k++) { if (k < n) oprintf("%02x ", b[k]); else oputs("   "); if (k == 7) oputc(' '); }
        oputs(" |");
        for (int k = 0; k < n; k++) oputc(b[k] >= 32 && b[k] < 127 ? b[k] : '.');
        oputs("|\n");
        off += (u32)n;
        if (n < 16) break;
    }
    oprintf("%08x\n", off);
    return 0;
}
