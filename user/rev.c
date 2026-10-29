// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* rev [file...]: reverse the characters of every line */
static int rc;
static void one(int fd)
{
    struct rd r; rd_init(&r, fd);
    char *line = 0; u32 cap = 0; int n;
    while ((n = rd_getline(&r, &line, &cap)) >= 0) { for (int i = n - 1; i >= 0; i--) oputc((unsigned char)line[i]); oputc('\n'); }
}
int main(int argc, char **argv)
{
    if (argc < 2) { one(0); return 0; }
    for (int i = 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? sys_open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { dprintf(2, "rev: %s: %s\n", argv[i], zwhy("cannot open")); rc = 1; continue; }
        one(fd); if (fd) sys_close(fd);
    }
    return rc;
}
