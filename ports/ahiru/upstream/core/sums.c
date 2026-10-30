/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* sums - MD5 (RFC 1321) and the checksum files. See sums.h. Plain C. */
#include <string.h>
#include "sums.h"

/* the four round functions, as RFC 1321 has them (F and G in the form
 * with one operation fewer), and one step written out: 64 steps in a
 * row, no table of shifts and no four-way choice each step - about
 * twice as fast on a 68000. unsigned int: 32 bits on the Amiga and on
 * the PC that runs the tests alike */
#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) ((y) ^ ((z) & ((x) ^ (y))))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define STEP(f, a, b, c, d, x, t, s) \
    (a) += f((b), (c), (d)) + (x) + (t), (a) = ((a) << (s)) | ((a) >> (32 - (s))), (a) += (b)

static void block(struct md5 *m, const unsigned char *p)
{
    unsigned int w[16], a = (unsigned int)m->a, b = (unsigned int)m->b, c = (unsigned int)m->c, d = (unsigned int)m->d;
    int i;
    /* the words are little-endian, whatever the machine */
    for (i = 0; i < 16; i++)
        w[i] = p[i * 4] | (unsigned int)p[i * 4 + 1] << 8 | (unsigned int)p[i * 4 + 2] << 16 |
               (unsigned int)p[i * 4 + 3] << 24;
    STEP(F, a, b, c, d, w[0], 0xd76aa478U, 7);
    STEP(F, d, a, b, c, w[1], 0xe8c7b756U, 12);
    STEP(F, c, d, a, b, w[2], 0x242070dbU, 17);
    STEP(F, b, c, d, a, w[3], 0xc1bdceeeU, 22);
    STEP(F, a, b, c, d, w[4], 0xf57c0fafU, 7);
    STEP(F, d, a, b, c, w[5], 0x4787c62aU, 12);
    STEP(F, c, d, a, b, w[6], 0xa8304613U, 17);
    STEP(F, b, c, d, a, w[7], 0xfd469501U, 22);
    STEP(F, a, b, c, d, w[8], 0x698098d8U, 7);
    STEP(F, d, a, b, c, w[9], 0x8b44f7afU, 12);
    STEP(F, c, d, a, b, w[10], 0xffff5bb1U, 17);
    STEP(F, b, c, d, a, w[11], 0x895cd7beU, 22);
    STEP(F, a, b, c, d, w[12], 0x6b901122U, 7);
    STEP(F, d, a, b, c, w[13], 0xfd987193U, 12);
    STEP(F, c, d, a, b, w[14], 0xa679438eU, 17);
    STEP(F, b, c, d, a, w[15], 0x49b40821U, 22);
    STEP(G, a, b, c, d, w[1], 0xf61e2562U, 5);
    STEP(G, d, a, b, c, w[6], 0xc040b340U, 9);
    STEP(G, c, d, a, b, w[11], 0x265e5a51U, 14);
    STEP(G, b, c, d, a, w[0], 0xe9b6c7aaU, 20);
    STEP(G, a, b, c, d, w[5], 0xd62f105dU, 5);
    STEP(G, d, a, b, c, w[10], 0x02441453U, 9);
    STEP(G, c, d, a, b, w[15], 0xd8a1e681U, 14);
    STEP(G, b, c, d, a, w[4], 0xe7d3fbc8U, 20);
    STEP(G, a, b, c, d, w[9], 0x21e1cde6U, 5);
    STEP(G, d, a, b, c, w[14], 0xc33707d6U, 9);
    STEP(G, c, d, a, b, w[3], 0xf4d50d87U, 14);
    STEP(G, b, c, d, a, w[8], 0x455a14edU, 20);
    STEP(G, a, b, c, d, w[13], 0xa9e3e905U, 5);
    STEP(G, d, a, b, c, w[2], 0xfcefa3f8U, 9);
    STEP(G, c, d, a, b, w[7], 0x676f02d9U, 14);
    STEP(G, b, c, d, a, w[12], 0x8d2a4c8aU, 20);
    STEP(H, a, b, c, d, w[5], 0xfffa3942U, 4);
    STEP(H, d, a, b, c, w[8], 0x8771f681U, 11);
    STEP(H, c, d, a, b, w[11], 0x6d9d6122U, 16);
    STEP(H, b, c, d, a, w[14], 0xfde5380cU, 23);
    STEP(H, a, b, c, d, w[1], 0xa4beea44U, 4);
    STEP(H, d, a, b, c, w[4], 0x4bdecfa9U, 11);
    STEP(H, c, d, a, b, w[7], 0xf6bb4b60U, 16);
    STEP(H, b, c, d, a, w[10], 0xbebfbc70U, 23);
    STEP(H, a, b, c, d, w[13], 0x289b7ec6U, 4);
    STEP(H, d, a, b, c, w[0], 0xeaa127faU, 11);
    STEP(H, c, d, a, b, w[3], 0xd4ef3085U, 16);
    STEP(H, b, c, d, a, w[6], 0x04881d05U, 23);
    STEP(H, a, b, c, d, w[9], 0xd9d4d039U, 4);
    STEP(H, d, a, b, c, w[12], 0xe6db99e5U, 11);
    STEP(H, c, d, a, b, w[15], 0x1fa27cf8U, 16);
    STEP(H, b, c, d, a, w[2], 0xc4ac5665U, 23);
    STEP(I, a, b, c, d, w[0], 0xf4292244U, 6);
    STEP(I, d, a, b, c, w[7], 0x432aff97U, 10);
    STEP(I, c, d, a, b, w[14], 0xab9423a7U, 15);
    STEP(I, b, c, d, a, w[5], 0xfc93a039U, 21);
    STEP(I, a, b, c, d, w[12], 0x655b59c3U, 6);
    STEP(I, d, a, b, c, w[3], 0x8f0ccc92U, 10);
    STEP(I, c, d, a, b, w[10], 0xffeff47dU, 15);
    STEP(I, b, c, d, a, w[1], 0x85845dd1U, 21);
    STEP(I, a, b, c, d, w[8], 0x6fa87e4fU, 6);
    STEP(I, d, a, b, c, w[15], 0xfe2ce6e0U, 10);
    STEP(I, c, d, a, b, w[6], 0xa3014314U, 15);
    STEP(I, b, c, d, a, w[13], 0x4e0811a1U, 21);
    STEP(I, a, b, c, d, w[4], 0xf7537e82U, 6);
    STEP(I, d, a, b, c, w[11], 0xbd3af235U, 10);
    STEP(I, c, d, a, b, w[2], 0x2ad7d2bbU, 15);
    STEP(I, b, c, d, a, w[9], 0xeb86d391U, 21);
    m->a = (m->a + a) & 0xFFFFFFFFUL;
    m->b = (m->b + b) & 0xFFFFFFFFUL;
    m->c = (m->c + c) & 0xFFFFFFFFUL;
    m->d = (m->d + d) & 0xFFFFFFFFUL;
}

void md5_init(struct md5 *m)
{
    m->a = 0x67452301;
    m->b = 0xefcdab89;
    m->c = 0x98badcfe;
    m->d = 0x10325476;
    m->lo = m->hi = 0;
}

void md5_add(struct md5 *m, const void *data, long n)
{
    const unsigned char *p = data;
    int used = (int)(m->lo & 63);
    unsigned long old = m->lo;
    m->lo = (m->lo + (unsigned long)n) & 0xFFFFFFFFUL;
    if (m->lo < old) m->hi++;
    if (used) {
        int take = 64 - used;
        if (take > n) take = (int)n;
        memcpy(m->buf + used, p, take);
        p += take;
        n -= take;
        if (used + take < 64) return;
        block(m, m->buf);
    }
    for (; n >= 64; n -= 64, p += 64) block(m, p);
    if (n) memcpy(m->buf, p, n);
}

void md5_end(struct md5 *m, unsigned char out[16])
{
    static const unsigned char pad[64] = { 0x80 };
    unsigned char len[8];
    unsigned long lo = m->lo << 3, hi = (m->hi << 3) | (m->lo >> 29);
    int i, used = (int)(m->lo & 63);
    for (i = 0; i < 4; i++) {
        len[i] = (unsigned char)(lo >> (8 * i));
        len[i + 4] = (unsigned char)(hi >> (8 * i));
    }
    md5_add(m, pad, used < 56 ? 56 - used : 120 - used);
    md5_add(m, len, 8);
    for (i = 0; i < 4; i++) {
        out[i] = (unsigned char)(m->a >> (8 * i));
        out[i + 4] = (unsigned char)(m->b >> (8 * i));
        out[i + 8] = (unsigned char)(m->c >> (8 * i));
        out[i + 12] = (unsigned char)(m->d >> (8 * i));
    }
}

void sum_hex(const unsigned char *b, int n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) {
        out[i * 2] = hx[b[i] >> 4];
        out[i * 2 + 1] = hx[b[i] & 15];
    }
    out[n * 2] = 0;
}

static int ishex(const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || ((s[i] | 32) >= 'a' && (s[i] | 32) <= 'f'))) return 0;
    return 1;
}

static void lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s += 32;
}

int sum_parse(char *line, int kind, char **name, char **want)
{
    int l = (int)strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ' || line[l - 1] == '\t'))
        line[--l] = 0;
    while (*line == ' ' || *line == '\t') line++, l--;
    if (!l || *line == ';' || *line == '#') return 0;
    if (kind == SUM_MD5) {
        int esc = 0;
        /* BSD (md5 -r's opposite, macOS, FreeBSD): "MD5 (name) = hash" */
        if (l > 40 && !strncmp(line, "MD5 (", 5) && !strncmp(line + l - 36, ") = ", 4) && ishex(line + l - 32, 32)) {
            line[l - 36] = 0;
            *name = line + 5;
            *want = line + l - 32;
            if (!**name) return -1;
            lower(*want);
            return 1;
        }
        /* md5sum puts a '\' first when the name holds a '\' or a newline,
         * written as \\ and \n */
        if (*line == '\\') esc = 1, line++, l--;
        /* "hash  name" or "hash *name" */
        if (l < 34 || !ishex(line, 32) || line[32] != ' ') return -1;
        line[32] = 0;
        *name = line + 33;
        if (**name == ' ' || **name == '*') (*name)++;
        if (!**name) return -1;
        if (esc) {
            char *r = *name, *w = *name;
            for (; *r; r++) {
                if (*r == '\\' && r[1] == '\\') *w++ = '\\', r++;
                else if (*r == '\\' && r[1] == 'n') *w++ = '\n', r++;
                else *w++ = *r;
            }
            *w = 0;
        }
        lower(line);
        *want = line;
        return 1;
    }
    /* "name crc": the last word */
    if (l < 10 || !ishex(line + l - 8, 8) || (line[l - 9] != ' ' && line[l - 9] != '\t')) return -1;
    *want = line + l - 8;
    l -= 9;
    while (l && (line[l - 1] == ' ' || line[l - 1] == '\t')) l--;
    line[l] = 0;
    if (!l) return -1;
    lower(*want);
    *name = line;
    /* SFV from Windows: "dir\file" is a path */
    for (; *line; line++)
        if (*line == '\\') *line = '/';
    return 1;
}
