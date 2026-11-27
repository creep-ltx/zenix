/* SPDX-License-Identifier: GPL-2.0-only */
/* The display server's drawing on a graphics card's screen (DESIGN D43, docs/d43-rtg.md 4): a chunky bitmap of one
 * byte a pixel (the pen, 0-15) in fast RAM, drawn by the CPU, and its conversion into the card's pixels through a
 * table of the 16 pens. No system calls and no libz: displayd and the PC's test (tools/rtg-host-test.c) build the same
 * file. Every rectangle a function is given is already clipped to its bitmaps by the caller (draw.c). */
#pragma once

struct fbr { short x0, y0, x1, y1; };   /* x1, y1 exclusive: the layout of displayd's struct rect */
struct fbmap { unsigned char *p; unsigned stride; };    /* chunky: pixel (x, y) at p[y * stride + x] */

#define FBA_BOLD  1                     /* the glyph's attributes, as the terminals' (TA_BOLD, TA_UNDER) */
#define FBA_UNDER 2

void fb_fill(const struct fbmap *b, const struct fbr *r, int pen);
void fb_xor(const struct fbmap *b, const struct fbr *r, int pen);      /* the pen's bits inverted */
/* r of d from (sx, sy) of s, also within one bitmap overlapping either way */
void fb_copy(const struct fbmap *s, int sx, int sy, const struct fbmap *d, const struct fbr *r);
/* an 8 x 8 glyph whose cell's top left is (x, y), the part inside c: fg where its bit is set, else bg */
void fb_glyph(const struct fbmap *b, int x, int y, const unsigned char *g, int fg, int bg, int attr, const struct fbr *c);
/* a text run's mask (rows of ms bytes, its byte 0 the bitmap's byte column bx0, its row 0 line y0): the part c */
void fb_mask(const struct fbmap *b, int bx0, int y0, const unsigned char *mask, int ms, const struct fbr *c, int fg,
             int bg);
/* a planar image (interleaved: a line stride bytes, a plane's line bpr bytes, depth planes) onto chunky d at r from
 * (sx, sy); with a mask (one plane, rows of bpr bytes) only where its bit is 1 */
void fb_planar(const unsigned char *img, unsigned stride, unsigned bpr, int depth, const unsigned char *mask, int sx,
               int sy, const struct fbmap *d, const struct fbr *r);

/* ---- the card ---- */
#define FB_PENS 16
#define FB_PTRW 16                      /* the pointer: 16 x 16, two planes (sprite 0's picture) */
struct fbcard {
    unsigned char *base;                /* the first pixel */
    unsigned bpr;                       /* bytes a row */
    unsigned short w, h;
    unsigned char format, bpp;          /* Picasso96's RGBFormat number, bytes a pixel */
    unsigned char pix[FB_PENS + 3][4];  /* the pens' bytes as the card stores them, then the pointer's three colours */
};
/* 1 if format is one the server draws (the hi- and true-colour ones; CLUT (1) at 1 byte only for the Pi's picture,
 * D63, whose palette the server writes itself; not YUV) with bpp bytes a pixel */
int fb_format_ok(int format, int bpp);
/* 1 if a card record's sizes are ones the server draws on: 320-4096 x 200-4096 pixels, bpp bytes a pixel, a row of
 * at least width * bpp and at most 65,536 bytes (so bpr * height fits in 32 bits: audit A6-27) */
int fb_size_ok(unsigned width, unsigned height, unsigned bpp, unsigned bpr);
/* the card's bytes for a 12-bit colour 0x0RGB in its format (bpp of them into out) */
void fb_colour(int format, unsigned rgb12, unsigned char out[4]);
/* the pens' table: n colours of pal (12-bit), then the pointer's (outline, face, both) */
void fb_pens(struct fbcard *c, const unsigned short *pal, int n, const unsigned short ptrcol[3]);
/* r (clipped here to the card; the pens' bitmap s is the card's size) of s into the card, then the part of the
 * pointer inside r: its picture's top left at (px, py), planes a and b (bit 15 the left pixel); px < -16: none */
void fb_put(const struct fbcard *c, const struct fbmap *s, const struct fbr *r, int px, int py,
            const unsigned short *pa, const unsigned short *pb);
