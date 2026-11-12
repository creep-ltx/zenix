/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Long names in UTF-16 on the disk, UTF-8 for programs: the part fatd (DESIGN D30, VFAT's long names) and exfatd (D47,
 * exFAT's File Name entries) share, one copy in the source. Both use u8/u16/u32 and -E* from the including file. */

/* the case a name is compared in: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic folded to their capitals; other
 * scripts compare exactly (no 128 KB up-case table, d30) */
static u32 u16_fold(u32 c)
{
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c < 0x100) return c >= 0xE0 && c != 0xF7 && c != 0xFF ? c - 32 : c == 0xFF ? 0x178 : c;
    if (c < 0x180) {
        if (c <= 0x137 || (c >= 0x14A && c <= 0x177)) return c & 1 && c != 0x131 ? c - 1 : c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c & 1 ? c : c - 1;
        return c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

/* NAME (LEN bytes of UTF-8) as UTF-16 in Q, fewer than CAP units (a character past the BMP takes two). The units, or
 * -EILSEQ (not UTF-8), -ENAMETOOLONG */
static int u16_from_utf8(const char *name, u32 len, u16 *q, u32 cap)
{
    u32 n = 0;
    const u8 *p = (const u8 *)name, *e = p + len;
    while (p < e) {
        u32 c = *p++, more = c < 0x80 ? 0 : c >= 0xC2 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF5 ? 3 : 9;
        if (more == 9 || more > (u32)(e - p)) return -EILSEQ;
        if (more) c &= 0x3Fu >> more;
        for (u32 i = 0; i < more; i++) {
            if ((*p & 0xC0) != 0x80) return -EILSEQ;
            c = c << 6 | (*p++ & 0x3F);
        }
        if ((more == 2 && (c < 0x800 || (c >= 0xD800 && c < 0xE000))) || (more == 3 && (c < 0x10000 || c > 0x10FFFF))) return -EILSEQ;
        if (n + (c >= 0x10000) >= cap) return -ENAMETOOLONG;
        if (c >= 0x10000) {
            c -= 0x10000;
            q[n++] = (u16)(0xD800 + (c >> 10));
            c = 0xDC00 + (c & 0x3FF);
        }
        q[n++] = (u16)c;
    }
    return (int)n;
}

/* is unit I of U (LEN units) a character a Unix name cannot hold: a control character, '/', half a surrogate pair? */
static int u16_bad(const u16 *u, u32 len, u32 i)
{
    u32 c = u[i];
    if (c < 0x20 || c == '/' || (c >= 0xDC00 && c < 0xE000)) return 1;
    return c >= 0xD800 && c < 0xDC00 && (i + 1 >= len || u[i + 1] < 0xDC00 || u[i + 1] >= 0xE000);
}

/* LEN units of UTF-16 as UTF-8 in OUT (CAP bytes with the NUL). BAD 0: a character a Unix name cannot hold (u16_bad)
 * makes the answer -1; else it is written as the byte BAD. The length, or -1 (also: too long for CAP) */
static int u16_to_utf8(const u16 *u, u32 len, char *out, u32 cap, int bad)
{
    u32 k = 0;
    for (u32 i = 0; i < len; i++) {
        u32 c = u[i];
        if (u16_bad(u, len, i)) {
            if (!bad) return -1;
            c = (u32)bad;
        } else if (c >= 0xD800 && c < 0xDC00) c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
        u32 need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (k + need >= cap) return -1;
        if (need == 1) out[k++] = (char)c;
        else {
            out[k++] = (char)(need == 2 ? 0xC0 | c >> 6 : need == 3 ? 0xE0 | c >> 12 : 0xF0 | c >> 18);
            for (u32 s = (need - 1) * 6; s; s -= 6) out[k++] = (char)(0x80 | ((c >> (s - 6)) & 0x3F));
        }
    }
    out[k] = 0;
    return (int)k;
}
