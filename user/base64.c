// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* base64 [-d] [-w columns] [file]: encode to base64 (lines of 76 characters), or decode with -d */
static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int main(int argc, char **argv)
{
    int dec = 0, wrap = 76, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--decode")) dec = 1;
        else if (!strncmp(argv[i], "-w", 2)) wrap = argv[i][2] ? atoi(argv[i] + 2) : (i + 1 < argc ? atoi(argv[++i]) : 76);
        else { dprintf(2, "base64: unknown option %s\n", argv[i]); return 2; }
    }
    int fd = 0;
    if (i < argc && strcmp(argv[i], "-")) { fd = sys_open(argv[i], O_RDONLY); if (fd < 0) { dprintf(2, "base64: %s: %s\n", argv[i], zwhy("cannot open")); return 1; } }
    struct rd r; rd_init(&r, fd);
    if (!dec) {
        int col = 0, c, n = 0; unsigned char b[3];
        #define OUT(ch) do { oputc(ch); if (wrap && ++col == wrap) { oputc('\n'); col = 0; } } while (0)
        while ((c = rd_getc(&r)) >= 0) {
            b[n++] = (unsigned char)c;
            if (n == 3) { OUT(tab[b[0] >> 2]); OUT(tab[(b[0] & 3) << 4 | b[1] >> 4]); OUT(tab[(b[1] & 15) << 2 | b[2] >> 6]); OUT(tab[b[2] & 63]); n = 0; }
        }
        if (n) { OUT(tab[b[0] >> 2]); OUT(tab[(b[0] & 3) << 4 | (n > 1 ? b[1] >> 4 : 0)]); OUT(n > 1 ? tab[(b[1] & 15) << 2] : '='); OUT('='); }
        if (col) oputc('\n');
        return 0;
    }
    int c, acc = 0, bits = 0;
    while ((c = rd_getc(&r)) >= 0) {
        const char *p = strchr(tab, c);
        if (c == '=' ) break;
        if (!p || !c) continue;                                  /* newlines and other characters are skipped */
        acc = acc << 6 | (int)(p - tab); bits += 6;
        if (bits >= 8) { bits -= 8; oputc((acc >> bits) & 255); }
    }
    return 0;
}
