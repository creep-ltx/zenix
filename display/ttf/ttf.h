/* SPDX-License-Identifier: GPL-2.0-only */
/* The TrueType reader and rasteriser (DESIGN D51, docs/d51-truetype.md): a .ttf read from memory, a glyph's
 * quadratic outline turned into a 1-bit picture at a pixel size, in integer arithmetic only (no FPU, no soft
 * float). No hinting (the font's instructions are skipped); a pixel is set when its centre is inside the outline
 * (the nonzero rule), and a stroke thinner than a pixel keeps one pixel (dropout control, both ways). No system
 * calls and no libz: ttf2font and the host test (tools/ttf-host-test.c) build the same file. Everything read from
 * the file is checked; a damaged or hostile file gives a reason and nothing else. */
#pragma once

#define TTF_MAXH 64             /* lines of a picture (the Amiga font reader's AF_MAXH) */
#define TTF_MAXW 64             /* columns of a picture (AF_MAXW) */
#define TTF_MAXFILE (4u << 20)  /* a .ttf read whole: 4 MB */
#define TTF_WORK (48u << 10)    /* the work area ttf_glyph needs */

struct ttf {
    const unsigned char *d;
    unsigned n;
    unsigned glyf, glyfn, loca, hmtx, hmtxn, cmap, cmapn;   /* offsets and lengths of the tables used */
    unsigned short upem, nglyphs, nhm, cmapfmt;
    short loc32;                /* head.indexToLocFormat: 1 = 32-bit loca entries */
    short asc, desc, gap;       /* hhea */
    short fixed;                /* post.isFixedPitch */
};

/* the picture of one glyph: h rows of bpr bytes, the leftmost pixel in bit 7; column 0 is x0 pixels right of the
 * pen; the advance in whole pixels */
struct ttf_pic {
    int x0, w, bpr, h, adv;
    unsigned char bits[TTF_MAXH * (TTF_MAXW / 8)];
};

/* d (n bytes) -> 0 and t filled; or the reason */
const char *ttf_open(struct ttf *t, const unsigned char *d, unsigned n);
/* a code point's glyph (0: the font has none) */
unsigned ttf_index(const struct ttf *t, unsigned cp);
/* a glyph's advance and left side bearing in font units (0 and the reason when its entry is not there) */
const char *ttf_hmetrics(const struct ttf *t, unsigned gi, int *adv, int *lsb);
/* a glyph's box in font units, from its header (an empty glyph: all 0) */
const char *ttf_box(const struct ttf *t, unsigned gi, int *x0, int *y0, int *x1, int *y1);
/* glyph gi drawn into p: em64 the em's height and emx64 its width in 1/64 pixels (64-8192, 64-15360: font units * em64 /
 * upem; a hires screen's pixels are taller than wide, so its width is more pixels), h the picture's lines, base the
 * line whose bottom edge is the baseline (0 = the top line); work: TTF_WORK bytes, 2-aligned, -> 0, or the reason
 * (ttf_wide, this very string, for a glyph whose ink is wider than TTF_MAXW or whose advance is past 255) */
extern const char ttf_wide[];
const char *ttf_glyph(const struct ttf *t, unsigned gi, int em64, int emx64, int h, int base, void *work,
                      struct ttf_pic *p);
