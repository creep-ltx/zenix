// SPDX-License-Identifier: GPL-2.0-only
/* What the LhA and LZX readers share (DESIGN D33): the Huffman tables and members' names. Cold code. */
#include "arcread.h"

int arc_list_refused;

unsigned arc_rev16(unsigned v)
{
    v = ((v & 0x5555) << 1) | ((v >> 1) & 0x5555);
    v = ((v & 0x3333) << 2) | ((v >> 2) & 0x3333);
    v = ((v & 0x0F0F) << 4) | ((v >> 4) & 0x0F0F);
    return ((v & 0x00FF) << 8) | ((v >> 8) & 0x00FF);
}

int hf_build(struct hf *h, const arc_u8 *lens, int n)
{
    int i, l, size = 1 << h->kbits;
    arc_u32 code = 0, kraft = 0;
    arc_u16 next[17];
    h->single = -1;
    for (l = 0; l <= 16; l++) h->cnt[l] = 0;
    for (i = 0; i < n; i++) {
        if (lens[i] > 16) return ARC_ECORRUPT;
        h->cnt[lens[i]]++;
    }
    h->cnt[0] = 0;
    {
        arc_u16 o = 0;
        for (l = 1; l <= 16; l++) {
            code <<= 1;
            h->first[l] = code;
            h->off[l] = o;
            next[l] = o;
            o = (arc_u16)(o + h->cnt[l]);
            code += h->cnt[l];
            kraft += (arc_u32)h->cnt[l] << (16 - l);
        }
    }
    if (kraft > 0x10000) return ARC_ECORRUPT;       /* more codes than the bits allow */
    for (i = 0; i < n; i++) if (lens[i]) h->syms[next[lens[i]]++] = (arc_u16)i;
    for (i = 0; i < size; i++) h->tab[i] = 0;
    for (l = 1; l <= h->kbits; l++) {
        int c;
        for (c = 0; c < h->cnt[l]; c++) {
            unsigned cw = h->first[l] + (unsigned)c;
            arc_u16 v = (arc_u16)((l << 12) | h->syms[h->off[l] + c]);
            int k;
            if (h->rev) {
                unsigned idx = arc_rev16(cw << (16 - l));   /* its first bit lowest */
                for (k = (int)idx; k < size; k += 1 << l) h->tab[k] = v;
            } else {
                unsigned idx = cw << (h->kbits - l);
                for (k = 0; k < 1 << (h->kbits - l); k++) h->tab[idx + (unsigned)k] = v;
            }
        }
    }
    return 0;
}

int hf_long(const struct hf *h, unsigned v, int *len)
{
    int l;
    for (l = h->kbits + 1; l <= 16; l++) {
        unsigned c = v >> (16 - l);
        if (c >= h->first[l] && c - h->first[l] < h->cnt[l]) {
            *len = l;
            return h->syms[h->off[l] + (c - h->first[l])];
        }
    }
    return -1;
}

/* the length of a whole UTF-8 sequence at s (n bytes left), 0 when it is not one (overlong, surrogate, past U+10FFFF) */
static int u8seq(const unsigned char *s, int n)
{
    int k, i;
    unsigned c = s[0], u;
    if (c < 0xC2 || c > 0xF4) return 0;
    k = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (n < k) return 0;
    u = c & (0xFF >> (k + 1));
    for (i = 1; i < k; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        u = (u << 6) | (s[i] & 0x3F);
    }
    if ((k == 3 && u < 0x800) || (k == 4 && u < 0x10000) || u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) return 0;
    return k;
}

int arc_fixname(const unsigned char *in, int n, char *out, int cap)
{
    int i, w = 0, bad = 0, utf8 = 1, partstart;
    if (n > 0 && in[0] == '/') bad = 1;
    for (i = 0; i < n; ) {
        int k;
        if (in[i] < 0x80) { i++; continue; }
        k = u8seq(in + i, n - i);
        if (!k) { utf8 = 0; break; }
        i += k;
    }
    partstart = 0;
    for (i = 0; i <= n; i++) {
        unsigned c = i < n ? in[i] : '/';
        if (c == '/') {
            int plen = w - partstart;
            if (plen == 0) continue;
            if (plen == 1 && out[partstart] == '.') { w = partstart; continue; }
            if (plen == 2 && out[partstart] == '.' && out[partstart + 1] == '.') bad = 1;
            if (i < n) {
                if (w + 1 >= cap) { bad = 1; break; }
                out[w++] = '/';
            }
            partstart = w;
            continue;
        }
        if (c < 0x20 || c == 0x7F) c = '_';
        if (utf8 && c == 0xC2 && i + 1 < n && in[i + 1] < 0xA0) {   /* U+0080-U+009F: control characters too (CSI) */
            c = '_';
            i++;
        }
        if (c >= 0x80 && !utf8) {
            if (c < 0xA0) c = '_';                  /* Latin-1's control bytes */
            else {
                if (w + 2 >= cap) { bad = 1; break; }
                out[w++] = (char)(0xC0 | (c >> 6));
                out[w++] = (char)(0x80 | (c & 0x3F));
                continue;
            }
        }
        if (w + 1 >= cap) { bad = 1; break; }
        out[w++] = (char)c;
    }
    if (w > 0 && out[w - 1] == '/') w--;
    out[w] = 0;
    if (w == 0) bad = 1;
    return bad;
}
