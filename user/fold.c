// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
static const char USAGE[] = "usage: fold [-w width] [-s] [file...]\n";
/* fold [-w width] [-s] [file...]: wrap long lines at width columns (default 80), with -s at the last blank */
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int width = 80, brk = 0, i = 1, rc = 0;
    for (; zisopt(argc, argv, &i, USAGE); i++) {
        if (!strcmp(argv[i], "-s")) brk = 1;
        else if (!strncmp(argv[i], "-w", 2)) width = argv[i][2] ? atoi(argv[i] + 2) : (i + 1 < argc ? atoi(argv[++i]) : 80);
        else if (isdigit((unsigned char)argv[i][1])) width = atoi(argv[i] + 1);
        else zbadopt(argv[i], USAGE);
    }
    if (width < 1) width = 1;
    for (int a = i; a < argc || a == i; a++) {
        int fd = 0;
        if (a < argc) { fd = sys_open(argv[a], O_RDONLY); if (fd < 0) { dprintf(2, "fold: %s: %s\n", argv[a], zwhy("cannot open")); rc = 1; continue; } }
        struct rd r; rd_init(&r, fd);
        char *line = 0; u32 cap = 0; int n;
        while ((n = rd_getline(&r, &line, &cap)) >= 0) {
            int pos = 0;
            while (n - pos > width) {
                int cut = width;
                if (brk) { int k = width; while (k > 0 && line[pos + k - 1] != ' ' && line[pos + k - 1] != '\t') k--; if (k > 0) cut = k; }
                for (int k = 0; k < cut; k++) oputc((unsigned char)line[pos + k]);
                oputc('\n'); pos += cut;
            }
            oputs(line + pos); oputc('\n');
        }
        if (fd) sys_close(fd);
        if (a >= argc) break;
    }
    return rc;
}
