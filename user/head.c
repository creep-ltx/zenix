/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
static const char USAGE[] = "usage: head [-n count | -count] [-c bytes] [file...]\n";
/* head [-n N | -N] [-c N] [file...]: the first lines (default 10) or bytes */
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    long nl = 10, nc = -1; int i = 1, rc = 0;
    for (; zisopt(argc, argv, &i, USAGE); i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) nl = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) nc = atoi(argv[++i]);
        else if (isdigit((unsigned char)argv[i][1])) nl = atoi(argv[i] + 1);
        else zbadopt(argv[i], USAGE);
    }
    int nfiles = argc - i;
    for (int f = 0; f < (nfiles ? nfiles : 1); f++) {
        int fd = 0; const char *name = "standard input";
        if (nfiles) { name = argv[i + f]; fd = sys_open(name, O_RDONLY); if (fd < 0) { dprintf(2, "head: %s: %s\n", name, zwhy("cannot open")); rc = 1; continue; } }
        if (nfiles > 1) oprintf("%s==> %s <==\n", f ? "\n" : "", name);
        struct rd r; rd_init(&r, fd);
        if (nc >= 0) { int c; for (long k = 0; k < nc && (c = rd_getc(&r)) >= 0; k++) oputc(c); }
        else {
            char *line = 0; u32 cap = 0;
            for (long k = 0; k < nl && rd_getline(&r, &line, &cap) >= 0; k++) { oputs(line); oputc('\n'); }
            free(line);
        }
        if (nfiles) sys_close(fd);
    }
    return rc;
}
