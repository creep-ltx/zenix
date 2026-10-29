/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* The console's widths and its Unicode font (DESIGN D28 sections 6, 7 and 9), a file of its own so the host tests
 * (tools/term-test.c, tools/confont-test.c) run the kernel's own code.
 *
 * Widths are the system's, not a font's: con_width is picolibc's wcwidth in its non-CJK locale (the C library is
 * compiled from the same two tables, so a program and the console agree on every column): 0 for NUL and the
 * zero-width ranges (combining marks), 2 for the wide ranges, -1 for the other controls, U+007F-009F and the
 * surrogates, 1 for everything else.
 *
 * The font (ZCF1, tools/mkconfont.py): checked here in one pass on the kernel's own copy (CON_UFONT), then each entry
 * of its half-pages gets its code point's width class in its top three bits, so a character the font maps has its
 * width with its glyph, for no extra search (section 9: the blocks of Claude Code's symbols also hold wide emoji). */

#include "cwidth.h"                     /* the width function, shared with libz (user/libz.c) */

int con_width(u32 c) { return cw_width(c); }

/* the loaded font (0: none): its index, half-pages (entries with their width class) and glyphs */
const u8 *cf_index;
const u16 *cf_pages;
const u8 *cf_glyphs;
u32 cf_nglyphs;
static u8 *cf_data;

/* a width class in an entry's top three bits, and back */
#define CF_CLASS(w) ((u16)((w) == 1 ? 0 : (w) == 0 ? 1 : (w) == 2 ? 2 : 3) << 13)
static const signed char cf_wclass[8] = { 1, 0, 2, -1, -1, -1, -1, -1 };

/* the width of a decoded character: from the font's entry when its half-page is there, else the search */
__attribute__((optimize("O2"))) int con_cwidth(u32 c)
{
    if (c < 0x300)
        return c < 0xA0 ? -1 : 1;
    if (c <= 0xFFFF && cf_index) {
        u32 p = cf_index[c >> 7];
        if (p)
            return cf_wclass[cf_pages[(p - 1) * 128 + (c & 127)] >> 13];
    }
    return con_width(c);
}

static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }

/* the checks of CON_UFONT (section 7), on the kernel's copy: 0, or -EINVAL. n was bounded by CONF_MAXSIZE before
 * the copy was made. */
long confont_check(const u8 *d, u32 n)
{
    if (n < CONF_HDR + CONF_INDEX || n > CONF_MAXSIZE)
        return -EINVAL;
    if (d[0] != 'Z' || d[1] != 'C' || d[2] != 'F' || d[3] != '1')
        return -EINVAL;
    u32 g = be16(d + 10), p = be16(d + 12);
    u32 total = (u32)d[16] << 24 | (u32)d[17] << 16 | (u32)d[18] << 8 | d[19];
    if (be16(d + 4) != CONF_HDR || be16(d + 6) != 0 || d[8] != 8 || d[9] != 8 || be16(d + 14) != 0)
        return -EINVAL;
    for (int i = 20; i < CONF_HDR; i++)
        if (d[i])
            return -EINVAL;
    if (g < 3 || g > CONF_MAXG || p > CONF_MAXP)
        return -EINVAL;
    if (total != n || total != CONF_HDR + CONF_INDEX + 256 * p + 8 * g)
        return -EINVAL;
    const u8 *ix = d + CONF_HDR;
    u8 seen[32];                        /* each half-page named by at most one index byte (its entries get one
                                         * block's widths) */
    memset(seen, 0, sizeof seen);
    if (ix[0] || ix[1])
        return -EINVAL;                 /* U+0000-00FF are Topaz's */
    for (u32 i = 0; i < CONF_INDEX; i++) {
        u32 b = ix[i];
        if (b > p || (b && (seen[b >> 3] >> (b & 7) & 1)))
            return -EINVAL;
        seen[b >> 3] |= (u8)(1u << (b & 7));
    }
    const u8 *e = d + CONF_HDR + CONF_INDEX;
    for (u32 i = 0; i < 128 * p; i++) {
        u32 v = be16(e + 2 * i);        /* 0, a printable Latin-1 byte (not 0x7F-0x9F: those are the box), a glyph */
        if (v && (v < 32 || (v >= 0x7F && v < 0xA0) || v >= 256 + g))
            return -EINVAL;
    }
    return 0;
}

/* a checked copy becomes the font: the width classes written into its entries (which are then in the CPU's own
 * order: the same on a 68k, swapped on the PC's host tests); -> the old copy (to be freed) */
u8 *confont_set(u8 *d)
{
    u8 *old = cf_data;
    if (d) {
        const u8 *ix = d + CONF_HDR;
        u16 *pg = (u16 *)(d + CONF_HDR + CONF_INDEX);
        for (u32 h = 0; h < CONF_INDEX; h++)
            if (ix[h]) {
                u16 *e = pg + (ix[h] - 1) * 128;
                for (u32 k = 0; k < 128; k++) {
                    u32 c = h << 7 | k;
                    e[k] = (u16)((be16((const u8 *)&e[k]) & 0x1FFF) | CF_CLASS(con_width(c)));   /* native order */
                }
            }
    }
    cf_data = d;
    if (d) {
        cf_pages = (const u16 *)(d + CONF_HDR + CONF_INDEX);
        cf_nglyphs = be16(d + 10);
        cf_glyphs = d + CONF_HDR + CONF_INDEX + 256 * be16(d + 12);
        cf_index = d + CONF_HDR;
    } else {
        cf_index = 0;
        cf_pages = 0;
        cf_glyphs = 0;
        cf_nglyphs = 0;
    }
    return old;
}
