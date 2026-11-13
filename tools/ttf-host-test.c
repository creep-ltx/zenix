/* SPDX-License-Identifier: GPL-2.0-only */
/* The TrueType rasteriser's host test (DESIGN D51, docs/d51-truetype.md "Tests"; run by tools/ttf-host-test.sh with
 * AddressSanitizer and UndefinedBehaviorSanitizer): display/ttf/ttf.c and ttfont.c and the Amiga font reader
 * display/server/amifont.c, the same files ttf2font and displayd build.
 *
 *   ttf-host-test FONT.ttf ZCF-OUT     every check below; exit 0 when all pass
 *   ttf-host-test text FONT.ttf SIZE STRING
 *                                      the string's pixels in the font at SIZE as ttf2font makes it, read back by
 *                                      amifont.c and laid out as displayd does (a glyph kern pixels right of the pen,
 *                                      the pen its advance on): "X Y" per set pixel, the pen starting at 0, line 0
 *                                      the font's top (tools/ttf-test.sh checks the desktop's title against it)
 *
 * 1. FreeType (when built with -DHAVE_FREETYPE): every glyph 33-255 drawn by both, unhinted, black and white, at the
 *    desktop's sizes and shape, compared pixel by pixel within the tolerances measured and argued in d51 "Tests".
 * 2. Every size 4-64 made into an Amiga font file and read back by amifont.c: the metrics, every glyph's picture as
 *    ttf_glyph drew it, the stamp; the contents file merged.
 * 3. The console's ZCF1 font written to ZCF-OUT (tools/mkconfont.py check reads it with the kernel's checks).
 * 4. Damaged files: hand-made faults refused with their reasons (one, units per em 16, only read safely); every cut of the file's first 4 KB and 600 cuts
 *    after; 4,000 files with bytes changed at random (a fixed seed): each read or refused, nothing read outside. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ttfont.h"
#include "amifont.h"
#ifdef HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}
static void *al(unsigned n) { return malloc(n); }
static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void p16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void p32(unsigned char *p, unsigned v) { p16(p, v >> 16); p16(p + 2, v); }

/* a table's offset in the file (0: none) */
static unsigned tab(const unsigned char *d, const char *tag, unsigned *len)
{
    unsigned n = (unsigned)d[4] << 8 | d[5];
    for (unsigned i = 0; i < n; i++)
        if (!memcmp(d + 12 + 16 * i, tag, 4)) {
            if (len)
                *len = be32(d + 12 + 16 * i + 12);
            return be32(d + 12 + 16 * i + 8);
        }
    return 0;
}

/* the whole pipeline on d (n bytes): opened, every glyph of one size made into a font file and read back -> the
 * reason it was refused, or 0 */
static const char *through(const unsigned char *d, unsigned n, int size)
{
    struct ttf t;
    const char *why = ttf_open(&t, d, n);
    if (why)
        return why;
    unsigned char *o;
    unsigned on;
    struct ttf_size hw;
    if ((why = ttf_amifont(&t, size, TTF_XR, "z", n, 1, al, free, &o, &on, &hw)))
        return why;
    struct af_font af;
    why = af_load(o, on, &af, al);
    if (!why)
        free(af.bits);
    free(o);
    return why ? "its font file is not read back" : 0;
}

#ifdef HAVE_FREETYPE
/* FreeType's glyph and ours in one grid -> the pixels that differ; *ink FreeType's */
static int ftdiff(FT_Face face, const struct ttf *t, unsigned c, int em64, int emx64, int h, int base, void *work,
                  int *ink)
{
    static unsigned char A[64][256], B[64][256];
    struct ttf_pic p;
    memset(A, 0, sizeof A);
    memset(B, 0, sizeof B);
    if (ttf_glyph(t, ttf_index(t, c), em64, emx64, h, base, work, &p))
        return 9999;
    for (int r = 0; r < h; r++)
        for (int k = 0; k < p.w; k++)
            if (p.bits[r * p.bpr + k / 8] & (0x80 >> (k & 7)) && p.x0 + k + 96 >= 0 && p.x0 + k + 96 < 256)
                A[r][p.x0 + k + 96] = 1;
    FT_Load_Glyph(face, FT_Get_Char_Index(face, c), FT_LOAD_NO_HINTING | FT_LOAD_TARGET_MONO | FT_LOAD_RENDER |
                  FT_LOAD_MONOCHROME);
    FT_Bitmap *b = &face->glyph->bitmap;
    int L = face->glyph->bitmap_left, T = face->glyph->bitmap_top;
    for (int j = 0; j < (int)b->rows; j++)
        for (int i = 0; i < (int)b->width; i++)
            if (b->buffer[j * b->pitch + i / 8] & (0x80 >> (i & 7))) {
                int r = base - T + 1 + j, x = L + i + 96;    /* FreeType's top row's top edge is T above the baseline */
                if (r >= 0 && r < h && x >= 0 && x < 256)
                    B[r][x] = 1;
            }
    int d = 0;
    *ink = 0;
    for (int r = 0; r < h; r++)
        for (int x = 0; x < 256; x++) {
            d += A[r][x] != B[r][x];
            *ink += B[r][x];
        }
    return d;
}
#endif

static int text(const char *font, int size, const char *str)
{
    FILE *f = fopen(font, "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    unsigned n = (unsigned)ftell(f);
    rewind(f);
    unsigned char *d = malloc(n), *o;
    if (fread(d, 1, n, f) != n)
        return 2;
    fclose(f);
    struct ttf t;
    unsigned on;
    struct ttf_size hw;
    struct af_font af;
    if (ttf_open(&t, d, n) || ttf_amifont(&t, size, TTF_XR, "z", n, 1, al, free, &o, &on, &hw) ||
        af_load(o, on, &af, al))
        return 1;
    int pen = 0;
    for (const unsigned char *s = (const unsigned char *)str; *s; s++) {
        const struct af_glyph *g = &af.g[*s];
        if (!g->have)
            return 1;                           /* the 8 x 8 would stand in: not what this checks */
        for (int r = 0; r < af.h; r++)
            for (int k = 0; k < g->w; k++)
                if (af.bits[g->off + r * g->bpr + k / 8] & (0x80 >> (k & 7)))
                    printf("%d %d\n", pen + g->kern + k, r);
        pen += g->adv;
    }
    printf("width %d\n", pen);
    free(af.bits);
    free(o);
    free(d);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "text"))
        return text(argv[2], atoi(argv[3]), argv[4]);
    if (argc != 3) {
        fprintf(stderr, "usage: ttf-host-test FONT.ttf ZCF-OUT\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    unsigned n = (unsigned)ftell(f);
    rewind(f);
    unsigned char *d = malloc(n), *m = malloc(n);
    if (fread(d, 1, n, f) != n)
        return 2;
    fclose(f);
    struct ttf t;
    const char *why = ttf_open(&t, d, n);
    check(!why, "the font opens");
    if (why)
        return 1;
    char msg[400];
    void *work = malloc(TTF_WORK);
    struct ttf_pic pic;

    /* ---- 1. against FreeType ---- */
#ifdef HAVE_FREETYPE
    {
        FT_Library lib;
        FT_Face face;
        FT_Init_FreeType(&lib);
        FT_New_Memory_Face(lib, d, (FT_Long)n, 0, &face);
        /* lines, the share of FreeType's ink allowed to differ in all (percent) */
        static const struct { int h, xr, pct; } cs[] = {
            { 8, TTF_XR, 13 }, { 9, TTF_XR, 13 }, { 10, TTF_XR, 13 }, { 11, TTF_XR, 8 }, { 12, TTF_XR, 8 },
            { 13, TTF_XR, 6 }, { 14, TTF_XR, 6 }, { 16, TTF_XR, 6 }, { 16, 64, 6 }, { 40, 64, 2 } };
        for (unsigned k = 0; k < sizeof cs / sizeof cs[0]; k++) {
            unsigned char *o;
            unsigned on;
            struct ttf_size hw;
            if (ttf_amifont(&t, cs[k].h, cs[k].xr, "z", 1, 1, al, free, &o, &on, &hw)) {
                check(0, "a size for FreeType's comparison");
                continue;
            }
            free(o);
            int emx = hw.em64 * cs[k].xr / 64;
            FT_Set_Char_Size(face, emx, hw.em64, 72, 72);
            long diff = 0, ink = 0;
            int worst = 0, wc = 0, ng = 0;
            for (unsigned c = 33; c < 256; c++) {
                if ((c >= 0x7F && c < 0xA0) || !ttf_index(&t, c))
                    continue;
                int i, dd = ftdiff(face, &t, c, hw.em64, emx, cs[k].h, hw.base, work, &i);
                diff += dd;
                ink += i;
                ng++;
                if (dd > worst)
                    worst = dd, wc = (int)c;
            }
            snprintf(msg, sizeof msg, "%d lines, width %d/64: %d glyphs differ from FreeType's in %ld of %ld pixels "
                     "(%ld%%, at most %d%%), at most %d in one (char %d; at most 24)", cs[k].h, cs[k].xr, ng, diff,
                     ink, ink ? 100 * diff / ink : 100, cs[k].pct, worst, wc);
            check(ng > 150 && ink && 100 * diff <= cs[k].pct * ink && worst <= 24, msg);
        }
        FT_Done_Face(face);
        FT_Done_FreeType(lib);
    }
#else
    printf("SKIP the comparison with FreeType (built without it)\n");
#endif

    /* ---- 2. every size as an Amiga font file, read back ---- */
    {
        int ok = 1, sizes = 0;
        char bad[160] = "";
        for (int size = 4; size <= 64; size++) {
            unsigned char *o;
            unsigned on;
            struct ttf_size hw;
            const char *w = ttf_amifont(&t, size, TTF_XR, "go", n, 1234567, al, free, &o, &on, &hw);
            struct af_font af;
            const char *r = w ? w : af_load(o, on, &af, al);
            if (r) {
                if (!*bad)
                    snprintf(bad, sizeof bad, "%d: %s", size, r);
                ok = 0;
                if (!w)
                    free(o);
                continue;
            }
            int good = af.h == size && af.base == hw.base && af.prop == hw.prop && af.nglyphs == hw.nglyphs &&
                       ttf_stamp_ok(o, on, n, 1234567, size) && !ttf_stamp_ok(o, on, n + 1, 1234567, size) &&
                       !ttf_stamp_ok(o, on, n, 1234568, size) && !ttf_stamp_ok(o, on, n, 1234567, size + 1);
            for (unsigned c = 32; c < 256 && good; c++) {        /* each glyph as ttf_glyph drew it */
                unsigned g = ttf_index(&t, c);
                if ((c >= 0x7F && c < 0xA0) || (!g && c != 32)) {
                    good = !af.g[c].have;
                    continue;
                }
                int em64 = hw.em64;
                if (ttf_glyph(&t, g, em64, em64 * TTF_XR / 64, size, hw.base, work, &pic) == ttf_wide) {
                    good = !af.g[c].have;
                    continue;
                }
                good = af.g[c].have && af.g[c].w == pic.w && af.g[c].kern == pic.x0 && af.g[c].adv == pic.adv &&
                       !memcmp(af.bits + af.g[c].off, pic.bits, (size_t)pic.bpr * (size_t)size);
            }
            if (!good && !*bad)
                snprintf(bad, sizeof bad, "%d: read back differently", size);
            ok &= good;
            sizes++;
            free(af.bits);
            free(o);
        }
        snprintf(msg, sizeof msg, "sizes 4-64 written and read back by amifont.c: metrics, every glyph, the stamp "
                 "(%d sizes%s%s)", sizes, *bad ? "; " : "", bad);
        check(ok && sizes == 61, msg);
        unsigned char *o;
        unsigned on;
        struct ttf_size hw;
        ttf_amifont(&t, 11, TTF_XR, "go", n, 5, al, free, &o, &on, &hw);
        snprintf(msg, sizeof msg, "go 11: 11 lines, baseline %d, em %d/64 pixels, %d glyphs, proportional %d, %u "
                 "bytes", hw.base, hw.em64, hw.nglyphs, hw.prop, on);
        check(hw.base == 8 && hw.nglyphs == 191 && hw.prop == 1 && !hw.wide, msg);
        free(o);
        /* the contents file: sizes merged and sorted, an entry per size, each read by af_contents */
        static unsigned char c1[4 + 260 * 64], c2[4 + 260 * 64];
        unsigned l1 = ttf_contents(0, 0, "go", 11, 1, c1);
        unsigned l2 = ttf_contents(c1, l1, "go", 8, 1, c2);
        unsigned l3 = ttf_contents(c2, l2, "go", 11, 1, c1);
        char file[64], sizes_s[64];
        const char *r1 = af_contents(c1, l3, 11, file, sizeof file, sizes_s, sizeof sizes_s);
        snprintf(msg, sizeof msg, "the contents file: 11, then 8, then 11 again -> \"%s\", 11 is %s", sizes_s,
                 r1 ? r1 : file);
        check(l1 == 264 && l2 == 524 && l3 == 524 && !r1 && !strcmp(sizes_s, "8 11") && !strcmp(file, "go/11"), msg);
        for (int s = 1; s <= 64; s++)
            l3 = ttf_contents(c1, l3, "go", s, 1, c2), memcpy(c1, c2, l3);
        check(l3 == 4 + 260 * 64 && !ttf_contents(c1, l3, "go", 65, 1, c2),
              "the contents file: 64 sizes at most (a 65th is refused)");
    }

    /* ---- 3. the console's font ---- */
    {
        unsigned char *z;
        unsigned zn;
        int ng;
        const char *w = ttf_zcf(&t, al, free, &z, &zn, &ng);
        snprintf(msg, sizeof msg, "the console's ZCF1 font: %d glyphs past U+00FF, %u bytes (%s)", ng, zn,
                 w ? w : "written");
        check(!w && ng > 100 && zn == be32(z + 16), msg);
        if (!w) {
            FILE *o = fopen(argv[2], "wb");
            fwrite(z, 1, zn, o);
            fclose(o);
            free(z);
        }
    }

    /* ---- 4. damaged files ---- */
    {
        int nc = 0, ok = 1;
        char bad[200] = "";
        unsigned gl, glyf = tab(d, "glyf", &gl), ll, loca = tab(d, "loca", &ll), head = tab(d, "head", 0);
        unsigned maxp = tab(d, "maxp", 0), hhea = tab(d, "hhea", 0);
        unsigned gA = ttf_index(&t, 'A'), gAr = ttf_index(&t, 'B');    /* A, and B made a composite of A below */
        unsigned locA = t.loc32 ? be32(d + loca + 4 * gA) : 2u * ((unsigned)d[loca + 2 * gA] << 8 | d[loca + 2 * gA + 1]);
        unsigned locAr = t.loc32 ? be32(d + loca + 4 * gAr) : 2u * ((unsigned)d[loca + 2 * gAr] << 8 | d[loca + 2 * gAr + 1]);
        const unsigned char *A = d + glyf + locA;
        int simpleA = (short)(A[0] << 8 | A[1]) > 0;
        check(simpleA, "the font's A is a simple glyph (for the cases below)");
        /* B as a composite of A: the header (-1 contours, A's box), flags ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES
         * | WE_HAVE_A_SCALE, A, offset 0,0, the scale (2.14) */
#define COMP(buf, scale) do { memcpy((buf) + glyf + locAr, A, 10); p16((buf) + glyf + locAr, 0xFFFF); \
        p16((buf) + glyf + locAr + 10, 0x000B); p16((buf) + glyf + locAr + 12, gA); p32((buf) + glyf + locAr + 14, 0); \
        p16((buf) + glyf + locAr + 18, (scale)); } while (0)
        {
            struct ttf t2;
            struct ttf_pic pa, pb;
            int ok = 1, wa = 0, wb = 0;
            for (int sc = 0; sc < 2; sc++) {
                memcpy(m, d, n);
                COMP(m, sc ? 0x2000 : 0x4000);
                ttf_open(&t2, m, n);
                for (int size = 8; size <= 40; size += 8) {
                    int em64 = size * 64 * 3 / 4, base = size * 3 / 4;
                    const char *e1 = ttf_glyph(&t2, gA, em64, em64, size, base, work, &pa);
                    const char *e2 = ttf_glyph(&t2, gAr, em64, em64, size, base, work, &pb);
                    if (e1 || e2)
                        ok = 0;
                    else if (!sc)
                        ok &= pa.w == pb.w && pa.x0 == pb.x0 && !memcmp(pa.bits, pb.bits, (size_t)pa.bpr * (size_t)size);
                    else
                        wa += pa.w, wb += pb.w;
                }
            }
            snprintf(msg, sizeof msg, "a composite glyph: B made of A unscaled is A pixel for pixel at 8-40 lines; "
                     "at half scale half as wide (%d of %d columns)", wb, wa);
            check(ok && 2 * wb >= wa - 6 && 2 * wb <= wa + 6, msg);
        }
        for (int k = 0; k < 18; k++) {
            memcpy(m, d, n);
            unsigned mn = n;
            const char *what = 0, *want = 0;
            switch (k) {
            case 0: what = "not a font"; memcpy(m, "\x7f" "ELF", 4); want = "not a TrueType font"; break;
            case 1: what = "CFF outlines"; memcpy(m, "OTTO", 4); want = "an OpenType font with PostScript (CFF) outlines: only TrueType outlines are read"; break;
            case 2: what = "a collection"; memcpy(m, "ttcf", 4); want = "a font collection (.ttc): not read"; break;
            case 3: what = "65535 tables"; p16(m + 4, 65535); want = "cut short in its table directory"; break;
            case 4:
                what = "glyf past the end";
                for (unsigned i = 0; i < (unsigned)(m[4] << 8 | m[5]); i++)
                    if (!memcmp(m + 12 + 16 * i, "glyf", 4))
                        p32(m + 12 + 16 * i + 12, n);
                want = "a table (glyf) outside the file";
                break;
            case 5:
                what = "no glyf";
                for (unsigned i = 0; i < (unsigned)(m[4] << 8 | m[5]); i++)
                    if (!memcmp(m + 12 + 16 * i, "glyf", 4))
                        memcpy(m + 12 + 16 * i, "glyX", 4);
                want = "no glyf table (not TrueType outlines)";
                break;
            case 6: what = "units per em 0"; p16(m + head + 18, 0); want = "units per em outside 16-16384"; break;
            case 7: what = "a loca entry past glyf"; if (t.loc32) p32(m + loca + 4 * gA + 4, gl + 4); else p16(m + loca + 2 * gA + 2, gl / 2 + 2);
                    want = "a glyph outside its glyf table"; break;
            case 8: what = "contour ends out of order"; if ((short)(A[0] << 8 | A[1]) >= 2) { p16(m + glyf + locA + 10, 60000); }
                    else p16(m + glyf + locA + 10, 0), p16(m + glyf + locA, 2), p16(m + glyf + locA + 12, 0);
                    want = (short)(A[0] << 8 | A[1]) >= 2 ? "its contours' ends are not in order" : 0; break;
            case 9: what = "2000 points"; p16(m + glyf + locA + 10 + 2 * ((short)(A[0] << 8 | A[1]) - 1), 1999);
                    want = "a glyph with more than 1024 points"; break;
            case 10: what = "a composite naming itself"; COMP(m, 0x4000); p16(m + glyf + locAr + 12, gAr); want = "composite glyphs nested too deep"; break;
            case 11: what = "a composite naming glyph 65000"; COMP(m, 0x4000); p16(m + glyf + locAr + 12, 65000); want = "a glyph index past the glyphs"; break;
            case 12: what = "cut inside A's points"; if (t.loc32) p32(m + loca + 4 * gA + 4, locA + 14); else p16(m + loca + 2 * gA + 2, (locA + 14) / 2);
                    want = "a glyph cut short"; break;
            case 13: what = "no glyphs"; p16(m + maxp + 4, 0); want = "no glyphs"; break;
            case 14: what = "no horizontal metrics"; p16(m + hhea + 34, 0); want = "its horizontal metrics count is 0 or past the glyphs"; break;
            case 15: what = "loca format 7"; p16(m + head + 50, 7); want = "an unknown loca format"; break;
            case 16:                            /* B, C, D, E: four parts each of the next, E of the space: 4^4 */
                what = "a composite of 4 x 4 x 4 x 4 parts";
                for (int q = 0; q < 4; q++) {
                    unsigned gq = ttf_index(&t, 'B' + q), nx = q < 3 ? ttf_index(&t, 'C' + q) : ttf_index(&t, ' ');
                    unsigned lq = t.loc32 ? be32(d + loca + 4 * gq) : 2u * ((unsigned)d[loca + 2 * gq] << 8 | d[loca + 2 * gq + 1]);
                    unsigned char *e = m + glyf + lq;
                    p16(e, 0xFFFF);
                    for (int r = 0; r < 4; r++, e += 8) {
                        p16(e + 10, r < 3 ? 0x0023 : 0x0003);   /* words, x/y values, more parts (not the last) */
                        p16(e + 12, nx);
                        p32(e + 14, 0);
                    }
                }
                want = "a composite glyph with too many parts (past 256)";
                break;
            case 17: what = "units per em 16 (every point far outside)"; p16(m + head + 18, 16); want = 0; break;
            }
            const char *got = through(m, mn, 11);
            nc++;
            int good = want ? got && !strcmp(got, want) : 1;
            if (!good && !*bad)
                snprintf(bad, sizeof bad, "%s: wanted \"%s\", got \"%s\"", what, want ? want : "-", got ? got : "read");
            ok &= good;
        }
        snprintf(msg, sizeof msg, "%d damaged fonts refused with their reasons (one only read, nothing outside)%s%s",
                 nc, *bad ? ": " : "", bad);
        check(ok, msg);
        (void)ll;
        /* cuts: every length up to 4 KB (the directory, head, cmap), then 600 more spread over the file; a cut is
         * refused unless it is past the end of every table read (head maxp hhea hmtx loca glyf cmap; post, name and
         * the others are not needed) */
        unsigned need = 0;
        static const char *const used[] = { "head", "maxp", "hhea", "hmtx", "loca", "glyf", "cmap" };
        for (int k = 0; k < 7; k++) {
            unsigned l, o = tab(d, used[k], &l);
            if (o + l > need)
                need = o + l;
        }
        int cuts = 0, wrong = 0, read = 0;
        for (unsigned l = 0; l < n; l += l < 4096 ? 1 : n / 600 + 1, cuts++) {
            unsigned char *c = malloc(l ? l : 1);
            memcpy(c, d, l);
            int r = !through(c, l, 8);
            read += r;
            wrong += r != (l >= need);
            free(c);
        }
        snprintf(msg, sizeof msg, "%d cuts of the file: each refused before the end of the tables read (%u bytes), "
                 "read after it (%d read), none read outside", cuts, need, read);
        check(!wrong, msg);
        /* changed bytes: 1-8 bytes set at random, 4,000 files */
        unsigned seed = 12345;
        int refused = 0;
        for (int i = 0; i < 4000; i++) {
            memcpy(m, d, n);
            if (i & 2)
                COMP(m, 0x4000);                /* half of them with the composite glyph in */
            int nb = 1 + (int)((seed >> 16) & 7);
            for (int k = 0; k < nb; k++) {
                seed = seed * 1103515245u + 12345u;
                unsigned at = (seed >> 8) % n;
                seed = seed * 1103515245u + 12345u;
                /* half the changes in the first 2 KB (the directory and the tables' headers), half anywhere */
                if (k & 1)
                    at %= 2048 < n ? 2048 : n;
                m[at] = (unsigned char)(seed >> 16);
            }
            refused += through(m, n, i & 1 ? 8 : 13) != 0;
        }
        snprintf(msg, sizeof msg, "4000 fonts with bytes changed at random: each read or refused, none read outside "
                 "(%d refused)", refused);
        check(refused > 0, msg);
    }
    free(work);
    free(m);
    free(d);
    printf("%s\n", fails ? "ttf-host-test: FAILED" : "ttf-host-test: all passed");
    return fails != 0;
}
