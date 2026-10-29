/* SPDX-License-Identifier: GPL-2.0-only */
/* telnet's keys (DESIGN D20 3.3): what the console gives -> what the board gets, per mode. DESIGN D28 12: the Unix
 * mode passes the keys through (the console sends UTF-8 itself); a BBS mode whose console sends UTF-8 (no 8-bit mode:
 * the PC font refused, the rules refused) decodes them first, so a Swedish letter is one Latin-1 key again. */
#include "telnet.h"

static unsigned char held[4];           /* a UTF-8 character split between two calls (one telnet a process) */
static int nheld;

/* the character at in (n bytes, the held ones first): its bytes (0: wait for more), the code point in *cp; a byte that
 * is not UTF-8 is itself (Latin-1) */
static int u8(const unsigned char *in, long n, unsigned long *cp)
{
    unsigned char c = in[0];
    int k = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 1;
    unsigned char lo = c == 0xE0 ? 0xA0 : c == 0xF0 ? 0x90 : 0x80, hi = c == 0xED ? 0x9F : c == 0xF4 ? 0x8F : 0xBF;
    *cp = c;
    if (k == 1)
        return 1;
    for (int i = 1; i < k; i++) {
        if (i >= n)
            return 0;
        if (i == 1 ? in[1] < lo || in[1] > hi : (in[i] & 0xC0) != 0x80)
            return 1;
    }
    *cp = k == 2 ? (unsigned long)(c & 0x1F) << 6 | (in[1] & 0x3F) :
          k == 3 ? (unsigned long)(c & 0x0F) << 12 | (unsigned long)(in[1] & 0x3F) << 6 | (in[2] & 0x3F) :
          (unsigned long)(c & 0x07) << 18 | (unsigned long)(in[1] & 0x3F) << 12 | (unsigned long)(in[2] & 0x3F) << 6 |
          (in[3] & 0x3F);
    return k;
}

/* one key's code point for a BBS mode -> out; returns the bytes written */
static long bbs_key(int mode, unsigned long c, unsigned char *out, int *bell)
{
    if (c >= 0x80 && mode == TN_PC) {
        unsigned char p = c <= 0xFF ? tn_latin1_cp437[c] : 0;
        for (int b = 0x80; !p && c > 0xFF && b < 0x100; b++)
            if (tn_cp437_ucs[b] == c)
                p = (unsigned char)b;           /* a box line or symbol pasted as UTF-8: its CP437 byte */
        if (p)
            out[0] = p;
        else if (bell)
            (*bell)++;                  /* no CP437 form: not sent */
        return p != 0;
    }
    if (c > 0xFF) {                     /* an Amiga board: Latin-1 only */
        if (bell)
            (*bell)++;
        return 0;
    }
    out[0] = (unsigned char)c;          /* arrows, F-keys, Help, controls, Latin-1 to an Amiga board: as given */
    return 1;
}

long tk_keys(int mode, int utf8, const unsigned char *in, long n, unsigned char *out, int *bell)
{
    long i, k = 0;
    if (mode == TN_UNIX) {
        for (i = 0; i < n; i++)
            out[k++] = in[i];           /* Backspace 0x7F, Del ESC[3~, UTF-8: everything as given */
        return k;
    }
    if (n > 64) {                       /* the held bytes and a piece fit the buffer below */
        k = tk_keys(mode, utf8, in, 64, out, bell);
        return k + tk_keys(mode, utf8, in + 64, n - 64, out + k, bell);
    }
    unsigned char b[68];                /* D28: a character held from the last call, then this call's keys */
    long m = 0;
    for (int h = 0; h < nheld; h++)
        b[m++] = held[h];
    nheld = 0;
    for (i = 0; i < n; i++)
        b[m++] = in[i];
    for (i = 0; i < m; i++) {
        unsigned char c = b[i];
        if (c == 0x7F) {
            out[k++] = 0x08;            /* Backspace: BS (boards expect it) */
            continue;
        }
        if (c == 0x1B && i + 3 < m && b[i + 1] == '[' && b[i + 2] == '3' && b[i + 3] == '~') {
            out[k++] = 0x7F;            /* Del (ESC[3~, whole in this read): DEL */
            i += 3;
            continue;
        }
        if (c >= 0x80 && utf8) {        /* D28: the console's UTF-8, decoded */
            unsigned long cp;
            int len = u8(b + i, m - i, &cp);
            if (!len) {                 /* the rest comes with the next call */
                while (i < m)
                    held[nheld++] = b[i++];
                return k;
            }
            i += len - 1;
            k += bbs_key(mode, cp, out + k, bell);
            continue;
        }
        k += bbs_key(mode, c, out + k, bell);
    }
    return k;
}
