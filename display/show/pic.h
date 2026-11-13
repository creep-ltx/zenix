/* SPDX-License-Identifier: GPL-2.0-only */
/* The picture decoders and the renderer of show (DESIGN D50, docs/d50-viewers.md): IFF ILBM (EA IFF 85, the ILBM
 * spec: BMHD, CMAP, CAMG, BODY; ByteRun1; 1-8 planes, EHB, HAM6, HAM8, 24 planes) and GIF (87a/89a, the first image).
 * No system calls and no libz: show and the host test (tools/show-host-test.c) build the same file. Everything read
 * is checked; a damaged or hostile file gives a reason and nothing else. */
#pragma once

#define PIC_MAXW 4096           /* pixels across, and lines */
#define PIC_MAXH 4096
#define PIC_MAXBYTES (4u << 20) /* a decoded picture's memory */

#define PIC_HAM6   0x01
#define PIC_HAM8   0x02
#define PIC_EHB    0x04
#define PIC_HIRES  0x08         /* 640-pixel-wide pixels (CAMG, or guessed from the size) */
#define PIC_SHRES  0x10         /* super-hires: 1280 */
#define PIC_LACE   0x20         /* interlaced lines: half as tall */
#define PIC_SQUARE 0x40         /* square pixels (GIF: a PC's picture) */
#define PIC_RGB    0x80         /* 24 planes: 8 bits a gun */
#define PIC_CHUNKY 0x100        /* data is one byte a pixel (GIF), else planar rows (ILBM) */
#define PIC_CUT    0x200        /* the file ended inside the picture: the rest is colour 0 */
#define PIC_NOCAMG 0x400        /* the mode was guessed (no or an unusable CAMG) */

struct pic {
    int w, h;                   /* pixels */
    int depth;                  /* planes (ILBM: 1-8, 24), 8 for GIF */
    unsigned flags;             /* PIC_* */
    int ncol;                   /* colours in pal */
    unsigned camg;              /* as read (0: none) */
    unsigned char pal[256 * 3]; /* 8 bits a gun; EHB's 32..63 the halves */
    unsigned char *data;        /* planar: h rows of depth planes of rowbytes; chunky: h rows of w */
    unsigned rowbytes;          /* a plane's row (planar), or w (chunky) */
    unsigned stride;            /* a whole row */
    const char *fmt;            /* "ILBM", "GIF" */
};

/* the file f (n bytes) -> 0 and p filled (p->data from alloc; the caller frees it), or the reason (nothing kept:
 * what was allocated is given back through release) */
const char *pic_decode(const unsigned char *f, unsigned n, struct pic *p, void *(*alloc)(unsigned),
                       void (*release)(void *));
/* is it a picture show reads? (the first 12 bytes are enough) -> "ILBM", "GIF" or 0 */
const char *pic_kind(const unsigned char *f, unsigned n);
/* row y as 8-bit RGB: w * 3 bytes at rgb (rgb has room for 4 * w + 24: the rest is scratch) */
void pic_row(const struct pic *p, int y, unsigned char *rgb);
/* the mode in words for a title: "HAM6", "EHB", "32 colours", "24-bit", with ", hires", ", lace" */
void pic_mode(const struct pic *p, char *o, unsigned osz);

/* ---- the renderer: the whole picture scaled (nearest pixel) to a canvas of w x h pixels in the screen's pens,
 * planar, as show draws it ---- */
struct canvas {
    int w, h, depth;            /* depth: planes of pens (1-4) */
    unsigned bpr;               /* a plane's row in bytes (even) */
    unsigned char *p;           /* h rows of depth planes of bpr (rows interleaved) */
};
struct pens {
    int n;                      /* pens (2..16) */
    unsigned char rgb[16][3];
    unsigned char lut[4096];    /* 12-bit colour -> the nearest pen (pens_init) */
};
void pens_init(struct pens *ps, const unsigned short *pal12, int n);    /* the screen's colours, 0x0RGB */
/* render, dither PIC_NEAREST (each pixel the nearest pen), PIC_ORDERED (a 4 x 4 pattern added before the nearest:
 * fast) or PIC_FS (Floyd-Steinberg: the error spread right and below). rowbuf: 4 * p->w + 24 bytes, work: (cw + 2) * 6
 * bytes; xs: cw shorts. The canvas must be allocated (cv->p of h * depth * bpr bytes, depth at most 4) */
#define PIC_NEAREST 0
#define PIC_ORDERED 1
#define PIC_FS 2
void pic_render(const struct pic *p, struct canvas *cv, const struct pens *ps, int dither,
                unsigned char *rowbuf, short *work, unsigned short *xs);
