/* SPDX-License-Identifier: GPL-2.0-only */
/* The display server's chunky drawing and the card's pixels (DESIGN D43, docs/d43-rtg.md 4; fb.h). Cold enough for
 * -Os except the conversion's inner loops, which are a load, a table look-up and a store a pixel. */
#include "fb.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned u32;

void *memset(void *, int, unsigned long);
void *memmove(void *, const void *, unsigned long);
void *memcpy(void *, const void *, unsigned long);

static u8 *at(const struct fbmap *b, int x, int y) { return b->p + (u32)y * b->stride + (u32)x; }

void fb_fill(const struct fbmap *b, const struct fbr *r, int pen)
{
    u32 w = (u32)(r->x1 - r->x0);
    for (int y = r->y0; y < r->y1; y++)
        memset(at(b, r->x0, y), pen & 15, w);
}

void fb_xor(const struct fbmap *b, const struct fbr *r, int pen)
{
    u8 m = (u8)(pen & 15);
    for (int y = r->y0; y < r->y1; y++) {
        u8 *p = at(b, r->x0, y);
        for (int x = r->x0; x < r->x1; x++)
            *p++ ^= m;
    }
}

void fb_copy(const struct fbmap *s, int sx, int sy, const struct fbmap *d, const struct fbr *r)
{
    int h = r->y1 - r->y0, up = s->p == d->p && r->y0 > sy;   /* the same bitmap downwards: the last row first */
    u32 w = (u32)(r->x1 - r->x0);
    for (int j = 0; j < h; j++) {
        int k = up ? h - 1 - j : j;
        memmove(at(d, r->x0, r->y0 + k), at(s, sx, sy + k), w);    /* (memmove: one row overlapping sideways) */
    }
}

void fb_glyph(const struct fbmap *b, int x, int y, const u8 *g, int fg, int bg, int attr, const struct fbr *c)
{
    u8 f = (u8)(fg & 15), k = (u8)(bg & 15);
    for (int yy = c->y0; yy < c->y1; yy++) {
        int row = yy - y;
        u8 bits = g[row];
        if (attr & FBA_BOLD)
            bits = (u8)(bits | bits >> 1);
        if ((attr & FBA_UNDER) && row == 7)
            bits = 0xFF;
        u8 *p = at(b, c->x0, yy);
        for (int xx = c->x0; xx < c->x1; xx++)
            *p++ = (bits << (xx - x)) & 0x80 ? f : k;
    }
}

void fb_mask(const struct fbmap *b, int bx0, int y0, const u8 *mask, int ms, const struct fbr *c, int fg, int bg)
{
    u8 f = (u8)(fg & 15), k = (u8)(bg & 15);
    for (int y = c->y0; y < c->y1; y++) {
        const u8 *m = mask + (u32)(y - y0) * (u32)ms - bx0;     /* m[x >> 3]: the byte of the pixel column x */
        u8 *p = at(b, c->x0, y);
        for (int x = c->x0; x < c->x1; x++)
            *p++ = (m[x >> 3] << (x & 7)) & 0x80 ? f : k;
    }
}

void fb_planar(const u8 *img, unsigned stride, unsigned bpr, int depth, const u8 *mask, int sx, int sy,
               const struct fbmap *d, const struct fbr *r)
{
    for (int y = r->y0; y < r->y1; y++) {
        int iy = sy + y - r->y0;
        const u8 *l = img + (u32)iy * stride, *ml = mask ? mask + (u32)iy * bpr : 0;
        u8 *p = at(d, r->x0, y);
        for (int x = r->x0; x < r->x1; x++, p++) {
            int ix = sx + x - r->x0, sh = 7 - (ix & 7);
            u32 o = (u32)ix >> 3;
            if (ml && !(ml[o] >> sh & 1))
                continue;
            u8 v = 0;
            for (int k = 0; k < depth && k < 4; k++)
                v |= (u8)((l[(u32)k * bpr + o] >> sh & 1) << k);
            *p = v;
        }
    }
}

/* ---- the card ---- */
/* Picasso96's RGBFormat numbers (its interface file, libraries/Picasso96.h: names and numbers only) */
enum { RGBFB_CLUT = 1, RGBFB_R8G8B8 = 2, RGBFB_B8G8R8, RGBFB_R5G6B5PC, RGBFB_R5G5B5PC, RGBFB_A8R8G8B8, RGBFB_A8B8G8R8, RGBFB_R8G8B8A8,
       RGBFB_B8G8R8A8, RGBFB_R5G6B5, RGBFB_R5G5B5, RGBFB_B5G6R5PC, RGBFB_B5G5R5PC };

int fb_format_ok(int format, int bpp)
{
    if (format == RGBFB_CLUT)
        return bpp == 1;                /* D63: the Pi's 8-bit picture, whose palette its driver gives (d63 8) */
    if (format == RGBFB_R8G8B8 || format == RGBFB_B8G8R8)
        return bpp == 3;
    if (format >= RGBFB_A8R8G8B8 && format <= RGBFB_B8G8R8A8)
        return bpp == 4;
    if (format == RGBFB_R5G6B5PC || format == RGBFB_R5G5B5PC || (format >= RGBFB_R5G6B5 && format <= RGBFB_B5G5R5PC))
        return bpp == 2;
    return 0;
}

int fb_size_ok(unsigned width, unsigned height, unsigned bpp, unsigned bpr)
{
    return width >= 320 && height >= 200 && width <= 4096 && height <= 4096 && bpp >= 1 && bpp <= 4 &&
           bpr >= width * bpp && bpr <= 65536;
}

void fb_colour(int format, unsigned rgb12, u8 out[4])
{
    u32 r = (rgb12 >> 8 & 15) * 17, g = (rgb12 >> 4 & 15) * 17, b = (rgb12 & 15) * 17, v = 0;
    int pc = 0;
    out[0] = out[1] = out[2] = out[3] = 0;     /* the unused alpha byte 0, as Picasso96's own drawing leaves it */
    switch (format) {
    case RGBFB_R8G8B8:   out[0] = (u8)r; out[1] = (u8)g; out[2] = (u8)b; return;
    case RGBFB_B8G8R8:   out[0] = (u8)b; out[1] = (u8)g; out[2] = (u8)r; return;
    case RGBFB_A8R8G8B8: out[1] = (u8)r; out[2] = (u8)g; out[3] = (u8)b; return;
    case RGBFB_A8B8G8R8: out[1] = (u8)b; out[2] = (u8)g; out[3] = (u8)r; return;
    case RGBFB_R8G8B8A8: out[0] = (u8)r; out[1] = (u8)g; out[2] = (u8)b; return;
    case RGBFB_B8G8R8A8: out[0] = (u8)b; out[1] = (u8)g; out[2] = (u8)r; return;
    case RGBFB_R5G6B5PC: pc = 1; /* fall through */
    case RGBFB_R5G6B5:   v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
    case RGBFB_R5G5B5PC: pc = 1; /* fall through */
    case RGBFB_R5G5B5:   v = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
    case RGBFB_B5G6R5PC: v = (b >> 3) << 11 | (g >> 2) << 5 | r >> 3; pc = 1; break;
    case RGBFB_B5G5R5PC: v = (b >> 3) << 10 | (g >> 3) << 5 | r >> 3; pc = 1; break;
    default: return;
    }
    out[pc ? 1 : 0] = (u8)(v >> 8);             /* PC: the low byte first */
    out[pc ? 0 : 1] = (u8)v;
}

void fb_pens(struct fbcard *c, const unsigned short *pal, int n, const unsigned short ptrcol[3])
{
    for (int i = 0; i < FB_PENS + 3; i++) {
        fb_colour(c->format, i < FB_PENS ? (i < n ? pal[i] : 0) : ptrcol[i - FB_PENS], c->pix[i]);
        if (c->format == RGBFB_CLUT)
            c->pix[i][0] = (u8)i;       /* D63: a pen is its own byte, the pointer's colours entries 16-18 */
    }
}

/* a row of pens into the card: n pixels from s to d (the card's bytes) */
static void put_row(const struct fbcard *c, u8 *d, const u8 *s, int n)
{
    if (c->bpp == 4) {
        u32 t[FB_PENS];
        for (int i = 0; i < FB_PENS; i++)
            memcpy(&t[i], c->pix[i], 4);        /* the bytes in the card's order whatever this CPU's order is */
        if (!((unsigned long)d & 3)) {
            u32 *q = (u32 *)d;
            while (n--)
                *q++ = t[*s++ & 15];
            return;
        }
        while (n--) {
            memcpy(d, &t[*s++ & 15], 4);
            d += 4;
        }
    } else if (c->bpp == 2) {
        u16 t[FB_PENS];
        for (int i = 0; i < FB_PENS; i++)
            memcpy(&t[i], c->pix[i], 2);
        if (!((unsigned long)d & 1)) {
            u16 *q = (u16 *)d;
            while (n--)
                *q++ = t[*s++ & 15];
            return;
        }
        while (n--) {
            memcpy(d, &t[*s++ & 15], 2);
            d += 2;
        }
    } else if (c->bpp == 1) {
        memcpy(d, s, (unsigned long)n);  /* D63: the pens are the picture's bytes (fb_fill and the rest keep them 0-15) */
    } else {
        while (n--) {
            const u8 *p = c->pix[*s++ & 15];
            *d++ = p[0];
            *d++ = p[1];
            *d++ = p[2];
        }
    }
}

void fb_put(const struct fbcard *c, const struct fbmap *s, const struct fbr *rr, int px, int py, const u16 *pa,
            const u16 *pb)
{
    struct fbr r = *rr;
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > c->w) r.x1 = (short)c->w;
    if (r.y1 > c->h) r.y1 = (short)c->h;
    if (r.x0 >= r.x1 || r.y0 >= r.y1)
        return;
    u32 bp = c->bpp;
    for (int y = r.y0; y < r.y1; y++) {
        u8 *d = c->base + (u32)y * c->bpr + (u32)r.x0 * bp;
        put_row(c, d, at(s, r.x0, y), r.x1 - r.x0);
        int row = y - py;
        if (row < 0 || row >= FB_PTRW || px + FB_PTRW <= r.x0 || px >= r.x1)
            continue;
        for (int x = px > r.x0 ? px : r.x0; x < px + FB_PTRW && x < r.x1; x++) {
            int k = (pa[row] << (x - px) & 0x8000 ? 1 : 0) | (pb[row] << (x - px) & 0x8000 ? 2 : 0);
            if (k)
                memcpy(c->base + (u32)y * c->bpr + (u32)x * bp, c->pix[FB_PENS + k - 1], bp);
        }
    }
}
