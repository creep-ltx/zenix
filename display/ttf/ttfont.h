/* SPDX-License-Identifier: GPL-2.0-only */
/* A TrueType font as an AmigaOS bitmap font (DESIGN D51, docs/d51-truetype.md): the characters 32-126 and 160-255
 * drawn by ttf.c at one size and written as the NAME/SIZE file the Amiga font reader (display/server/amifont.c) and
 * AmigaOS's diskfont read, with a stamp naming the .ttf it came from; the NAME.font contents file; the console's
 * ZCF1 supplement (DESIGN D28 7) at 8 x 8. No system calls: ttf2font and the host test build the same file. */
#pragma once
#include "ttf.h"

#define TTF_STAMP_VER 1         /* the renderer's version: a cache file of another version is made again */
#define TTF_XR 120              /* the em's width for its height, in 64ths: a hires screen's pixel is 1.875 times as
                                   tall as wide (640 x 256 PAL on a 4:3 picture), so a glyph takes as many more columns
                                   to look as drawn (docs/d51-truetype.md "The pixels' shape") */

struct ttf_size {               /* how the size was laid out (for the messages and the tests) */
    int h, base, em64, nglyphs, prop;
    int wide;                   /* glyphs left out: wider than 64 pixels or advancing past 255 (the 8 x 8 stands in) */
};

/* the font file for size lines (4-64) from t, its width xr / 64 of its height (TTF_XR): *out (from alloc, *outn bytes; the caller frees it), its stamp the
 * .ttf's byte count and time; name the font's name (at most 25 bytes, "NAME" of NAME.font) -> 0, or the reason */
const char *ttf_amifont(const struct ttf *t, int size, int xr, const char *name, unsigned tsize, unsigned tmtime,
                        void *(*alloc)(unsigned), void (*release)(void *), unsigned char **out, unsigned *outn,
                        struct ttf_size *how);
/* the contents file NAME.font with size added (old: the file there, oldn 0 for none; its sizes kept with their style
 * and flags when it is a plain FontContentsHeader, 0x0f00 - another kind is replaced): written to out (at most 4 + 260 * 64 bytes) -> its length, or 0 when it already lists 64 other sizes */
unsigned ttf_contents(const unsigned char *old, unsigned oldn, const char *name, int size, int prop,
                      unsigned char *out);
/* the console's Unicode font (ZCF1, 8 x 8, the code points past U+00FF the font has, at most 4096 glyphs): *out from
 * alloc -> 0, or the reason */
const char *ttf_zcf(const struct ttf *t, void *(*alloc)(unsigned), void (*release)(void *), unsigned char **out,
                    unsigned *outn, int *nglyphs);

/* the stamp at a cache file's end: HUNK_DEBUG, 4 longs ("ZTTF", the .ttf's bytes, its time, the version << 16 | the
 * size), HUNK_END. 1 when the file d (n bytes) carries the stamp of this .ttf at this size and this version. */
static inline int ttf_stamp_ok(const unsigned char *d, unsigned n, unsigned tsize, unsigned tmtime, int size)
{
    if (n < 28)
        return 0;
    const unsigned char *s = d + n - 28;
    unsigned v[7];
    for (int i = 0; i < 7; i++)
        v[i] = (unsigned)s[4 * i] << 24 | (unsigned)s[4 * i + 1] << 16 | (unsigned)s[4 * i + 2] << 8 | s[4 * i + 3];
    return v[0] == 0x3F1 && v[1] == 4 && v[2] == 0x5A545446u && v[3] == tsize && v[4] == tmtime &&
           v[5] == ((unsigned)TTF_STAMP_VER << 16 | (unsigned)size) && v[6] == 0x3F2;
}
