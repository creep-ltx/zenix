/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* uniq [-c] [-d] [-u] [-i] [input [output]]: collapse adjacent duplicate lines */
static int o_c, o_d, o_u, o_i;
static int same(const char *a, const char *b)
{
    if (!o_i) return !strcmp(a, b);
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return *a == *b;
}
static const char USAGE[] = "usage: uniq [-cdui] [input [output]]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int i = 1;
    for (; zisopt(argc, argv, &i, USAGE); i++)
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'c') o_c = 1; else if (*p == 'd') o_d = 1; else if (*p == 'u') o_u = 1; else if (*p == 'i') o_i = 1; else zbadch(*p, USAGE); }
    int fd = 0;
    if (i < argc) { fd = sys_open(argv[i], O_RDONLY); if (fd < 0) { dprintf(2, "uniq: %s: %s\n", argv[i], zwhy("cannot open")); return 1; } }
    struct rd r; rd_init(&r, fd);
    char *line = 0, *prev = 0; u32 cap = 0; long count = 0;
#define FLUSH() do { if (prev && (!o_d || count > 1) && (!o_u || count == 1)) { if (o_c) oprintf("%7d ", (int)count); oputs(prev); oputc('\n'); } } while (0)
    while (rd_getline(&r, &line, &cap) >= 0) {
        if (prev && same(prev, line)) { count++; continue; }
        FLUSH();
        free(prev); prev = strdup(line); count = 1;
        if (!prev) { dprintf(2, "uniq: out of memory\n"); return 1; }
    }
    FLUSH();
    return 0;
}
