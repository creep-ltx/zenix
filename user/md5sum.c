// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* md5sum [file...]: the MD5 checksum of each file (or standard input), as GNU md5sum prints it. MD5 is not safe against
 * deliberate collisions; use sha256sum for anything that matters. */
struct md5 { u32 a, b, c, d, n; unsigned char buf[64]; u32 len_lo, len_hi; };
static const u32 K[64] = {
 0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
 0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
 0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
 0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
static const unsigned char S[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20, 4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
#define ROL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))
static void block(struct md5 *m, const unsigned char *p)
{
    u32 w[16], a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4*i] | (u32)p[4*i+1] << 8 | (u32)p[4*i+2] << 16 | (u32)p[4*i+3] << 24;
    for (int i = 0; i < 64; i++) {
        u32 f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; } else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; } else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        u32 t = d; d = c; c = b; b = b + ROL(a + f + K[i] + w[g], S[i]); a = t;
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}
static void add(struct md5 *m, const unsigned char *p, u32 n)
{
    u32 old = m->len_lo; m->len_lo += n; if (m->len_lo < old) m->len_hi++;
    while (n--) { m->buf[m->n++] = *p++; if (m->n == 64) { block(m, m->buf); m->n = 0; } }
}
static void sum(int fd, const char *name)
{
    struct md5 m; memset(&m, 0, sizeof m); m.a = 0x67452301; m.b = 0xefcdab89; m.c = 0x98badcfe; m.d = 0x10325476;
    unsigned char buf[1024]; int n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) add(&m, buf, (u32)n);
    u32 bits_lo = m.len_lo << 3, bits_hi = (m.len_hi << 3) | (m.len_lo >> 29);
    unsigned char pad = 0x80; add(&m, &pad, 1);
    pad = 0; while (m.n != 56) add(&m, &pad, 1);
    unsigned char l[8] = { (unsigned char)bits_lo, (unsigned char)(bits_lo >> 8), (unsigned char)(bits_lo >> 16), (unsigned char)(bits_lo >> 24),
                           (unsigned char)bits_hi, (unsigned char)(bits_hi >> 8), (unsigned char)(bits_hi >> 16), (unsigned char)(bits_hi >> 24) };
    add(&m, l, 8);
    u32 v[4] = { m.a, m.b, m.c, m.d };
    static const char hex[] = "0123456789abcdef"; char s[33];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { unsigned char byte = (unsigned char)(v[i] >> (8 * j)); s[8*i + 2*j] = hex[byte >> 4]; s[8*i + 2*j + 1] = hex[byte & 15]; }
    s[32] = 0; printf("%s  %s\n", s, name);
}
int main(int argc, char **argv)
{
    if (argc < 2) { sum(0, "-"); return 0; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? sys_open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { fprintf(2, "md5sum: %s: %s\n", argv[i], zwhy("cannot open")); rc = 1; continue; }
        sum(fd, argv[i]); if (fd) sys_close(fd);
    }
    return rc;
}
