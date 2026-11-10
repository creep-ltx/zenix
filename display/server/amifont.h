/* SPDX-License-Identifier: GPL-2.0-only */
/* The AmigaOS bitmap font reader (docs/d38-desktop.md "Fonts"): a NAME.font contents file and a NAME/SIZE font file
 * (an AmigaDOS hunk executable holding a DiskFontHeader and its TextFont), read from memory and converted at load
 * into one packed picture a glyph. No system calls and no libz: displayd and the host test (tools/amifont-host-test.c)
 * build the same file. Everything is checked; a damaged or hostile file gives a reason and nothing else. */
#pragma once

#define AF_MAXH 64              /* lines: a taller font is not for the desktop's bars */
#define AF_MAXW 64              /* a glyph's picture, pixels */
#define AF_MAXBYTES 65535u      /* the converted pictures together */
#define AF_MAXFILE (256u << 10) /* a font file read whole */
#define AF_MAXCONTENTS (4u + 260u * 64u)   /* a contents file: 64 sizes */

struct af_glyph {
    unsigned short off;         /* its picture in bits[]: h rows of bpr bytes, the leftmost pixel in bit 7 */
    unsigned char w, bpr;       /* its picture's width in pixels, bytes a row */
    signed char kern;           /* drawn this far right of the pen (tf_CharKern) */
    unsigned char adv;          /* the pen moves this far (tf_CharSpace + kern, or tf_XSize) */
    unsigned char have;         /* the font has it (else the 8 x 8 stands in) */
    unsigned char pad;
};
struct af_font {
    unsigned short h, base, xsize;  /* tf_YSize, tf_Baseline (below h), tf_XSize */
    unsigned char style, flags;     /* tf_Style, tf_Flags */
    unsigned char lo, hi;           /* tf_LoChar, tf_HiChar */
    unsigned char prop, colour;     /* proportional (FPF_PROPORTIONAL and a spacing table); a colour font read as one */
    unsigned short nglyphs;         /* code points it has */
    unsigned nbytes;                /* bits[]'s size */
    struct af_glyph g[256];
    unsigned char *bits;
};

/* the contents file d (n bytes): the entry for ysize (plain style preferred) -> 0 and its file name ("NAME/SIZE",
 * relative, checked) in file; or the reason. sizes (if not 0) gets the sizes it lists ("8 11 13"), for a message. */
const char *af_contents(const unsigned char *d, unsigned n, int ysize, char *file, unsigned filesz, char *sizes,
                        unsigned sizessz);
/* the font file d (n bytes) -> 0 and f filled, f->bits from alloc (its size f->nbytes; the caller frees it); or the
 * reason (nothing allocated). With alloc 0 the file is only checked (every check made, f->bits not converted): ZINE's
 * diskfont (user/zine/bfont.c) then loads the file as AmigaOS does. */
const char *af_load(const unsigned char *d, unsigned n, struct af_font *f, void *(*alloc)(unsigned));
