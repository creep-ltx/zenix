/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* ilbm - IFF pictures (FORM ILBM and FORM PBM) without datatypes.
 *
 * Plain C, no Amiga calls, the shape of core/arc.h: the program supplies
 * the bytes (struct arcsrc) and lends the memory (arc_alloc/arc_free).
 * ilbm_open reads the header chunks (BMHD, CMAP, CAMG; the rest is
 * skipped, odd lengths padded); ilbm_decode streams BODY (raw or
 * ByteRun1, a mask plane skipped) row by row to a callback, as planar
 * rows (straight into Amiga bitplanes) and/or as chunky pen numbers
 * (for RTG). Nothing holds the whole picture. Damaged or crafted input
 * gives an ARC_ error code: never a read outside a buffer, never an
 * endless loop.
 *
 * Also the colour work a viewer needs when the screen cannot show the
 * picture as it is: rows resolved to RGB (palette, EHB, HAM6/HAM8,
 * 24-bit), a 4096-bin histogram, picking N colours from it, and a HAM6
 * encoder - plain C so it is proven on the host too.
 */
#ifndef CF_ILBM_H
#define CF_ILBM_H

#include "arc.h"

#define ILBM_MAXW 16384
#define ILBM_MAXH 16384

/* CAMG bits (the classic ViewModes, = the low word of a mode id) */
#define ILBM_CAMG_LACE  0x0004
#define ILBM_CAMG_SUPER 0x0020
#define ILBM_CAMG_EHB   0x0080
#define ILBM_CAMG_HAM   0x0800
#define ILBM_CAMG_HIRES 0x8000

struct ilbm {
    int w, h;               /* pixels */
    int depth;              /* 1-8, or 24 (deep ILBM: 8 R, 8 G, 8 B planes) */
    int pbm;                /* FORM PBM: chunky bytes in BODY */
    int masking;            /* BMHD masking (1 = a mask plane follows each row) */
    int compress;           /* 0 raw, 1 ByteRun1 */
    int transparent;        /* BMHD transparentColor */
    int xasp, yasp;         /* BMHD aspect */
    int pagew, pageh;       /* BMHD page size */
    int ncolors;            /* CMAP entries read (0 = none: a grey ramp is made) */
    unsigned char pal[256 * 3]; /* 8-bit R, G, B; 4-bit CMAPs scaled up (x | x >> 4) */
    int cmap4;              /* the CMAP was 4-bit style and has been scaled up */
    unsigned long camg;     /* the cleaned CAMG (0 = none or a bad one) */
    int hascamg;
    int hasbmhd;            /* a BMHD was read (with no BODY: a palette-only file) */
    /* what to show it as - from CAMG, else guessed from size and aspect */
    int ham;                /* 0, 6 or 8 (the plane count of a HAM picture) */
    int ehb;
    int hires, lace, superhires;
    unsigned long bodyoff, bodylen;     /* BODY data in the file */
    int bpr;                /* bytes per planar row (even) */
};

/* parse up to BODY: ARC_OK, ARC_ECORRUPT (not an IFF picture or a broken
 * one), ARC_EUNSUP (a picture of a kind not read here), ARC_EREAD */
int ilbm_open(const struct arcsrc *src, struct ilbm *p);

/* one decoded row, as the callback sees it (valid during the call only) */
struct ilbmrow {
    int y;
    int depth;              /* = p->depth */
    int bpr;                /* bytes per row of each plane */
    unsigned char *plane[24];   /* planar: plane[0] the lowest bit (ILBM_PLANAR) */
    unsigned char *chunky;      /* one byte per pixel, w of them (ILBM_CHUNKY, depth <= 8) */
};
/* return 1 to go on, 0 to stop (then ilbm_decode gives ARC_ESTOP) */
typedef int (*ilbm_rowfn)(void *ctx, const struct ilbmrow *r);

#define ILBM_PLANAR 1
#define ILBM_CHUNKY 2

/* decode BODY, calling fn for rows 0..h-1 in order. want: ILBM_PLANAR
 * and/or ILBM_CHUNKY. A BODY that ends early still delivers every row
 * (the missing ones blank) and then returns ARC_ECORRUPT. */
int ilbm_decode(const struct arcsrc *src, const struct ilbm *p, int want, ilbm_rowfn fn, void *ctx);

/* planar row -> chunky pens (depth <= 8), and back */
void ilbm_c2chunky(unsigned char *const *plane, int depth, int w, unsigned char *out);
void ilbm_c2planar(const unsigned char *in, int depth, int w, unsigned char *const *plane, int bpr);

/* a row resolved to 8-bit RGB triples (w * 3 bytes), as the picture's
 * own display would show it: palette, EHB, HAM6/HAM8 (from colour 0 at
 * the row start, as the hardware does), or 24-bit */
void ilbm_rgbrow(const struct ilbm *p, const struct ilbmrow *r, unsigned char *rgb);

/* ---- colour reduction ---------------------------------------------- */
/* a histogram of 12-bit colours: 4096 counters, saturating */
void ilbm_hist(unsigned short *hist, const unsigned char *rgb, int w);
/* pick n colours (<= 256) from the histogram into pal (8-bit RGB):
 * the most used first, then whatever is used AND far from the picked
 * ones. Returns how many were picked (fewer when fewer were used). */
int ilbm_pickpal(const unsigned short *hist, int n, unsigned char *pal);
/* nearest of pal[0..n-1] to each 12-bit colour: lut[4096], filled
 * lazily (0xFFFF = not yet) through ilbm_nearest */
int ilbm_nearest(unsigned short *lut, const unsigned char *pal, int n, int r, int g, int b);
/* an RGB row -> HAM6 pens (0-63) over a 16-colour base palette pal;
 * lut[4096] is the caller's, 0xFFFF-filled before the first row (it
 * caches each colour's nearest base colour) */
void ilbm_ham6row(const unsigned char *rgb, int w, const unsigned char *pal, unsigned char *out, unsigned short *lut);

#endif
