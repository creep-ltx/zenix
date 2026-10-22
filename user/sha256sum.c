// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* sha256sum [file...]: the SHA-256 of each file (or standard input), as sha256sum prints it */
static int sum(int fd, const char *name)
{
    static unsigned char *data; static u32 cap;
    u32 n = 0; int r;
    if (!data) { cap = 65536; data = malloc(cap); }
    for (;;) {
        if (n + 4096 > cap) { unsigned char *nd = malloc(cap * 2); if (!nd) return -1; memcpy(nd, data, n); data = nd; cap *= 2; }
        if ((r = sys_read(fd, data + n, 4096)) <= 0) break;
        n += (u32)r;
    }
    unsigned char h[32];
    sha256(data, n, h);
    static const char hex[] = "0123456789abcdef";
    char s[65];
    for (int i = 0; i < 32; i++) { s[2*i] = hex[h[i] >> 4]; s[2*i+1] = hex[h[i] & 15]; }
    s[64] = 0;
    printf("%s  %s\n", s, name);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc < 2) return sum(0, "-") != 0;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(2, "sha256sum: %s: %s\n", argv[i], zwhy("cannot open")); rc = 1; continue; }
        if (sum(fd, argv[i])) rc = 1;
        sys_close(fd);
    }
    return rc;
}
