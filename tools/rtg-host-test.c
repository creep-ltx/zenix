// SPDX-License-Identifier: GPL-2.0-only
/* The display server's drawing on a graphics card (display/server/fb.c, DESIGN D43, docs/d43-rtg.md 10) on the PC:
 * random fills, inverts, copies (also overlapping, every direction), 8 x 8 glyphs at any column with bold, underline
 * and their clipping, font masks and planar images with and without masks, each compared pixel for pixel with a
 * plain model; every format's bytes for known colours; the conversion of a rectangle into a card with the pointer
 * over it, clipped at the card's edges, and nothing written outside. Run by tools/rtg-host-test.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../display/server/fb.h"

typedef unsigned char u8;
typedef unsigned short u16;
static int fails, checks;
static void check(int ok, const char *what, int i)
{
    checks++;
    if (!ok && fails++ < 10)
        printf("FAIL %s (case %d)\n", what, i);
}

#define W 203                           /* not a multiple of 8 or 16 */
#define H 77
static u8 a[H][W], m[H][W];             /* fb.c's bitmap and the model */
static const struct fbmap A = { &a[0][0], W };
static int rnd(int n) { return rand() % n; }
static void rrect(struct fbr *r)
{
    int x0 = rnd(W), y0 = rnd(H);
    r->x0 = (short)x0; r->y0 = (short)y0;
    r->x1 = (short)(x0 + 1 + rnd(W - x0)); r->y1 = (short)(y0 + 1 + rnd(H - y0));
}
static int same(void) { return !memcmp(a, m, sizeof a); }

static void ops(void)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            a[y][x] = m[y][x] = (u8)rnd(16);
    for (int i = 0; i < 4000; i++) {
        struct fbr r;
        rrect(&r);
        int k = rnd(6), pen = rnd(256);         /* pens above 15: only the low four bits count */
        if (k == 0) {
            fb_fill(&A, &r, pen);
            for (int y = r.y0; y < r.y1; y++) for (int x = r.x0; x < r.x1; x++) m[y][x] = (u8)(pen & 15);
            check(same(), "fill", i);
        } else if (k == 1) {
            fb_xor(&A, &r, pen);
            for (int y = r.y0; y < r.y1; y++) for (int x = r.x0; x < r.x1; x++) m[y][x] ^= (u8)(pen & 15);
            check(same(), "invert", i);
        } else if (k == 2) {                    /* a copy within the bitmap, source anywhere it fits */
            int w = r.x1 - r.x0, h = r.y1 - r.y0, sx = rnd(W - w + 1), sy = rnd(H - h + 1);
            static u8 t[H][W];
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) t[y][x] = m[sy + y][sx + x];
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) m[r.y0 + y][r.x0 + x] = t[y][x];
            fb_copy(&A, sx, sy, &A, &r);
            check(same(), "copy (overlapping)", i);
        } else if (k == 3) {                    /* a glyph, its cell anywhere (also partly off), clipped */
            u8 g[8];
            for (int j = 0; j < 8; j++) g[j] = (u8)rnd(256);
            int x = rnd(W + 8) - 8, y = rnd(H + 8) - 8, fg = rnd(16), bg = rnd(16), at = rnd(4);
            struct fbr c = r;                   /* the clip: r and the cell */
            if (c.x0 < x) c.x0 = (short)x;
            if (c.y0 < y) c.y0 = (short)y;
            if (c.x1 > x + 8) c.x1 = (short)(x + 8);
            if (c.y1 > y + 8) c.y1 = (short)(y + 8);
            if (c.x0 >= c.x1 || c.y0 >= c.y1) continue;
            fb_glyph(&A, x, y, g, fg, bg, at, &c);
            for (int yy = c.y0; yy < c.y1; yy++) {
                int row = yy - y, bits = g[row];
                if (at & FBA_BOLD) bits |= bits >> 1;
                if ((at & FBA_UNDER) && row == 7) bits = 0xFF;
                for (int xx = c.x0; xx < c.x1; xx++)
                    m[yy][xx] = (u8)(bits >> (7 - (xx - x)) & 1 ? fg : bg);
            }
            check(same(), "glyph", i);
        } else if (k == 4) {                    /* a font run's mask: byte column bx0, ms bytes a row */
            int bx0 = r.x0 >> 3, ms = ((r.x1 - 1) >> 3) - bx0 + 1 + rnd(3), y0 = r.y0 - rnd(r.y0 + 1);
            int rows = r.y1 - y0, fg = rnd(16), bg = rnd(16);
            u8 *mk = malloc((size_t)(ms * rows));
            for (int j = 0; j < ms * rows; j++) mk[j] = (u8)rnd(256);
            fb_mask(&A, bx0, y0, mk, ms, &r, fg, bg);
            for (int y = r.y0; y < r.y1; y++)
                for (int x = r.x0; x < r.x1; x++)
                    m[y][x] = (u8)(mk[(y - y0) * ms + (x >> 3) - bx0] >> (7 - (x & 7)) & 1 ? fg : bg);
            free(mk);
            check(same(), "font mask", i);
        } else {                                /* a planar image (interleaved), with or without a mask */
            int w = r.x1 - r.x0, h = r.y1 - r.y0, depth = 1 + rnd(4), sx = rnd(9), sy = rnd(5);
            int iw = sx + w + rnd(9), ih = sy + h, bpr = (iw + 15) / 16 * 2, stride = bpr * depth;
            u8 *img = malloc((size_t)(stride * ih)), *mk = rnd(2) ? malloc((size_t)(bpr * ih)) : 0;
            for (int j = 0; j < stride * ih; j++) img[j] = (u8)rnd(256);
            for (int j = 0; mk && j < bpr * ih; j++) mk[j] = (u8)rnd(256);
            fb_planar(img, (unsigned)stride, (unsigned)bpr, depth, mk, sx, sy, &A, &r);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int ix = sx + x, iy = sy + y, v = 0;
                    if (mk && !(mk[iy * bpr + ix / 8] >> (7 - ix % 8) & 1)) continue;
                    for (int p = 0; p < depth; p++)
                        v |= (img[iy * stride + p * bpr + ix / 8] >> (7 - ix % 8) & 1) << p;
                    m[r.y0 + y][r.x0 + x] = (u8)v;
                }
            free(img);
            free(mk);
            check(same(), mk ? "planar image with a mask" : "planar image", i);
        }
    }
    /* a copy between two bitmaps */
    static u8 b2[H][W];
    struct fbmap B = { &b2[0][0], W };
    struct fbr r = { 3, 5, 120, 60 };
    fb_copy(&A, 10, 2, &B, &r);
    int ok = 1;
    for (int y = r.y0; y < r.y1; y++) for (int x = r.x0; x < r.x1; x++) ok &= b2[y][x] == a[y - 3][x + 7];
    check(ok, "copy to another bitmap", 0);
}

/* 0xD54 (0xDD, 0x55, 0x44) in each of Picasso96's formats, as the card's memory holds it */
static const struct { int format, bpp; u8 b[4]; } want[] = {
    { 2, 3, { 0xDD, 0x55, 0x44 } },             /* R8G8B8 */
    { 3, 3, { 0x44, 0x55, 0xDD } },             /* B8G8R8 */
    { 4, 2, { 0xA8, 0xDA } },                   /* R5G6B5PC: 11011 010101 01000 = 0xDAA8, low byte first */
    { 5, 2, { 0x48, 0x6D } },                   /* R5G5B5PC: 0 11011 01010 01000 = 0x6D48, low byte first */
    { 6, 4, { 0x00, 0xDD, 0x55, 0x44 } },       /* A8R8G8B8 */
    { 7, 4, { 0x00, 0x44, 0x55, 0xDD } },       /* A8B8G8R8 */
    { 8, 4, { 0xDD, 0x55, 0x44, 0x00 } },       /* R8G8B8A8 */
    { 9, 4, { 0x44, 0x55, 0xDD, 0x00 } },       /* B8G8R8A8 */
    { 10, 2, { 0xDA, 0xA8 } },                  /* R5G6B5 */
    { 11, 2, { 0x6D, 0x48 } },                  /* R5G5B5 */
    { 12, 2, { 0xBB, 0x42 } },                  /* B5G6R5PC: 01000 010101 11011 = 0x42BB, low byte first */
    { 13, 2, { 0x5B, 0x21 } },                  /* B5G5R5PC: 0 01000 01010 11011 = 0x215B */
};

static void formats(void)
{
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        u8 o[4];
        fb_colour(want[i].format, 0xD54, o);
        int ok = fb_format_ok(want[i].format, want[i].bpp) && !memcmp(o, want[i].b, (size_t)want[i].bpp);
        if (!ok)
            printf("  format %d: %02X %02X %02X %02X\n", want[i].format, o[0], o[1], o[2], o[3]);
        check(ok, "a format's bytes", want[i].format);
        check(!fb_format_ok(want[i].format, want[i].bpp == 4 ? 2 : want[i].bpp + 1), "a format with the wrong size",
              want[i].format);
    }
    check(!fb_format_ok(1, 1) && !fb_format_ok(0, 1) && !fb_format_ok(14, 2), "CLUT, planar and YUV refused", 0);
    u8 o[4];
    fb_colour(6, 0xFFF, o);
    check(o[1] == 0xFF && o[2] == 0xFF && o[3] == 0xFF, "white is 0xFF", 0);
    fb_colour(10, 0xFFF, o);
    check(o[0] == 0xFF && o[1] == 0xFF, "white is all ones in 16 bits", 0);
}

static void put(int format, int bpp)
{
    enum { CW = 101, CH = 53, PAD = 7 };
    int bpr = CW * bpp + PAD;
    static u8 mem[(CH + 2) * (CW * 4 + PAD)];
    struct fbcard c;
    memset(&c, 0, sizeof c);
    c.base = mem + bpr;                         /* one guard row before */
    c.bpr = (unsigned)bpr;
    c.w = CW; c.h = CH;
    c.format = (u8)format; c.bpp = (u8)bpp;
    u16 pal[16], ptr[3] = { 0x000, 0xFFF, 0x888 }, pa[16], pb[16];
    for (int i = 0; i < 16; i++) pal[i] = (u16)(rnd(4096));
    for (int i = 0; i < 16; i++) { pa[i] = (u16)rnd(65536); pb[i] = (u16)rnd(65536); }
    fb_pens(&c, pal, 16, ptr);
    static u8 pens[CH][CW];
    for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++) pens[y][x] = (u8)rnd(16);
    struct fbmap s = { &pens[0][0], CW };
    for (int i = 0; i < 300; i++) {
        memset(mem, 0xA5, sizeof mem);
        struct fbr r = { (short)(rnd(CW + 20) - 10), (short)(rnd(CH + 20) - 10), 0, 0 };
        r.x1 = (short)(r.x0 + rnd(60)); r.y1 = (short)(r.y0 + rnd(40));
        int px = rnd(CW + 32) - 16, py = rnd(CH + 32) - 16;
        fb_put(&c, &s, &r, px, py, pa, pb);
        int ok = 1;
        for (int y = -1; y <= CH; y++)         /* the guard rows before and after too */
            for (int x = 0; x < CW; x++) {
                const u8 *p = mem + (y + 1) * bpr + x * bpp;
                int in = y >= 0 && y < CH && x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
                u8 e[4] = { 0xA5, 0xA5, 0xA5, 0xA5 };
                if (in) {
                    int k = 0;
                    if (x >= px && x < px + 16 && y >= py && y < py + 16)
                        k = (pa[y - py] >> (15 - (x - px)) & 1) | (pb[y - py] >> (15 - (x - px)) & 1) << 1;
                    u8 v[4];
                    fb_colour(format, k ? ptr[k - 1] : pal[pens[y][x]], v);
                    memcpy(e, v, (size_t)bpp);
                }
                ok &= !memcmp(p, e, (size_t)bpp);
            }
        for (int y = 0; y < CH; y++)            /* each row's padding never written */
            for (int j = CW * bpp; j < bpr; j++)
                ok &= mem[(y + 1) * bpr + j] == 0xA5;
        check(ok, "a rectangle onto the card with the pointer, clipped", format * 1000 + i);
    }
}

int main(void)
{
    srand(43);
    ops();
    formats();
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++)
        put(want[i].format, want[i].bpp);
    printf("%s rtg-host-test: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
