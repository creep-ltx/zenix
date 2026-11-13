/* SPDX-License-Identifier: GPL-2.0-only */
/* A TrueType font as an AmigaOS bitmap font, its contents file, and the console's ZCF1 font (ttfont.h; DESIGN D51,
 * docs/d51-truetype.md). The font file's layout is the one docs/d38-desktop.md "Fonts" reads: HUNK_HEADER, one
 * HUNK_CODE ("moveq #-1,d0; rts", the DiskFontHeader, the TextFont at 58, then the strike and its tables),
 * HUNK_RELOC32 for the six pointers, the stamp (a HUNK_DEBUG block, which AmigaOS's loader skips) and HUNK_END. */
#include "ttfont.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define TF 58
#define NCH 224                 /* 32-255 */

static void p16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void p32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void zero(void *p, u32 n) { u8 *b = p; while (n--) *b++ = 0; }
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

/* the Latin-1 letters' extent and the em that fits them into size lines: the ascent above the baseline and the
 * descent below it of the font's own hhea, widened to the highest and lowest of the characters 32-255 (so an
 * accent on a capital is not cut), in font units -> the em in 1/64 pixels and the baseline's line */
static void layout(const struct ttf *t, int size, int *em64, int *base)
{
    int hi = t->asc, lo = t->desc;
    for (u32 c = 33; c < 256; c++) {
        if (c >= 0x7F && c < 0xA0)
            continue;
        u32 g = ttf_index(t, c);
        int x0, y0, x1, y1;
        if (!g || ttf_box(t, g, &x0, &y0, &x1, &y1))
            continue;
        if (y1 > hi)
            hi = y1;
        if (y0 < lo)
            lo = y0;
    }
    if (hi <= lo)
        hi = lo + t->upem;
    int span = hi - lo, e = size * 64 * (int)t->upem / span;
    if (e > 64 * 128)
        e = 64 * 128;
    if (e < 64)
        e = 64;
    /* the baseline: the line whose bottom edge is at the ascent, rounded */
    int a = (hi * e / (int)t->upem + 32) >> 6;
    if (a < 1)
        a = 1;
    if (a > size)
        a = size;
    *em64 = e;
    *base = a - 1;
}

const char *ttf_amifont(const struct ttf *t, int size, int xr, const char *name, u32 tsize, u32 tmtime,
                        void *(*alloc)(unsigned), void (*release)(void *), u8 **out, u32 *outn, struct ttf_size *how)
{
    *out = 0;
    *outn = 0;
    u32 nl = slen(name);
    if (size < 4 || size > TTF_MAXH)
        return "a size outside 4-64 lines";
    if (!nl || nl > 25)
        return "a font name of 1-25 bytes is needed";
    int em64, base;
    layout(t, size, &em64, &base);
    struct ttf_pic *pic = alloc(sizeof *pic);
    void *work = alloc(TTF_WORK);
    struct gl { short x0, w, adv; u32 at; } *gl = alloc(sizeof(struct gl) * (NCH + 1));
    u8 *bits = alloc((u32)size * 8 * (NCH + 1));                /* every glyph's rows, packed, at most 64 wide */
    const char *why = 0;
    u32 used = 0, total = 0, nglyphs = 0, bytes = 0;
    int adv0 = -1, prop = 0, wide = 0;
    if (!pic || !work || !gl || !bits) {
        why = "no memory";
        goto done;
    }
    for (u32 i = 0; i <= NCH; i++) {            /* 32-255, then the default glyph (the font's glyph 0) */
        u32 c = 32 + i, g = i == NCH ? 0 : ttf_index(t, c);
        gl[i].x0 = gl[i].w = gl[i].adv = 0;
        gl[i].at = used;
        if (i < NCH && ((c >= 0x7F && c < 0xA0) || (!g && c != 32)))
            continue;                           /* control codes, and what the font lacks: the 8 x 8's */
        if (i < NCH && !g) {                    /* no space in the font: an empty one, a quarter em wide */
            gl[i].adv = (short)((em64 * xr / 64 / 4 + 32) >> 6);
            nglyphs++;
            adv0 = gl[i].adv;                   /* (the first character: no other advance seen yet) */
            continue;
        }
        if ((why = ttf_glyph(t, g, em64, em64 * xr / 64, size, base, work, pic)) == ttf_wide) {
            why = 0;                            /* left out, and counted: the 8 x 8 stands in */
            if (i < NCH)                        /* (the default glyph too wide: an empty one) */
                wide++;
            continue;
        }
        if (why)
            goto done;
        gl[i].x0 = (short)pic->x0;
        gl[i].w = (short)pic->w;
        gl[i].adv = (short)pic->adv;
        if (pic->x0 < -64 || pic->x0 > 64) {
            why = "a glyph drawn more than 64 pixels from its place";
            goto done;
        }
        for (int r = 0; r < size; r++)
            for (int b = 0; b < pic->bpr; b++)
                bits[used + (u32)r * (u32)pic->bpr + (u32)b] = pic->bits[r * pic->bpr + b];
        used += (u32)size * (u32)pic->bpr;
        if (i < NCH)
            bytes += (u32)size * (u32)pic->bpr;
        total += (u32)pic->w;
        if (i < NCH) {
            nglyphs++;
            if (adv0 < 0)
                adv0 = pic->adv;
            else if (pic->adv != adv0)
                prop = 1;
        }
    }
    if (!nglyphs) {
        why = "it has none of the characters 32-255";
        goto done;
    }
    if (bytes > 65535) {                        /* amifont.c's AF_MAXBYTES: the desktop would refuse the file */
        why = "its glyphs take more than 64 KB at this size (the desktop's limit)";
        goto done;
    }
    {
        u32 mod = (total + 15) / 16 * 2, nent = NCH + 1;
        if (!mod)
            mod = 2;
        if (total > 65535) {
            why = "its glyphs side by side are wider than 65535 pixels";
            goto done;
        }
        u32 o_data = (TF + 52 + 3) & ~3u, o_loc = o_data + mod * (u32)size, o_space = o_loc + 4 * nent,
            o_kern = o_space + 2 * nent, hl = (o_kern + 2 * nent + 3) & ~3u;
        u32 nrel = 6, flen = 4 * 6 + 8 + hl + 4 * (4 + nrel) + 28;
        u8 *f = alloc(flen);
        if (!f) {
            why = "no memory";
            goto done;
        }
        zero(f, flen);
        u8 *p = f;
        p32(p, 0x3F3); p32(p + 4, 0); p32(p + 8, 1); p32(p + 12, 0); p32(p + 16, 0); p32(p + 20, hl / 4);
        p += 24;
        p32(p, 0x3E9); p32(p + 4, hl / 4);
        u8 *h = p + 8;
        p16(h, 0x70FF);                         /* moveq #-1,d0; rts: a font file run by mistake does nothing */
        p16(h + 2, 0x4E75);
        h[4 + 8] = 12;                          /* the DiskFontHeader's node: NT_FONT, its name dfh_Name */
        p32(h + 4 + 10, 26);
        p16(h + 18, 0x0F80);                    /* DFH_ID */
        p16(h + 20, 1);                         /* dfh_Revision */
        for (u32 k = 0; k < nl && k < 25; k++)
            h[26 + k] = (u8)name[k];
        u8 *dn = h + 26 + (nl < 25 ? nl : 25);
        dn[0] = '.'; dn[1] = 'f'; dn[2] = 'o'; dn[3] = 'n'; dn[4] = 't';
        u8 *tf = h + TF;
        tf[8] = 12;                             /* tf_Message's node: NT_FONT, the same name */
        p32(tf + 10, 26);
        u32 ml = o_kern + 2 * nent - TF;
        p16(tf + 18, ml > 65535 ? 65535 : ml);  /* mn_Length: the TextFont and its tables (a UWORD) */
        p16(tf + 20, (u32)size);                /* tf_YSize */
        tf[22] = 0;                             /* tf_Style */
        tf[23] = 0x02 | 0x40 | (prop ? 0x20 : 0);   /* FPF_DISKFONT | FPF_DESIGNED (| FPF_PROPORTIONAL) */
        int xs = prop && gl['n' - 32].adv ? gl['n' - 32].adv : adv0;     /* tf_XSize: an n's width, or the one */
        p16(tf + 24, (u32)(xs < 0 ? 0 : xs > 255 ? 255 : xs));
        p16(tf + 26, (u32)base);                /* tf_Baseline */
        p16(tf + 28, 1);                        /* tf_BoldSmear */
        tf[32] = 32;                            /* tf_LoChar, tf_HiChar */
        tf[33] = 255;
        p32(tf + 34, o_data);
        p16(tf + 38, mod);
        p32(tf + 40, o_loc);
        p32(tf + 44, o_space);
        p32(tf + 48, o_kern);
        u32 x = 0;
        for (u32 i = 0; i < nent; i++) {
            const struct gl *g = &gl[i];
            u32 bpr = ((u32)g->w + 7) / 8;
            for (int r = 0; r < size; r++)
                for (u32 k = 0; k < (u32)g->w; k++)
                    if (bits[g->at + (u32)r * bpr + k / 8] & (0x80 >> (k & 7))) {
                        u32 b = x + k;
                        h[o_data + (u32)r * mod + b / 8] |= (u8)(0x80 >> (b & 7));
                    }
            p16(h + o_loc + 4 * i, x);
            p16(h + o_loc + 4 * i + 2, (u32)g->w);
            int empty = !g->w && !g->adv;
            p16(h + o_space + 2 * i, (u32)(u16)(empty ? 0 : g->adv - g->x0));     /* the pen: space + kern */
            p16(h + o_kern + 2 * i, (u32)(u16)(empty ? 0 : g->x0));
            x += (u32)g->w;
        }
        p = h + hl;
        p32(p, 0x3EC); p32(p + 4, nrel); p32(p + 8, 0);
        static const u16 rel[6] = { 14, TF + 10, TF + 34, TF + 40, TF + 44, TF + 48 };
        for (u32 k = 0; k < nrel; k++)
            p32(p + 12 + 4 * k, rel[k]);
        p32(p + 12 + 4 * nrel, 0);
        p += 16 + 4 * nrel;
        p32(p, 0x3F1); p32(p + 4, 4); p32(p + 8, 0x5A545446u); p32(p + 12, tsize); p32(p + 16, tmtime);
        p32(p + 20, (u32)TTF_STAMP_VER << 16 | (u32)size); p32(p + 24, 0x3F2);
        *out = f;
        *outn = flen;
        if (how) {
            how->h = size;
            how->base = base;
            how->em64 = em64;
            how->nglyphs = (int)nglyphs;
            how->prop = prop;
            how->wide = wide;
        }
    }
done:
    if (pic) release(pic);
    if (work) release(work);
    if (gl) release(gl);
    if (bits) release(bits);
    return why;
}

u32 ttf_contents(const u8 *old, u32 oldn, const char *name, int size, int prop, u8 *out)
{
    u32 n = 0, nl = slen(name);
    u16 sizes[65];
    u8 sf[65][2];                               /* each size's style and flags, kept as they were */
    if (oldn >= 4 && old[0] == 0x0f && old[1] == 0x00) {        /* a plain FontContentsHeader: its sizes kept */
        u32 num = (u32)old[2] << 8 | old[3];
        for (u32 i = 0; i < num && i < 64 && 4 + 260 * (i + 1) <= oldn; i++) {
            u32 s = (u32)old[4 + 260 * i + 256] << 8 | old[4 + 260 * i + 257];
            int dup = s == (u32)size || s < 1 || s > 64;
            for (u32 k = 0; k < n; k++)
                dup |= sizes[k] == s;
            if (!dup) {
                sf[n][0] = old[4 + 260 * i + 258];
                sf[n][1] = old[4 + 260 * i + 259];
                sizes[n++] = (u16)s;
            }
        }
    }
    if (n >= 64)
        return 0;
    sf[n][0] = 0;
    sf[n][1] = (u8)(0x02 | 0x40 | (prop ? 0x20 : 0));
    sizes[n++] = (u16)size;
    for (u32 i = 1; i < n; i++)
        for (u32 j = i; j > 0 && sizes[j - 1] > sizes[j]; j--) {
            u16 v = sizes[j];
            sizes[j] = sizes[j - 1];
            sizes[j - 1] = v;
            u8 a = sf[j][0], b = sf[j][1];
            sf[j][0] = sf[j - 1][0];
            sf[j][1] = sf[j - 1][1];
            sf[j - 1][0] = a;
            sf[j - 1][1] = b;
        }
    zero(out, 4 + 260 * n);
    p16(out, 0x0f00);
    p16(out + 2, n);
    for (u32 i = 0; i < n; i++) {
        u8 *e = out + 4 + 260 * i;
        u32 k = 0;
        for (; k < nl && k < 240; k++)
            e[k] = (u8)name[k];
        e[k++] = '/';
        if (sizes[i] >= 10)
            e[k++] = (u8)('0' + sizes[i] / 10);
        e[k] = (u8)('0' + sizes[i] % 10);
        p16(e + 256, sizes[i]);
        e[258] = sf[i][0];                      /* style */
        e[259] = sf[i][1];
    }
    return 4 + 260 * n;
}

/* ---- the console's font (ZCF1, docs/frozen-interfaces.md; DESIGN D28 7) ---- */
/* single-width code points past U+00FF a console font is for: the alphabets up to U+10FF, then U+1E00-U+2E7F
 * (Latin additions, Greek extended, punctuation, symbols, arrows, box lines, blocks); the combining marks
 * U+0300-U+036F (drawn over a cell, not in one) and the wide scripts are left out */
static int zcf_wanted(u32 cp)
{
    return (cp >= 0x100 && cp < 0x1100 && !(cp >= 0x300 && cp < 0x370)) || (cp >= 0x1E00 && cp < 0x2E80);
}

const char *ttf_zcf(const struct ttf *t, void *(*alloc)(unsigned), void (*release)(void *), u8 **out, u32 *outn,
                    int *nglyphs)
{
    *out = 0;
    *outn = 0;
    *nglyphs = 0;
    u32 g = 3, hp = 0;
    u8 index[512];
    zero(index, sizeof index);
    for (u32 cp = 0x100; cp < 0x2E80; cp++)     /* count first: the glyphs and the half-pages they need */
        if (zcf_wanted(cp) && ttf_index(t, cp)) {
            if (g >= 4096)
                break;
            g++;
            if (!index[cp >> 7])
                index[cp >> 7] = (u8)++hp;
        }
    if (g == 3)
        return "it has no characters past U+00FF for the console";
    u32 total = 32 + 512 + 256 * hp + 8 * g;
    u8 *f = alloc(total);
    struct ttf_pic *pic = alloc(sizeof *pic);
    void *work = alloc(TTF_WORK);
    const char *why = 0;
    if (!f || !pic || !work) {
        why = "no memory";
        goto done;
    }
    zero(f, total);
    f[0] = 'Z'; f[1] = 'C'; f[2] = 'F'; f[3] = '1';
    p16(f + 4, 32);
    f[8] = f[9] = 8;
    p16(f + 10, g);
    p16(f + 12, hp);
    p32(f + 16, total);
    for (u32 i = 0; i < 512; i++)
        f[32 + i] = index[i];
    u8 *gl = f + 32 + 512 + 256 * hp;
    static const u8 box[3][8] = {
        { 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00 },
        { 0x00, 0x7F, 0x40, 0x40, 0x40, 0x40, 0x7F, 0x00 },   /* the wide box's halves */
        { 0x00, 0xFE, 0x02, 0x02, 0x02, 0x02, 0xFE, 0x00 } };
    for (u32 k = 0; k < 24; k++)
        gl[k] = box[k / 8][k % 8];
    int em64, base;
    layout(t, 8, &em64, &base);
    u32 n = 3;
    for (u32 cp = 0x100; cp < 0x2E80 && n < g; cp++) {
        u32 gi;
        if (!zcf_wanted(cp) || !(gi = ttf_index(t, cp)))
            continue;
        if ((why = ttf_glyph(t, gi, em64, em64, 8, base, work, pic)) == ttf_wide)
            why = 0, pic->w = 0;                /* cut to the cell below anyway */
        if (why)
            goto done;
        int sh = pic->x0 + (8 - (pic->adv < 8 ? pic->adv : 8)) / 2;   /* the advance centred in the cell */
        for (int r = 0; r < 8; r++)
            for (int k = 0; k < pic->w; k++)
                if (pic->bits[r * pic->bpr + k / 8] & (0x80 >> (k & 7)) && sh + k >= 0 && sh + k < 8)
                    gl[8 * n + (u32)r] |= (u8)(0x80 >> (sh + k));
        u8 *e = f + 32 + 512 + 256 * (u32)(index[cp >> 7] - 1) + 2 * (cp & 127);
        p16(e, 256 + n);
        n++;
    }
    *out = f;
    *outn = total;
    *nglyphs = (int)(g - 3);
    f = 0;
done:
    if (f) release(f);
    if (pic) release(pic);
    if (work) release(work);
    return why;
}
