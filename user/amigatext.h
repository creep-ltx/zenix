/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* What the Amiga filesystem servers share (ffsd, pfs3d): names and soft link text are Latin-1 on the disk and UTF-8 on
 * Zenix, dates are days, minutes and ticks since 1978. Header only: each server has its own copy of what it uses. */
#define AM_EPOCH 252460800u            /* 1978-01-01 in Unix time */

static __attribute__((unused)) u32 am_to_unix(u32 days, u32 mins, u32 ticks)
{
    if (days > 46000) return 0xFFFFFFFFu;             /* past the Unix time here; damage */
    if (mins > 1439) mins = 1439;
    if (ticks > 2999) ticks = 2999;
    return AM_EPOCH + days * 86400 + mins * 60 + ticks / 50;
}
/* NAME (LEN bytes of UTF-8) as Latin-1 in Q, at most CAP. 0, -EILSEQ (not Latin-1, or not UTF-8), -ENAMETOOLONG */
static __attribute__((unused)) int am_name_in(const char *name, u32 len, u8 *q, u32 cap, u32 *qlen)
{
    u32 n = 0;
    const u8 *p = (const u8 *)name, *e = p + len;
    while (p < e) {
        u32 c = *p++;
        if (c >= 0x80) {
            if ((c != 0xC2 && c != 0xC3) || p >= e || (*p & 0xC0) != 0x80) return -EILSEQ;
            c = (c & 3) << 6 | (*p++ & 0x3F);
        }
        if (n == cap) return -ENAMETOOLONG;
        q[n++] = (u8)c;
    }
    *qlen = n;
    return 0;
}
/* Latin-1 as UTF-8 in OUT, what can be typed: damage (a control character, a /) shows as _. Length, or -1 past CAP */
static __attribute__((unused)) int am_utf8_of(const u8 *n, u32 len, char *out, u32 cap, int raw)
{
    u32 k = 0;
    for (u32 i = 0; i < len; i++) {
        u32 c = n[i];
        if (!raw && (c < 0x20 || c == '/')) c = '_';
        if (k + 2 >= cap) return -1;
        if (c < 0x80) out[k++] = (char)c;
        else { out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[k] = 0;
    return (int)k;
}
/* a name for a listing: one over the 63 bytes of UTF-8 a udirent holds is its start, ~ and KEY in hex, which opens it */
static __attribute__((unused)) void am_name_list(const u8 *n, u32 len, u32 key, char *out, u32 cap)
{
    if (am_utf8_of(n, len, out, cap, 0) >= 0) return;
    u32 k = len;
    while (k && am_utf8_of(n, k, out, cap - 10, 0) < 0) k--;
    char *p = out + strlen(out);
    *p++ = '~';
    int s = 28;
    while (s > 0 && !(key >> s)) s -= 4;
    for (; s >= 0; s -= 4) *p++ = "0123456789abcdef"[(key >> s) & 15];
    *p = 0;
}
/* The text of a soft link as a Zenix path: a leading / is ../, a//b is a/../b, this volume's name is the way up to its
 * root (the server does not know the mount point), another volume's stays as it is and leads nowhere. Two steps because
 * only the server can count the way up.
 * am_link_root: after which byte of T the path starts from the volume's root (":x" or "ThisVolume:x"), or -1 if it does
 * not. UP folds case as the volume's names do */
static __attribute__((unused)) int am_link_root(const u8 *t, u32 n, const u8 *vol, u32 vlen, u32 (*up)(u32))
{
    for (u32 c = 0; c < n; c++) {
        if (t[c] != ':') continue;
        u32 i = 0;
        if (c == vlen) while (i < c && up(t[i]) == up(vol[i])) i++;
        return !c || i == c ? (int)c + 1 : -1;
    }
    return -1;
}
/* T[START..N) added to OUT at K. The new length, or -ENAMETOOLONG */
static __attribute__((unused)) int am_link_rest(const u8 *t, u32 start, u32 n, char *out, u32 k, u32 cap)
{
    int seg = 1;
    for (u32 i = start; i < n; i++) {
        u32 ch = t[i];
        if (ch == '/' && seg) {
            if (k + 3 >= cap) return -ENAMETOOLONG;
            out[k++] = '.'; out[k++] = '.'; out[k++] = '/';
            continue;
        }
        seg = ch == '/';
        if (k + 2 >= cap) return -ENAMETOOLONG;
        if (ch < 0x80) out[k++] = (char)ch;
        else { out[k++] = (char)(0xC0 | ch >> 6); out[k++] = (char)(0x80 | (ch & 0x3F)); }
    }
    if (!k) out[k++] = '.';                           /* "ThisVolume:" at the root */
    out[k] = 0;
    return (int)k;
}
