// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* sha256sum [file...]: the SHA-256 of each file (or standard input), as sha256sum prints it */
static int sum(int fd, const char *name)
{
    static unsigned char buf[4096];             /* a piece at a time: any size of file (it read the whole file into
                                                 * memory before, and a file over the free memory gave nothing) */
    struct sha256 c;
    int r;
    sha256_init(&c);
    while ((r = sys_read(fd, buf, sizeof buf)) > 0)
        sha256_update(&c, buf, (u32)r);
    if (r < 0) {
        dprintf(2, "sha256sum: %s: %s\n", name, zstrerror(r) ? zstrerror(r) : "cannot read");
        return -1;
    }
    unsigned char h[32];
    sha256_final(&c, h);
    static const char hex[] = "0123456789abcdef";
    char s[65];
    for (int i = 0; i < 32; i++) { s[2*i] = hex[h[i] >> 4]; s[2*i+1] = hex[h[i] & 15]; }
    s[64] = 0;
    dprintf(1, "%s  %s\n", s, name);
    return 0;
}
static const char USAGE[] = "usage: sha256sum [file...]\n";
int main(int argc, char **argv)
{
    zoperands(&argc, &argv, USAGE);
    if (argc < 2) return sum(0, "-") != 0;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) { dprintf(2, "sha256sum: %s: %s\n", argv[i], zwhy("cannot open")); rc = 1; continue; }
        if (sum(fd, argv[i])) rc = 1;
        sys_close(fd);
    }
    return rc;
}
