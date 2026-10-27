/* SPDX-License-Identifier: GPL-2.0-only */
/* ssh's wire format (RFC 4251 5): the checked reader, the writer, name-lists, base64, and the text cleaner of
   DESIGN D21 4.3. */
#include <string.h>
#include "ssh_int.h"

unsigned rd_u8(struct rd *r)
{
    if (r->bad || r->n < 1) {
        r->bad = 1;
        return 0;
    }
    r->n--;
    return *r->p++;
}

unsigned long rd_u32(struct rd *r)
{
    unsigned long v;

    if (r->bad || r->n < 4) {
        r->bad = 1;
        return 0;
    }
    v = (unsigned long)r->p[0] << 24 | (unsigned long)r->p[1] << 16 | (unsigned long)r->p[2] << 8 | r->p[3];
    r->p += 4;
    r->n -= 4;
    return v;
}

const unsigned char *rd_str(struct rd *r, long *len)
{
    unsigned long l = rd_u32(r);
    const unsigned char *p;

    if (r->bad || l > (unsigned long)r->n) {
        r->bad = 1;
        *len = 0;
        return 0;
    }
    p = r->p;
    r->p += l;
    r->n -= (long)l;
    *len = (long)l;
    return p;
}

int rd_bool(struct rd *r)
{
    return rd_u8(r) != 0;
}

void wr_bytes(struct wr *w, const void *b, long n)
{
    if (w->bad || n < 0 || n > w->cap - w->n) {
        w->bad = 1;
        return;
    }
    if (n)
        memcpy(w->p + w->n, b, (size_t)n);
    w->n += n;
}

void wr_u8(struct wr *w, unsigned v)
{
    unsigned char b = (unsigned char)v;

    wr_bytes(w, &b, 1);
}

void wr_u32(struct wr *w, unsigned long v)
{
    unsigned char b[4];

    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)v;
    wr_bytes(w, b, 4);
}

void wr_str(struct wr *w, const void *b, long n)
{
    wr_u32(w, (unsigned long)n);
    wr_bytes(w, b, n);
}

void wr_cstr(struct wr *w, const char *s)
{
    wr_str(w, s, (long)strlen(s));
}

int nl_has(const unsigned char *list, long n, const char *name)
{
    long i = 0, k = (long)strlen(name);

    while (i <= n) {
        long j = i;

        while (j < n && list[j] != ',')
            j++;
        if (j - i == k && memcmp(list + i, name, (size_t)k) == 0)
            return 1;
        i = j + 1;
    }
    return 0;
}

/* ---- base64 (RFC 4648) ---- */
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

long ssh_b64_decode(unsigned char *out, long cap, const char *in, long n)
{
    unsigned long acc = 0;
    int bits = 0, pad = 0;
    long i, o = 0;

    for (i = 0; i < n; i++) {
        int c = (unsigned char)in[i], v;

        if (c == '\n' || c == '\r')     /* the PEM armour's line breaks */
            continue;
        if (c == '=') {
            pad++;
            continue;
        }
        if (pad)                        /* data after padding */
            return -1;
        v = b64v(c);
        if (v < 0)
            return -1;
        acc = (acc << 6) | (unsigned long)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap)
                return -1;
            out[o++] = (unsigned char)(acc >> bits);
        }
    }
    if (pad > 2 || bits >= 6 || (acc & ((1UL << bits) - 1)))  /* a stray character, or bits left over */
        return -1;
    return o;
}

long ssh_b64_encode(char *out, long cap, const unsigned char *in, long n, int pad)
{
    long i, o = 0;

    for (i = 0; i < n; i += 3) {
        unsigned long v = (unsigned long)in[i] << 16;
        int k = n - i < 3 ? (int)(n - i) : 3, j;

        if (k > 1) v |= (unsigned long)in[i + 1] << 8;
        if (k > 2) v |= in[i + 2];
        for (j = 0; j < 4; j++) {
            if (j > k) {
                if (!pad)
                    break;
                if (o >= cap)
                    return -1;
                out[o++] = '=';
                continue;
            }
            if (o >= cap)
                return -1;
            out[o++] = b64[(v >> (18 - 6 * j)) & 63];
        }
    }
    return o;
}

/* ---- the cleaner (D21 4.3) ---- */
long ssh_clean(char *dst, const char *src, long n)
{
    const unsigned char *s = (const unsigned char *)src;
    long i = 0, o = 0;

    while (i < n) {
        unsigned c = s[i];
        unsigned long cp;
        int len, k;

        if (c < 0x80) {
            if (c == '\r' && i + 1 < n && s[i + 1] == '\n') {     /* CR LF: the LF alone */
                i++;
                continue;
            }
            dst[o++] = (c == '\t' || c == '\n' || (c >= 0x20 && c < 0x7f)) ? (char)c : '?';
            i++;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) { len = 2; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { len = 3; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { len = 4; cp = c & 0x07; }
        else { dst[o++] = '?'; i++; continue; }
        if (i + len > n) { dst[o++] = '?'; i++; continue; }
        for (k = 1; k < len; k++) {
            if ((s[i + k] & 0xc0) != 0x80)
                break;
            cp = (cp << 6) | (s[i + k] & 0x3f);
        }
        if (k < len || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
            (cp >= 0xd800 && cp <= 0xdfff) || (cp >= 0x80 && cp <= 0x9f)) {
            dst[o++] = '?';             /* invalid, overlong, a surrogate, or a C1 control */
            i++;
            continue;
        }
        for (k = 0; k < len; k++)       /* (o <= i always: the copy never overtakes the reading) */
            dst[o++] = (char)s[i + k];
        i += len;
    }
    return o;
}
