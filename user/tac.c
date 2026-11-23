// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* tac [file...]: the lines of each file (or standard input) in reverse order */
static int rc;
static void one(int fd, const char *name)
{
    struct rd r; rd_init(&r, fd);
    char **v = 0; u32 n = 0, cap = 0; char *line = 0, *tmp = 0; u32 lc = 0;
    while (rd_getline(&r, &line, &lc) >= 0) {
        if (n == cap) { u32 nc = cap ? cap * 2 : 64; char **nv = realloc(v, nc * sizeof *nv); if (!nv) { dprintf(2, "tac: %s: out of memory\n", name); rc = 1; break; } v = nv; cap = nc; }
        if (!(v[n] = strdup(line))) { dprintf(2, "tac: %s: out of memory\n", name); rc = 1; break; }
        n++;
    }
    (void)tmp;
    while (n) { oputs(v[--n]); oputc('\n'); free(v[n]); }
    free(v); free(line);
}
static const char USAGE[] = "usage: tac [file...]\n";
int main(int argc, char **argv)
{
    zoperands(&argc, &argv, USAGE);
    if (argc < 2) { one(0, "-"); return rc; }
    for (int i = 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? sys_open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { dprintf(2, "tac: %s: %s\n", argv[i], zwhy("cannot open")); rc = 1; continue; }
        one(fd, argv[i]); if (fd) sys_close(fd);
    }
    return rc;
}
