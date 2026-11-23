/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
static const char USAGE[] = "usage: tee [-a] [file...]\n";
/* tee [-a] [file...]: copy standard input to standard output and the files */
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int app = 0, i = 1, fds[8], nf = 0, rc = 0;
    if (i < argc && !strcmp(argv[i], "-a")) { app = 1; i++; }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    else if (zisopt(argc, argv, &i, USAGE)) { zbadopt(argv[i], USAGE); }
    if (argc - i > 8) { dprintf(2, "tee: at most 8 files\n"); return 2; }    /* a limit that is hit says so */
    for (; i < argc; i++) { int fd = sys_open(argv[i], O_WRONLY | O_CREAT | (app ? O_APPEND : O_TRUNC)); if (fd < 0) { dprintf(2, "tee: %s: %s\n", argv[i], zwhy("cannot write")); rc = 1; } else fds[nf++] = fd; }
    char b[4096]; int n;                    /* 4 KB a call (audit 2026-11-23 OS-15) */
    while ((n = sys_read(0, b, sizeof b)) > 0) { sys_write(1, b, (u32)n); for (int k = 0; k < nf; k++) sys_write(fds[k], b, (u32)n); }
    for (int k = 0; k < nf; k++) sys_close(fds[k]);
    return rc;
}
