/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* cat [-n] [-b] [-s] [-E] [file...] */
static int o_n, o_b, o_s, o_E, lineno, blank_run, at_start = 1;
static int dump(int fd)
{
    if (!o_n && !o_b && !o_s && !o_E) {                       /* plain: big blocks, untouched */
        char b[1024]; int n;
        while ((n = sys_read(fd, b, sizeof b)) > 0) if (sys_write(1, b, (u32)n) < 0) return 1;
        return n < 0;
    }
    struct rd r; rd_init(&r, fd); char *line = 0; u32 cap = 0; int len;
    while ((len = rd_getline(&r, &line, &cap)) >= 0) {
        if (len == 0) { blank_run++; if (o_s && blank_run > 1) continue; } else blank_run = 0;
        if (o_n || (o_b && len)) oprintf("%6d\t", ++lineno);
        oputs(line); if (o_E) oputc('$'); oputc('\n');
    }
    free(line);
    (void)at_start;
    return 0;
}
int main(int argc, char **argv)
{
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'n') o_n = 1; else if (*p == 'b') o_b = 1; else if (*p == 's') o_s = 1; else if (*p == 'E') o_E = 1; else { dprintf(2, "cat: unknown option -%c\n", *p); return 2; } }
    if (o_b) o_n = 0;
    if (i >= argc) return dump(0);
    for (; i < argc; i++) {
        int fd = !strcmp(argv[i], "-") ? 0 : sys_open(argv[i], O_RDONLY);
        if (fd < 0) { dprintf(2, "cat: %s: %s\n", argv[i], zwhy("cannot open")); rc = 1; continue; }
        rc |= dump(fd); if (fd) sys_close(fd);
    }
    return rc;
}
