/* SPDX-License-Identifier: GPL-2.0-only */
/* displayd's drawing (DESIGN D38 3.2, 6.3): bitmaps of interleaved planes in chip RAM, filled, inverted and copied by
 * the blitter while the console is shown (use_blit) and by the CPU while it is not (the kernel has the blitter then:
 * GEV_RELEASE), text by the CPU a byte per plane as the console draws it. Rectangles and regions for the clipping.
 * Every rectangle is clipped to its bitmap here, so no order can write outside the screen or a kept copy. */
#include "dpyd.h"

#define CUST(o) (*(volatile u16 *)(0xDFF000 + (o)))
#define DMACONR CUST(0x002)
#define BLTCON0 CUST(0x040)
#define BLTCON1 CUST(0x042)
#define BLTAFWM CUST(0x044)
#define BLTALWM CUST(0x046)
#define BLTCPTH CUST(0x048)
#define BLTBPTH CUST(0x04C)
#define BLTDPTH CUST(0x054)
#define BLTSIZE CUST(0x058)
#define BLTCMOD CUST(0x060)
#define BLTBMOD CUST(0x062)
#define BLTDMOD CUST(0x066)
#define BLTADAT CUST(0x074)

struct bm scr;
int use_blit;
u32 nblits, npix, nglyphs, ncopies;              /* DPY_STAT's counters: blits started, pixels drawn (a rectangle once, every
                                         * plane), glyphs drawn */
u32 chip_used, chip_budget = 256u << 10;

/* the blitter's busy bit, read twice (the first read may come before the blit starts); a blit of a screen takes tens
 * of milliseconds: past half a second (the E-clock) it is stuck, and drawing goes on (a rule of the project: every wait
 * on hardware has an end) */
void blit_idle(void)
{
    (void)DMACONR;
    if (!(DMACONR & 0x4000))
        return;
    u32 t0 = sys_hrtime();
    while (DMACONR & 0x4000)
        if (sys_hrtime() - t0 > 360000u) {
            dprintf(2, "displayd: the blitter did not finish in 500 ms\n");
            return;
        }
}
static void bptr(u32 reg, const void *p)
{
    CUST(reg) = (u16)((u32)p >> 16);
    CUST(reg + 2) = (u16)(u32)p;
}

/* ---- rectangles and regions ---- */
int r_and(struct rect *o, const struct rect *a, const struct rect *b)
{
    o->x0 = a->x0 > b->x0 ? a->x0 : b->x0;
    o->y0 = a->y0 > b->y0 ? a->y0 : b->y0;
    o->x1 = a->x1 < b->x1 ? a->x1 : b->x1;
    o->y1 = a->y1 < b->y1 ? a->y1 : b->y1;
    return o->x0 < o->x1 && o->y0 < o->y1;
}
static int r_empty(const struct rect *r) { return r->x0 >= r->x1 || r->y0 >= r->y1; }
void rg_set(struct region *g, const struct rect *r)
{
    g->n = 0;
    g->over = 0;
    if (!r_empty(r))
        g->r[g->n++] = *r;
}
static int lost_said;
static void put(struct region *g, s16 x0, s16 y0, s16 x1, s16 y1)
{
    if (x0 >= x1 || y0 >= y1)
        return;
    if (g->n >= RMAX) {                 /* too many pieces: this one is not drawn (never more than is visible) */
        g->over = 1;                    /* and the region says so: whole() is false, a paint draws all again */
        if (!lost_said++)
            dprintf(2, "displayd: a window's visible region has more than %d pieces: some are not drawn\n", RMAX);
        return;
    }
    struct rect *o = &g->r[g->n++];
    o->x0 = x0; o->y0 = y0; o->x1 = x1; o->y1 = y1;
}
/* g minus r: each piece that meets r is cut into up to four (above, below, left, right) */
void rg_sub(struct region *g, const struct rect *r)
{
    struct region o;
    o.n = 0;
    o.over = g->over;
    for (int i = 0; i < g->n; i++) {
        struct rect a = g->r[i], c;
        if (!r_and(&c, &a, r)) {
            put(&o, a.x0, a.y0, a.x1, a.y1);
            continue;
        }
        put(&o, a.x0, a.y0, a.x1, c.y0);
        put(&o, a.x0, c.y1, a.x1, a.y1);
        put(&o, a.x0, c.y0, c.x0, c.y1);
        put(&o, c.x1, c.y0, a.x1, c.y1);
    }
    *g = o;
}
void rg_and(struct region *g, const struct rect *r)
{
    u16 k = 0;
    for (int i = 0; i < g->n; i++) {
        struct rect c;
        if (r_and(&c, &g->r[i], r))
            g->r[k++] = c;
    }
    g->n = k;
}
void rg_add(struct region *g, const struct rect *r)
{
    struct region o = { 0, 0, { { 0, 0, 0, 0 } } };
    rg_set(&o, r);
    for (int i = 0; i < g->n; i++)      /* the new rectangle's own pieces, so no pixel is in two */
        rg_sub(&o, &g->r[i]);
    for (int i = 0; i < o.n; i++)
        put(g, o.r[i].x0, o.r[i].y0, o.r[i].x1, o.r[i].y1);
    g->over |= o.over;
}

/* ---- bitmaps ---- */
int bm_alloc(struct bm *b, int w, int h, int depth)
{
    b->w = (u16)w;
    b->h = (u16)h;
    b->bpr = (u16)((w + 15) / 16 * 2);
    b->depth = (u16)depth;
    b->stride = (u32)b->bpr * depth;
    u32 n = b->stride * (u32)h;
    b->p = n ? sys_memalloc_chip(n) : 0;
    if (!b->p)
        return -ENOMEM;
    memset(b->p, 0, n);
    b->kept = 1;
    chip_used += n;
    return 0;
}
void bm_free(struct bm *b)
{
    if (!b->p)
        return;
    blit_idle();
    if (b->kept)
        chip_used -= b->stride * b->h;
    sys_memfree(b->p);
    b->p = 0;
}
static int clip_bm(const struct bm *b, struct rect *r)
{
    struct rect all = { 0, 0, (s16)b->w, (s16)b->h };
    return r_and(r, r, &all);
}
static u16 fmask(int x) { return (u16)(0xFFFF >> (x & 15)); }
static u16 lmask(int x1) { return (u16)(0xFFFF << (15 - ((x1 - 1) & 15))); }

/* minterms: A the mask (BLTADAT with the edge masks), C and D the bitmap */
#define MT_SET 0xFA
#define MT_CLR 0x0A
#define MT_INV 0x5A
static void blit_rect(struct bm *b, const struct rect *r, int plane, u8 mt)
{
    int w0 = r->x0 >> 4, w1 = (r->x1 - 1) >> 4, W = w1 - w0 + 1, h = r->y1 - r->y0;
    u8 *p = b->p + (u32)r->y0 * b->stride + (u32)plane * b->bpr + (u32)w0 * 2;
    while (h > 0) {
        int k = h > 1024 ? 1024 : h;
        blit_idle();
        BLTCON0 = (u16)(0x0300 | mt);   /* C and D on, A from BLTADAT */
        BLTCON1 = 0;
        BLTAFWM = W == 1 ? (u16)(fmask(r->x0) & lmask(r->x1)) : fmask(r->x0);
        BLTALWM = W == 1 ? (u16)(fmask(r->x0) & lmask(r->x1)) : lmask(r->x1);
        BLTADAT = 0xFFFF;
        BLTCMOD = BLTDMOD = (u16)(b->stride - (u32)W * 2);
        bptr(0x048, p);
        bptr(0x054, p);
        BLTSIZE = (u16)((k & 1023) << 6 | W);
        nblits++;
        p += (u32)k * b->stride;
        h -= k;
    }
}
/* the CPU's way: one byte at a time, its edge bits masked */
static void cpu_rect(struct bm *b, const struct rect *r, int plane, int how)  /* how: 0 clear, 1 set, 2 invert */
{
    int bx0 = r->x0 >> 3, bx1 = (r->x1 - 1) >> 3;
    u8 m0 = (u8)(0xFF >> (r->x0 & 7)), m1 = (u8)(0xFF << (7 - ((r->x1 - 1) & 7)));
    for (int y = r->y0; y < r->y1; y++) {
        u8 *l = b->p + (u32)y * b->stride + (u32)plane * b->bpr;
        for (int x = bx0; x <= bx1; x++) {
            u8 m = (u8)((x == bx0 ? m0 : 0xFF) & (x == bx1 ? m1 : 0xFF));
            l[x] = how == 0 ? (u8)(l[x] & ~m) : how == 1 ? (u8)(l[x] | m) : (u8)(l[x] ^ m);
        }
    }
}
void bm_fill(struct bm *b, const struct rect *rr, int pen)
{
    struct rect r = *rr;
    if (!b->p || !clip_bm(b, &r))
        return;
    npix += (u32)(r.x1 - r.x0) * (u32)(r.y1 - r.y0);
    for (int k = 0; k < b->depth; k++) {
        int on = pen >> k & 1;
        if (use_blit)
            blit_rect(b, &r, k, on ? MT_SET : MT_CLR);
        else
            cpu_rect(b, &r, k, on);
    }
}
void bm_xor(struct bm *b, const struct rect *rr, int pen)
{
    struct rect r = *rr;
    if (!b->p || !clip_bm(b, &r))
        return;
    for (int k = 0; k < b->depth; k++)
        if (pen >> k & 1) {
            if (use_blit)
                blit_rect(b, &r, k, MT_INV);
            else
                cpu_rect(b, &r, k, 2);
        }
}

/* the CPU's copy of one plane's rectangle: whole bytes when both sides lie on 8-pixel columns, else pixel by pixel
 * (frames and images only: window insides lie on 8-pixel columns, D38 3.2); rows in the order an overlap needs */
static void cpu_copy(struct bm *s, int sx, int sy, struct bm *d, const struct rect *r, int plane)
{
    int w = r->x1 - r->x0, h = r->y1 - r->y0, up = d == s && r->y0 > sy;
    for (int j = 0; j < h; j++) {
        int yy = up ? h - 1 - j : j;
        const u8 *sl = s->p + (u32)(sy + yy) * s->stride + (u32)plane * s->bpr;
        u8 *dl = d->p + (u32)(r->y0 + yy) * d->stride + (u32)plane * d->bpr;
        if (!(sx & 7) && !(r->x0 & 7) && !(w & 7)) {
            memmove(dl + (r->x0 >> 3), sl + (sx >> 3), (u32)w >> 3);
            continue;
        }
        int back = d == s && r->y0 == sy && r->x0 > sx;
        for (int i = 0; i < w; i++) {
            int ii = back ? w - 1 - i : i, a = sx + ii, b = r->x0 + ii;
            u8 bit = (u8)(sl[a >> 3] >> (7 - (a & 7)) & 1), m = (u8)(0x80 >> (b & 7));
            dl[b >> 3] = bit ? (u8)(dl[b >> 3] | m) : (u8)(dl[b >> 3] & ~m);
        }
    }
}
/* the blitter's copy, ascending (shift right by s >= 0) or, for an overlap that needs it, descending with no shift:
 * A the edge masks, B the source, C and D the destination, D = AB + ~AC */
static void blit_copy(struct bm *s, int sx, int sy, struct bm *d, const struct rect *r, int plane, int desc)
{
    int dw0 = r->x0 >> 4, dw1 = (r->x1 - 1) >> 4, W = dw1 - dw0 + 1, h = r->y1 - r->y0;
    int sh = (r->x0 & 15) - (sx & 15), sw0 = sx >> 4;
    u16 fw = W == 1 ? (u16)(fmask(r->x0) & lmask(r->x1)) : fmask(r->x0);
    u16 lw = W == 1 ? fw : lmask(r->x1);
    blit_idle();
    BLTCON0 = 0x07CA;                   /* B, C, D on; A (the masks) from BLTADAT, never shifted */
    BLTCON1 = (u16)((sh & 15) << 12 | (desc ? 2 : 0));
    BLTAFWM = desc ? lw : fw;
    BLTALWM = desc ? fw : lw;
    BLTADAT = 0xFFFF;
    BLTBMOD = (u16)(s->stride - (u32)W * 2);
    BLTCMOD = BLTDMOD = (u16)(d->stride - (u32)W * 2);
    const u8 *sp = s->p + (u32)sy * s->stride + (u32)plane * s->bpr + (u32)sw0 * 2;
    u8 *dp = d->p + (u32)r->y0 * d->stride + (u32)plane * d->bpr + (u32)dw0 * 2;
    if (desc) {
        sp += (u32)(h - 1) * s->stride + (u32)(W - 1) * 2;
        dp += (u32)(h - 1) * d->stride + (u32)(W - 1) * 2;
    }
    bptr(0x04C, sp);
    bptr(0x048, dp);
    bptr(0x054, dp);
    BLTSIZE = (u16)((h & 1023) << 6 | W);
    nblits++;
}
/* every plane at once, unshifted: an interleaved bitmap's lines of all planes follow each other bpr apart, so the
 * rectangle is one block of h x depth lines. On whole words B to D only (half the bus slots of A, B, C, D); with
 * edges inside a word the edges masked (A from BLTADAT through the first and last word masks) and C kept there */
static void blit_block(struct bm *s, int sx, int sy, struct bm *d, const struct rect *r, int desc);
static void blit_whole(struct bm *s, int sx, int sy, struct bm *d, const struct rect *r, int desc)
{
    int m0 = (r->x0 + 15) & ~15, m1 = r->x1 & ~15;
    if (((r->x0 & 15) || (r->x1 & 15)) && m1 - m0 >= 32) {   /* the whole words B to D, the two edge words masked */
        struct rect mid = { (s16)m0, r->y0, (s16)m1, r->y1 }, le = { r->x0, r->y0, (s16)m0, r->y1 },
                    re = { (s16)m1, r->y0, r->x1, r->y1 };
        blit_block(s, sx + (m0 - r->x0), sy, d, &mid, desc);
        if (le.x1 > le.x0) blit_block(s, sx, sy, d, &le, desc);
        if (re.x1 > re.x0) blit_block(s, sx + (m1 - r->x0), sy, d, &re, desc);
        return;
    }
    blit_block(s, sx, sy, d, r, desc);
}
static void blit_block(struct bm *s, int sx, int sy, struct bm *d, const struct rect *r, int desc)
{
    int w0 = r->x0 >> 4, w1 = (r->x1 - 1) >> 4, W = w1 - w0 + 1, n = (r->y1 - r->y0) * d->depth;
    int edges = (r->x0 & 15) || (r->x1 & 15);
    u16 fw = W == 1 ? (u16)(fmask(r->x0) & lmask(r->x1)) : fmask(r->x0), lw = W == 1 ? fw : lmask(r->x1);
    const u8 *sp = s->p + (u32)sy * s->stride + (u32)(sx >> 4) * 2;
    u8 *dp = d->p + (u32)r->y0 * d->stride + (u32)w0 * 2;
    if (desc) {
        sp += (u32)(n - 1) * s->bpr + (u32)(W - 1) * 2;
        dp += (u32)(n - 1) * d->bpr + (u32)(W - 1) * 2;
    }
    blit_idle();
    BLTCON0 = edges ? 0x07CA : 0x05CC;  /* D = AB + ~AC, or D = B */
    BLTCON1 = (u16)(desc ? 2 : 0);
    BLTAFWM = edges ? (desc ? lw : fw) : 0xFFFF;
    BLTALWM = edges ? (desc ? fw : lw) : 0xFFFF;
    BLTADAT = 0xFFFF;
    BLTBMOD = (u16)(s->bpr - (u32)W * 2);
    BLTCMOD = BLTDMOD = (u16)(d->bpr - (u32)W * 2);
    bptr(0x04C, sp);
    bptr(0x048, dp);
    bptr(0x054, dp);
    BLTSIZE = (u16)((n & 1023) << 6 | W);
    nblits++;
}
void bm_copy(struct bm *s, int sx, int sy, struct bm *d, const struct rect *dr)
{
    struct rect r = *dr;
    if (!s->p || !d->p)
        return;
    if (!clip_bm(d, &r))
        return;
    sx += r.x0 - dr->x0;
    sy += r.y0 - dr->y0;
    if (sx < 0 || sy < 0 || sx + (r.x1 - r.x0) > s->w || sy + (r.y1 - r.y0) > s->h)
        return;                         /* never read outside the source */
    npix += (u32)(r.x1 - r.x0) * (u32)(r.y1 - r.y0);
    if (d == &scr)
        ncopies++;
    int depth = s->depth < d->depth ? s->depth : d->depth;
    int same = s == d, after = same && (r.y0 > sy || (r.y0 == sy && r.x0 > sx));
    int sh = (r.x0 & 15) - (sx & 15);
    int bl = use_blit && r.y1 - r.y0 <= 1024 && ((!after && sh >= 0) || (after && sh == 0));
    if (bl && sh == 0 && depth == s->depth && depth == d->depth && s->bpr == d->bpr && s->stride == d->stride &&
        (r.y1 - r.y0) * depth <= 1024) {
        blit_whole(s, sx, sy, d, &r, after);    /* every plane in one blit */
        return;
    }
    for (int k = 0; k < depth; k++) {
        if (bl)
            blit_copy(s, sx, sy, d, &r, k, after);
        else {
            blit_idle();
            cpu_copy(s, sx, sy, d, &r, k);
        }
    }
    for (int k = depth; k < d->depth; k++)              /* a shallower image: the planes it lacks are 0 */
        if (use_blit) blit_rect(d, &r, k, MT_CLR); else cpu_rect(d, &r, k, 0);
}

/* an image with a mask (one plane of the image's size, 1 where drawn): pixel by pixel through the mask (icons and
 * gadgets, a few hundred pixels); without one a plain copy */
void bm_image(const struct bm *img, const u8 *mask, int sx, int sy, struct bm *d, const struct rect *dr)
{
    if (!mask) {
        bm_copy((struct bm *)img, sx, sy, d, dr);
        return;
    }
    struct rect r = *dr;
    if (!d->p || !clip_bm(d, &r))
        return;
    sx += r.x0 - dr->x0;
    sy += r.y0 - dr->y0;
    if (sx < 0 || sy < 0 || sx + (r.x1 - r.x0) > img->w || sy + (r.y1 - r.y0) > img->h)
        return;
    blit_idle();
    for (int y = r.y0; y < r.y1; y++) {
        int iy = sy + y - r.y0;
        const u8 *ml = mask + (u32)iy * img->bpr;
        for (int x = r.x0; x < r.x1; x++) {
            int ix = sx + x - r.x0;
            if (!(ml[ix >> 3] >> (7 - (ix & 7)) & 1))
                continue;
            u8 m = (u8)(0x80 >> (x & 7));
            for (int k = 0; k < d->depth; k++) {
                u8 *dl = d->p + (u32)y * d->stride + (u32)k * d->bpr + (x >> 3);
                int bit = k < img->depth ? img->p[(u32)iy * img->stride + (u32)k * img->bpr + (ix >> 3)] >>
                                           (7 - (ix & 7)) & 1 : 0;
                *dl = bit ? (u8)(*dl | m) : (u8)(*dl & ~m);
            }
        }
    }
}

/* one 8 x 8 glyph at (x, y) in pens fg on bg, clipped to clip (and the bitmap): a byte per plane per line, as the
 * console: (g & m1) ^ m2 with m1 where fg and bg differ, m2 where bg is set; bold smears one pixel, underline is the
 * last line, inverse swaps the pens. On an 8-pixel column a byte; else two, masked. */
void bm_text(struct bm *b, int x, int y, const u8 *glyph, int fg, int bg, int attr, const struct rect *clipr)
{
    struct rect c = *clipr, cell = { (s16)x, (s16)y, (s16)(x + 8), (s16)(y + 8) };
    if (!b->p || !clip_bm(b, &c) || !r_and(&c, &c, &cell))
        return;
    if (attr & TA_INV) { int t = fg; fg = bg; bg = t; }
    if (use_blit)
        blit_idle();
    nglyphs++;
    npix += (u32)(c.x1 - c.x0) * (u32)(c.y1 - c.y0);
    int sh = x & 7, bx = x >> 3;
    if (!sh && c.x0 == x && c.x1 == x + 8) {    /* a whole cell on an 8-pixel column (a terminal's): a byte a plane */
        u8 m1[4], m2[4];
        for (int k = 0; k < b->depth && k < 4; k++) {
            int f = fg >> k & 1, bk = bg >> k & 1;
            m1[k] = f != bk ? 0xFF : 0;
            m2[k] = bk ? 0xFF : 0;
        }
        u8 *l = b->p + (u32)c.y0 * b->stride + (u32)bx;
        for (int row = c.y0 - y; row < c.y1 - y; row++, l += b->stride) {
            u8 g = glyph[row];
            if (attr & TA_BOLD)
                g = (u8)(g | g >> 1);
            if ((attr & TA_UNDER) && row == 7)
                g = 0xFF;
            u8 *q = l;
            for (int k = 0; k < b->depth; k++, q += b->bpr)
                *q = (u8)((g & m1[k]) ^ m2[k]);
        }
        return;
    }
    u16 cm = 0;                         /* the bits of the cell inside the clip, over the two bytes: the same each row */
    for (int px = c.x0; px < c.x1; px++)
        cm |= (u16)(0x8000 >> (px - (bx << 3)));
    for (int row = c.y0 - y; row < c.y1 - y; row++) {
        u8 g = glyph[row];
        if (attr & TA_BOLD)
            g = (u8)(g | g >> 1);
        if ((attr & TA_UNDER) && row == 7)
            g = 0xFF;
        u16 gw = (u16)(g << (8 - sh));
        u8 *l = b->p + (u32)(y + row) * b->stride;
        for (int k = 0; k < b->depth; k++, l += b->bpr) {
            int f = fg >> k & 1, bk = bg >> k & 1;
            u16 v = (u16)(f == bk ? (f ? 0xFFFF : 0) : f ? gw : (u16)~gw);
            u8 hi = (u8)(cm >> 8), lo = (u8)cm;
            if (hi)
                l[bx] = (u8)((l[bx] & ~hi) | ((v >> 8) & hi));
            if (lo && bx + 1 < b->bpr)
                l[bx + 1] = (u8)((l[bx + 1] & ~lo) | (v & lo));
        }
    }
}
