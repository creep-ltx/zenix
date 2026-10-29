// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* paste [-d list] [-s] file...: merge lines of files side by side (or one file per line with -s) */
int main(int argc, char **argv)
{
    const char *delims = "\t"; int serial = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-s")) serial = 1;
        else if (!strncmp(argv[i], "-d", 2)) delims = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "\t");
        else { dprintf(2, "paste: unknown option %s\n", argv[i]); return 2; }
    }
    static struct rd rds[8];                     /* 8 x 2 KB buffers: not on the 16 KB stack */
    int nf = argc - i; int fds[8], done[8];
    if (nf < 1 || nf > 8) { if (nf < 1) { nf = 1; fds[0] = 0; rd_init(&rds[0], 0); done[0] = 0; } else { dprintf(2, "paste: at most 8 files\n"); return 2; } }
    else for (int k = 0; k < nf; k++) {
        fds[k] = strcmp(argv[i + k], "-") ? sys_open(argv[i + k], O_RDONLY) : 0;
        if (fds[k] < 0) { dprintf(2, "paste: %s: %s\n", argv[i + k], zwhy("cannot open")); return 1; }
        rd_init(&rds[k], fds[k]); done[k] = 0;
    }
    int nd = (int)strlen(delims); if (!nd) { delims = "\t"; nd = 1; }
    char *line = 0; u32 cap = 0;
    if (serial) {
        for (int k = 0; k < nf; k++) { int first = 1, n = 0; while (rd_getline(&rds[k], &line, &cap) >= 0) { if (!first) oputc(delims[(n - 1) % nd]); oputs(line); first = 0; n++; } oputc('\n'); }
        return 0;
    }
    for (;;) {
        int alive = 0; static char *cells[8];
        for (int k = 0; k < nf; k++) {
            cells[k] = 0;
            if (!done[k]) { if (rd_getline(&rds[k], &line, &cap) >= 0) { if (!(cells[k] = strdup(line))) { dprintf(2, "paste: out of memory\n"); return 1; } alive = 1; } else done[k] = 1; }
        }
        if (!alive) break;
        for (int k = 0; k < nf; k++) { if (k) oputc(delims[(k - 1) % nd]); if (cells[k]) { oputs(cells[k]); free(cells[k]); } }
        oputc('\n');
    }
    return 0;
}
