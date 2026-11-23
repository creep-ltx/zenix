/* SPDX-License-Identifier: GPL-2.0-only */
/* displayd's drawing (DESIGN D38 3.2, 6.3): bitmaps of interleaved planes in chip RAM, filled, inverted and copied by
 * the blitter while the console is shown (use_blit) and by the CPU while it is not (the kernel has the blitter then:
 * GEV_RELEASE), text by the CPU a byte per plane as the console draws it. Rectangles and regions for the clipping.
 * Every rectangle is clipped to its bitmap here, so no order can write outside the screen or a kept copy. */
#include "dpyd.h"
#include "fb.h"

#define FBR(r) (&(struct fbr){ (r).x0, (r).y0, (r).x1, (r).y1 })   /* displayd's rectangle as fb.c's */

#define CUST(o) (*(volatile u16 *)(0xDFF000 + (o)))
#define DMACONR CUST(0x002)
#define VPOSR   CUST(0x004)
#define VHPOSR  CUST(0x006)
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
u32 nshows, tshows, twaits, nlate;

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
    b->bpr = (u16)(depth == CHUNKY ? w : (w + 15) / 16 * 2);
    b->depth = (u16)depth;
    b->stride = depth == CHUNKY ? (u32)w : (u32)b->bpr * depth;
    u32 n = b->stride * (u32)h;
    b->p = !n ? 0 : depth == CHUNKY ? sys_memalloc(n) : sys_memalloc_chip(n);   /* D43: the CPU's, in fast RAM */
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

static void dirty_add(const struct rect *r);
/* minterms: A the mask (BLTADAT with the edge masks), C and D the bitmap */
#define MT_SET 0xFA
#define MT_CLR 0x0A
#define MT_INV 0x5A
static void blit_rect(struct bm *b, const struct rect *r, int plane, u8 mt)
{
    int w0 = r->x0 >> 4, w1 = (r->x1 - 1) >> 4, W = w1 - w0 + 1, h = r->y1 - r->y0;
    if (W > 64) {                       /* BLTSIZE's width is 6 bits (0: 64 words); wider (a lifted window's bitmap with
                                         * its zero padding, D38 "Dragging with the chips") in pieces of 64 words. A
                                         * wider width spilled into the height: the owner's A1200 of 2026-11-06 showed
                                         * the uncleared padding as garbage beside a dragged window */
        struct rect a = *r;
        a.x1 = (s16)((w0 + 64) << 4);
        blit_rect(b, &a, plane, mt);
        a.x0 = a.x1;
        a.x1 = r->x1;
        blit_rect(b, &a, plane, mt);
        return;
    }
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
        BLTSIZE = (u16)((k & 1023) << 6 | (W & 63));
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
    if (b == &scr)
        dirty_add(&r);
    npix += (u32)(r.x1 - r.x0) * (u32)(r.y1 - r.y0);
    if (CHUNKY_BM(b)) {
        fb_fill(&(struct fbmap){ b->p, b->stride }, FBR(r), pen);
        return;
    }
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
    if (b == &scr)
        dirty_add(&r);
    if (CHUNKY_BM(b)) {
        fb_xor(&(struct fbmap){ b->p, b->stride }, FBR(r), pen);
        return;
    }
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
    BLTSIZE = (u16)((h & 1023) << 6 | (W & 63));
    nblits++;
}
/* every plane at once, unshifted: an interleaved bitmap's lines of all planes follow each other bpr apart, so the
 * rectangle is one block of h x depth lines. On whole words B to D only (half the bus slots of A, B, C, D); with
 * edges inside a word the edges masked (A from BLTADAT through the first and last word masks) and C kept there */
static void blit_block(struct bm *s, int sx, int sy, struct bm *d, const struct rect *r, int desc);
static void blit_whole(struct bm *s, int sx, int sy, struct bm *d, const struct rect *r, int desc)
{
    int m0 = (r->x0 + 15) & ~15, m1 = r->x1 & ~15;
    if (((r->x0 & 15) || (r->x1 & 15)) && m1 - m0 >= 32 && (s != d || sx == r->x0)) {
        /* the whole words B to D, the two edge words masked (three blits: only where none reads what another wrote -
         * another bitmap, or a move straight up or down) */
        struct rect mid = { (s16)m0, r->y0, (s16)m1, r->y1 }, le = { r->x0, r->y0, (s16)m0, r->y1 },
                    re = { (s16)m1, r->y0, r->x1, r->y1 };
        blit_block(s, sx + (m0 - r->x0), sy, d, &mid, desc);
        if (le.x1 > le.x0) blit_block(s, sx, sy, d, &le, desc);
        if (re.x1 > re.x0) blit_block(s, sx + (m1 - r->x0), sy, d, &re, desc);
        return;
    }
    blit_block(s, sx, sy, d, r, desc);
}
static void blit_block1(struct bm *s, int sx, int sy, struct bm *d, const struct rect *r, int desc);
/* BLTSIZE has 10 bits of height: a block of more than 1,024 plane lines (an interlaced screen's 512 lines at 4 planes
 * are 2,048) in pieces of whole lines, from the bottom when it copies descending */
static void blit_block(struct bm *s, int sx, int sy, struct bm *d, const struct rect *r, int desc)
{
    int k = 1024 / d->depth, h = r->y1 - r->y0;
    if (h <= k) {
        blit_block1(s, sx, sy, d, r, desc);
        return;
    }
    for (int i = 0; i < h; i += k) {
        int o = desc ? (h - i - k > 0 ? h - i - k : 0) : i, n = desc ? h - i - o : (h - i < k ? h - i : k);
        struct rect p = { r->x0, (s16)(r->y0 + o), r->x1, (s16)(r->y0 + o + n) };
        blit_block1(s, sx, sy + o, d, &p, desc);
    }
}
static void blit_block1(struct bm *s, int sx, int sy, struct bm *d, const struct rect *r, int desc)
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
    BLTSIZE = (u16)((n & 1023) << 6 | (W & 63));
    nblits++;
}
/* ---- a drawn screen and the one on view (a whole window's drag; D38 6.3, the owner's A1200 of 2026-11-05) ----
 * Moving a window paints the damage back to front: the background, the frames, the insides from their copies, each
 * pixel once, but in several passes and not top to bottom. Painted on the screen on view, the raster shows whatever
 * is half done when it passes: the window's top at the new place and its bottom at the old (a torn frame), on any
 * CPU, as the blitter's speed sets the time. So while a whole window is dragged, the server draws into a second
 * screen in chip RAM (shadow_begin: a copy of the screen, its pointer swapped into scr, so every drawing call is
 * the same) and notes what it drew (dirty); once a pass of the main loop (one move at most, the mouse's moves since
 * merged) shadow_show copies what changed onto the screen on view: one blit of whole words, B to D, top to bottom,
 * started where the beam cannot overtake it nor come round to meet it before it ends (beam_rule), so every frame
 * shows the area all as it was or all as it is. The screen's first line is the kernel's DIW_V (the same make
 * variable); a frame is 312 lines on PAL, 262 on NTSC (by the console's height, as the kernel chose it). In
 * interlace screen line y is on frame line y / 2 (D38 "Screen modes"): a band's frame lines hold two screen lines
 * each, so the beam's rule is reckoned in frame lines with twice the words a line. */
#ifndef DIW_V
#define DIW_V 0x2C
#endif
#define NDIRTY 16                      /* 4 on the chipset (a wait for the beam each), 16 on a card (D43) */
int shadowing;                          /* scr is the drawn screen; shown is the one on view */
static struct bm shown;
static struct rect dirty[NDIRTY];
static int ndirty;
static u32 area(const struct rect *r) { return (u32)(r->x1 - r->x0) * (u32)(r->y1 - r->y0); }
static void unite(struct rect *a, const struct rect *b)
{
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}
/* r (clipped to the screen) joins the rectangle that grows least by it; an overlap always joins */
static void dirty_add(const struct rect *r)
{
    if (!shadowing && !rtg)
        return;
    int best = -1;
    u32 grow = ~0u;
    for (int i = 0; i < ndirty; i++) {
        struct rect u = dirty[i], k;
        unite(&u, r);
        u32 g = area(&u) - area(&dirty[i]);
        if (r_and(&k, &dirty[i], r))
            g = 0;
        if (g < grow) { grow = g; best = i; }
    }
    if (best >= 0 && (grow == 0 || ndirty == (rtg ? NDIRTY : 4)))
        unite(&dirty[best], r);
    else
        dirty[ndirty++] = *r;
}
static int frame_lines(void) { return frame_l; }
/* the display line the beam is on (0 the screen's first; negative above it, in the border and the vertical blank) */
int dpy_beam(void)
{
    u16 a, b, c;
    do {
        a = VPOSR;
        b = VHPOSR;
        c = VPOSR;
    } while ((a ^ c) & 1);              /* line 255 to 256 (or to 0) between the reads: again */
    return ((a & 1) << 8 | b >> 8) - DIW_V;
}
/* when a copy of lines y0..y1-1, W words wide, may start: a span of the beam's lines [*lo, *lo + *len). The copy goes
 * top to bottom at r microseconds a line, the beam at 64 (L lines a frame, H in the band). Two ways:
 *  - started after the beam left the band (lo = y1): the frame on show is untouched whatever the copy's speed; it
 *    must end each line before the beam is back at it: at most d lines late, d < L - H (at y0) and
 *    H r < (L - d) 64 (at y1);
 *  - started after the beam passed y0 (lo = y0), when the copy is surely slower than the beam: the beam stays ahead
 *    of it, and each line must end before the beam comes round: (y - y0) (r - 64) < (L - d) 64 up to y1.
 * The one that allows the later start is taken (the shorter wait); the second only if r is at least a third over 64.
 * r is W x depth x k, k the nanoseconds a word of one plane takes: the HRM's 564 (4 ticks a word, B to D) without
 * contention; with the display's 4 planes, the Copper and the CPU's reads of the blitter's busy bit it was about
 * 1,040 in FS-UAE's A1200 (emulator figures), so k starts at 1,000 and follows what the big shows take (show_k: up
 * at once, down slowly). The allowances are reckoned with r a quarter more, less 8 lines for the start and the
 * scheduler; at least 4 lines (the wait's loop must see the beam in it). */
static u32 show_k = 1000;
static void beam_rule(int y0, int y1, int words, int *lo, int *len)
{
    int L = frame_lines(), H = y1 - y0;
    s32 rl = (s32)((u32)words * scr.depth * show_k), bl = 64000, rh = rl + rl / 4;  /* ns a line */
    s32 fast = L - H * rh / bl, slow = rl * 3 >= bl * 4 ? L - H * (rh - bl) / bl : -1;
    if (fast > L - H)
        fast = L - H;
    if (slow > fast) {
        *lo = y0;
        *len = slow;
    } else {
        *lo = y1;
        *len = fast;
    }
    *len -= 8;
    if (*len < 4)
        *len = 4;
}
/* until the beam is in [lo, lo + len) of the frame's lines; 0 if not within 60 ms (3 frames: the beam stopped?) */
static int beam_wait(int lo, int len)
{
    int L = frame_lines();
    u32 t0 = sys_hrtime();
    for (int n = 0;; n++) {
        int d = ((dpy_beam() - lo) % L + L) % L;
        if (d < len)
            return 1;
        if (!(n & 15) && sys_hrtime() - t0 > 42000u)    /* (the clock's call is slower than a line now and then) */
            return 0;
    }
}
/* the server's own check of a show (DPY_STAT's late): the copy of y0..y1 started with the beam on b0 and took us
 * microseconds; taken as even, line y was written by (y - y0 + 1) / H of it. Every frame must show the band all as it
 * was or all as it is. With the beam inside the band at the start, the frame on show has shown the lines above it as
 * they were, so the lines below it must still be so when the beam gets there, and the next frame shows all new; with
 * the beam outside, the band's next showing is all old or all new, and if old the one after all new. */
static int show_torn(int b0, u32 us, int y0, int y1)
{
    int L = frame_lines(), H = y1 - y0, early = 0, later = 0, bad = 0, in = b0 >= y0 && b0 < y1;
    u32 fr = (u32)L * 64;
    for (int y = y0; y < y1; y++) {
        u32 w = (u32)(y - y0 + 1) * us / (u32)H;
        int d = ((y - b0) % L + L) % L;
        if (!d)
            d = L;                      /* the beam on the line itself: it shows the old one now */
        u32 p1 = (u32)d * 64;           /* the beam's next pass over line y */
        if (in && y > b0)
            bad |= w < p1 || w >= p1 + fr;      /* still old in this frame, new in the next */
        else if (in)
            bad |= w >= p1;                     /* new in the next frame */
        else if (w < p1)
            early++;
        else {
            later++;
            bad |= w >= p1 + fr;
        }
    }
    return bad || (early && later);
}
/* ---- D43: the screen on a graphics card (docs/d43-rtg.md 4): scr holds the pens, the card gets what changed ---- */
#ifndef NO_RTG
int rtg;
struct fbcard card;
static int ptr_x = -100, ptr_y;         /* the pointer's picture's top left on the card */
static int ptr_ox = -100, ptr_oy;       /* (D61) where the card shows it */
void (*fb_band_hook)(void);
static const u16 *ptr_a, *ptr_b;
static void dirty_clip(int x0, int y0, int x1, int y1)
{
    struct rect r = { (s16)x0, (s16)y0, (s16)x1, (s16)y1 };
    if (clip_bm(&scr, &r))
        dirty_add(&r);
}
void fb_pointer_image(const u16 *a, const u16 *b) { ptr_a = a; ptr_b = b; }
void fb_pointer(int x, int y)
{
    dirty_clip(ptr_x, ptr_y, ptr_x + FB_PTRW, ptr_y + FB_PTRW);    /* where it was: the pens again */
    ptr_x = x;
    ptr_y = y;
    dirty_clip(x, y, x + FB_PTRW, y + FB_PTRW);
}
void fb_all(void) { dirty_clip(0, 0, scr.w, scr.h); }
void fb_ptr_show(void)
{
    if (!rtg || !ptr_a)
        return;
    for (int i = 0; i < 2; i++) {       /* its old place (the pens again) and its new one; both stay damaged for the
                                         * pass's show, which puts what was drawn under them since */
        struct rect r = { (s16)(i ? ptr_x : ptr_ox), (s16)(i ? ptr_y : ptr_oy), 0, 0 };
        r.x1 = (s16)(r.x0 + FB_PTRW);
        r.y1 = (s16)(r.y0 + FB_PTRW);
        if (clip_bm(&scr, &r))
            fb_put(&card, &(struct fbmap){ scr.p, scr.stride }, FBR(r), ptr_x, ptr_y, ptr_a, ptr_b);
    }
    ptr_ox = ptr_x;
    ptr_oy = ptr_y;
}
void fb_show(void)
{
    if (!rtg || !ptr_a)
        return;
    /* D61: in bands of 32 lines, the mouse looked at between them (fb_band_hook: the pointer onto the card at once):
     * a big change takes a 68020 a second, and the pointer must not wait for it. The list is taken first, so what
     * the hook draws goes into the next show */
    struct rect d[NDIRTY];
    int n = ndirty;
    memcpy(d, dirty, sizeof d);
    ndirty = 0;
    ptr_ox = ptr_x;
    ptr_oy = ptr_y;
    for (int i = 0; i < n; i++)
        for (int y = d[i].y0; y < d[i].y1; y += 32) {
            struct rect b = d[i];
            b.y0 = (s16)y;
            if (b.y1 > y + 32)
                b.y1 = (s16)(y + 32);
            fb_put(&card, &(struct fbmap){ scr.p, scr.stride }, FBR(b), ptr_x, ptr_y, ptr_a, ptr_b);
            if (fb_band_hook)
                fb_band_hook();
        }
}
#endif

/* the drawn screen from now on: a copy of the one on view (1 if so; 0 no chip RAM, or the blitter is not ours) */
int shadow_begin(void)
{
    if (shadowing)
        return 1;
    if (rtg)                            /* D43: a card's screen is always drawn off the card and shown by fb_show */
        return shadowing = 1;
    if (!use_blit || !scr.p)
        return 0;
    u8 *p = sys_memalloc_chip(scr.stride * scr.h);
    if (!p)
        return 0;
    shown = scr;
    struct bm d = scr;
    d.p = p;
    struct rect all = { 0, 0, (s16)scr.w, (s16)scr.h };
    blit_block(&shown, 0, 0, &d, &all, 0);
    blit_idle();
    scr.p = p;
    shadowing = 1;
    ndirty = 0;
    return 1;
}
/* what was drawn since the last show onto the screen on view, each rectangle (whole words) timed to the beam */
void shadow_show(void)
{
    if (!shadowing)
        return;
    if (rtg) {
        fb_show();
        return;
    }
    if (ndirty > 1) {                   /* one show for all when their box is not much more (a wait for the beam each) */
        struct rect u = dirty[0];
        u32 sum = area(&dirty[0]);
        for (int i = 1; i < ndirty; i++) {
            unite(&u, &dirty[i]);
            sum += area(&dirty[i]);
        }
        if (area(&u) <= sum + sum / 2) {
            dirty[0] = u;
            ndirty = 1;
        }
    }
    for (int i = 0; i < ndirty; i++) {
        struct rect r = dirty[i];
        r.x0 = (s16)(r.x0 & ~15);
        r.x1 = (s16)((r.x1 + 15) & ~15);
        if (r.x1 > scr.w)
            r.x1 = (s16)scr.w;
        int words = (r.x1 - r.x0) >> 4, lo, len;
        if (words <= 0 || r.y1 <= r.y0)
            continue;
        int fy0 = r.y0 >> lace, fy1 = (r.y1 + lace) >> lace;     /* the band's frame lines */
        beam_rule(fy0, fy1, words << lace, &lo, &len);
        blit_idle();                    /* the drawing finished first */
        u32 t0 = sys_hrtime();
        int ok = beam_wait(lo, len);
        u32 t1 = sys_hrtime();
        int b0 = dpy_beam();
        blit_block(&scr, r.x0, r.y0, &shown, &r, 0);
        blit_idle();
        u32 t2 = sys_hrtime();
        twaits += t1 - t0;
        tshows += t2 - t1;
        nshows++;
        u32 us = (t2 - t1) * 141u / 100u, k = us * 1000u / ((u32)(r.y1 - r.y0) * (u32)words * scr.depth);
        if ((r.y1 - r.y0) * words >= 512) {     /* a big one (a small one is mostly the call's own time) */
            show_k = k > show_k ? k : (7 * show_k + k) / 8;    /* what the copies take on this machine */
            if (show_k < 564)
                show_k = 564;
        }
        if (!ok || show_torn(b0, us, fy0, fy1)) {
            if (nlate++ < 4 || !(nlate & 255))      /* (the serial line: the first few, then one in 256) */
                dprintf(2, "displayd: a show late for the beam: lines %d..%d, %d words, the beam at %d (wanted %d "
                        "+%d), %u us%s\n", r.y0, r.y1, words, b0, lo, len, us, ok ? "" : ", the beam not seen");
        }
    }
    ndirty = 0;
}
/* back to drawing on the screen on view: what is left shown, the drawn screen freed */
void shadow_end(void)
{
    if (!shadowing)
        return;
    shadow_show();
    if (rtg) {
        shadowing = 0;
        return;
    }
    blit_idle();
    u8 *p = scr.p;
    scr.p = shown.p;
    shadowing = 0;
    sys_memfree(p);
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
    if (d == &scr) {
        ncopies++;
        dirty_add(&r);
    }
    if (CHUNKY_BM(d)) {        /* D43: from a chunky bitmap, or from a client's planar image */
        if (CHUNKY_BM(s))
            fb_copy(&(struct fbmap){ s->p, s->stride }, sx, sy, &(struct fbmap){ d->p, d->stride },
                    FBR(r));
        else
            fb_planar(s->p, s->stride, s->bpr, s->depth, 0, sx, sy, &(struct fbmap){ d->p, d->stride },
                      FBR(r));
        return;
    }
    if (CHUNKY_BM(s))
        return;                         /* (never asked: nothing planar is made from a card's screen) */
    int depth = s->depth < d->depth ? s->depth : d->depth;
    int same = s == d, after = same && (r.y0 > sy || (r.y0 == sy && r.x0 > sx));
    int sh = (r.x0 & 15) - (sx & 15);
    int bl = use_blit && r.y1 - r.y0 <= 1024 && ((!after && sh >= 0) || (after && sh == 0));
    if (bl && sh == 0 && depth == s->depth && depth == d->depth && s->bpr == d->bpr && s->stride == d->stride) {
        blit_whole(s, sx, sy, d, &r, after);    /* every plane in one blit (more than 1,024 plane lines in pieces) */
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
    if (d == &scr)
        dirty_add(&r);
    if (CHUNKY_BM(d)) {
        fb_planar(img->p, img->stride, img->bpr, img->depth, mask, sx, sy, &(struct fbmap){ d->p, d->stride },
                  FBR(r));
        return;
    }
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
    if (b == &scr)
        dirty_add(&c);
    if (use_blit)
        blit_idle();
    nglyphs++;
    npix += (u32)(c.x1 - c.x0) * (u32)(c.y1 - c.y0);
    if (CHUNKY_BM(b)) {
        fb_glyph(&(struct fbmap){ b->p, b->stride }, x, y, glyph, fg, bg,
                 (attr & TA_BOLD ? FBA_BOLD : 0) | (attr & TA_UNDER ? FBA_UNDER : 0), FBR(c));
        return;
    }
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

/* a text run of the desktop's font (font.c builds it in fast RAM): rows of mask bytes, mask[0] at the bitmap's byte
 * column bx0, line y0, ms bytes a row; pixels x0..x1-1 written in pens fg (mask 1) on bg (mask 0). One write a byte
 * a plane, as bm_text's whole cell: only the two edge bytes of a row are read (chip RAM is slow to read). Clipped to
 * the bitmap here. */
void bm_mask(struct bm *b, int bx0, int y0, int rows, const u8 *mask, int ms, int x0, int x1, int fg, int bg)
{
    struct rect c = { (s16)x0, (s16)y0, (s16)x1, (s16)(y0 + rows) };
    if (!b->p || !clip_bm(b, &c) || c.x0 < bx0 * 8 || c.x1 > (bx0 + ms) * 8)
        return;
    if (b == &scr)
        dirty_add(&c);
    if (use_blit)
        blit_idle();
    npix += (u32)(c.x1 - c.x0) * (u32)(c.y1 - c.y0);
    if (CHUNKY_BM(b)) {
        fb_mask(&(struct fbmap){ b->p, b->stride }, bx0, y0, mask, ms, FBR(c), fg, bg);
        return;
    }
    u8 m1[8], m2[8];
    for (int k = 0; k < b->depth && k < 8; k++) {
        int f = fg >> k & 1, bk = bg >> k & 1;
        m1[k] = f != bk ? 0xFF : 0;
        m2[k] = bk ? 0xFF : 0;
    }
    int q0 = c.x0 >> 3, q1 = (c.x1 - 1) >> 3;
    u8 lm = (u8)(0xFF >> (c.x0 & 7)), rm = (u8)(0xFF << (7 - ((c.x1 - 1) & 7)));
    if (q0 == q1)
        lm &= rm;
    for (int y = c.y0; y < c.y1; y++) {
        const u8 *g = mask + (u32)(y - y0) * (u32)ms;   /* g[q - bx0]: the mask's byte for the column byte q */
        u8 *l = b->p + (u32)y * b->stride;
        for (int k = 0; k < b->depth; k++, l += b->bpr) {
            u8 a = m1[k], z = m2[k];
            int q = q0;
            l[q] = (u8)((l[q] & ~lm) | (((g[q - bx0] & a) ^ z) & lm));
            if (q0 == q1)
                continue;
            for (q++; q < q1; q++)
                l[q] = (u8)((g[q - bx0] & a) ^ z);
            l[q] = (u8)((l[q] & ~rm) | (((g[q - bx0] & a) ^ z) & rm));
        }
    }
}
