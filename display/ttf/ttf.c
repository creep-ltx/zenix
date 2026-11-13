/* SPDX-License-Identifier: GPL-2.0-only */
/* The TrueType reader and rasteriser (ttf.h; DESIGN D51, docs/d51-truetype.md). Written from the TrueType format as
 * Apple and Microsoft publish it (the tables head, maxp, hhea, hmtx, loca, glyf, cmap, post); no code from another
 * rasteriser.
 *
 * A glyph's outline (straight lines and quadratic curves, in font units) is scaled to 1/64 pixels, each curve cut
 * into at most 16 lines (the curve is never more than 1/16 pixel from them), and each line's crossings with the
 * pixel rows' centres collected. A row's crossings sorted by x give spans where the outline's winding is not 0; a
 * pixel is set when its centre is in a span or on its edge. A span with no pixel centre in it (a stem thinner than a
 * pixel) sets the pixel under its middle (dropout control) unless it is narrower than 3/8 pixel (the tip of a stroke
 * cut at a slant); the columns are scanned the same way (a centre on a horizontal edge, a thin horizontal stroke).
 * Integer arithmetic only: 32-bit multiplies and divides, nothing wider (a 68060 has no 64-bit mulu.l). */
#include "ttf.h"

const char ttf_wide[] = "a glyph wider than 64 pixels or advancing past 255 at this size";

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define MAXPTS  1024            /* points of one simple glyph */
#define MAXEDGE 2048            /* lines of one glyph after the curves are cut */
#define MAXCROSS 4096           /* crossings of one glyph with the rows (and then with the columns) */
#define TMPW    128             /* the picture before it is cut to its ink: 128 columns */
#define MAXDEPTH 6              /* composite glyphs inside composite glyphs */
#define MAXPARTS 256            /* the parts of one glyph, however nested (a hostile font's N^depth: the review) */
#define MAXWORK 16384           /* the points of one glyph, all its parts together */
#define DROPMIN 24              /* a span narrower than 3/8 pixel (a stroke's tip) keeps no pixel: measured against
                                   FreeType's unhinted mono at 8, 11 and 16 lines, tools/ttf-host-test.sh */

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

/* ---- the tables ---- */
static int table(const u8 *d, u32 n, u32 num, const char *tag, u32 *off, u32 *len)
{
    for (u32 i = 0; i < num; i++) {
        const u8 *r = d + 12 + 16 * i;
        if (r[0] == (u8)tag[0] && r[1] == (u8)tag[1] && r[2] == (u8)tag[2] && r[3] == (u8)tag[3]) {
            u32 o = be32(r + 8), l = be32(r + 12);
            if (o > n || l > n - o)
                return -1;
            *off = o;
            *len = l;
            return 1;
        }
    }
    return 0;
}

/* a cmap subtable's format and length, checked to lie in the cmap table -> its length, or 0 */
static u32 sub_ok(const struct ttf *t, u32 o, u32 cmapn, u16 *fmt)
{
    const u8 *d = t->d;
    if (o > cmapn || cmapn - o < 8)
        return 0;
    u32 s = t->cmap + o;
    u16 f = be16(d + s);
    u32 l = f == 12 ? be32(d + s + 4) : be16(d + s + 2);
    if (l > cmapn - o)
        return 0;
    if (f == 4) {
        if (l < 16)
            return 0;
        u32 seg2 = be16(d + s + 6);
        if (!seg2 || (seg2 & 1) || 16 + 4 * seg2 > l)
            return 0;
    } else if (f == 12) {
        if (l < 16 || be32(d + s + 12) > (l - 16) / 12)
            return 0;
    } else if (f == 0) {
        if (l < 6 + 256)
            return 0;
    } else if (f == 6) {
        if (l < 10 || 10 + 2 * (u32)be16(d + s + 8) > l)
            return 0;
    } else
        return 0;
    *fmt = f;
    return l;
}

const char *ttf_open(struct ttf *t, const u8 *d, u32 n)
{
    u8 *z = (u8 *)t;
    for (u32 i = 0; i < sizeof *t; i++)
        z[i] = 0;
    if (n > TTF_MAXFILE)
        return "larger than 4 MB";
    if (n < 12)
        return "too short for a TrueType font";
    u32 v = be32(d);
    if (v == 0x4F54544Fu)
        return "an OpenType font with PostScript (CFF) outlines: only TrueType outlines are read";
    if (v == 0x74746366u)
        return "a font collection (.ttc): not read";
    if (v != 0x00010000u && v != 0x74727565u)
        return "not a TrueType font";
    u32 num = be16(d + 4), o, l;
    if (!num || 12 + 16 * num > n)
        return "cut short in its table directory";
    t->d = d;
    t->n = n;
#define NEED(tag, minlen) do { int r_ = table(d, n, num, tag, &o, &l); \
        if (r_ < 0) return "a table (" tag ") outside the file"; \
        if (!r_) return "no " tag " table"; \
        if (l < (minlen)) return "its " tag " table is too short"; } while (0)
    NEED("head", 54);
    t->upem = be16(d + o + 18);
    t->loc32 = (short)be16(d + o + 50);
    if (t->upem < 16 || t->upem > 16384)
        return "units per em outside 16-16384";
    if (t->loc32 != 0 && t->loc32 != 1)
        return "an unknown loca format";
    NEED("maxp", 6);
    t->nglyphs = be16(d + o + 4);
    if (!t->nglyphs)
        return "no glyphs";
    NEED("hhea", 36);
    t->asc = (short)be16(d + o + 4);
    t->desc = (short)be16(d + o + 6);
    t->gap = (short)be16(d + o + 8);
    t->nhm = be16(d + o + 34);
    if (!t->nhm || t->nhm > t->nglyphs)
        return "its horizontal metrics count is 0 or past the glyphs";
    NEED("hmtx", 4u * t->nhm);
    t->hmtx = o;
    t->hmtxn = l;
    NEED("loca", (t->nglyphs + 1u) * (t->loc32 ? 4u : 2u));
    t->loca = o;
    int r = table(d, n, num, "glyf", &o, &l);
    if (r < 0)
        return "a table (glyf) outside the file";
    if (!r)
        return "no glyf table (not TrueType outlines)";
    t->glyf = o;
    t->glyfn = l;
    r = table(d, n, num, "post", &o, &l);
    if (r > 0 && l >= 16)
        t->fixed = be32(d + o + 12) != 0;
    NEED("cmap", 4);
#undef NEED
    t->cmap = o;
    t->cmapn = l;
    u32 ns = be16(d + o + 2), best = 0, bestrank = 0;
    if (4 + 8 * ns > l)
        return "cut short in its cmap table";
    for (u32 i = 0; i < ns; i++) {
        const u8 *e = d + o + 4 + 8 * i;
        u16 pid = be16(e), eid = be16(e + 2), fmt = 0;
        u32 so = be32(e + 4);
        if (!sub_ok(t, so, l, &fmt))
            continue;
        /* Unicode with all planes first, then the Basic Multilingual Plane, then Macintosh Roman (8 bits) */
        u32 rank = (pid == 3 && eid == 10) || (pid == 0 && (eid == 4 || eid == 6)) ? 5
                 : (pid == 3 && eid == 1) || pid == 0 ? 4 : (pid == 1 && eid == 0) ? 2 : 0;
        if (rank && fmt == 12 && rank < 4)
            rank = 0;
        if (rank > bestrank) {
            bestrank = rank;
            best = so;
            t->cmapfmt = fmt;
        }
    }
    if (!bestrank)
        return "no Unicode or Macintosh character map it can read";
    t->cmap = o + best;
    t->cmapn = sub_ok(t, best, l, &t->cmapfmt);
    return 0;
}

unsigned ttf_index(const struct ttf *t, unsigned cp)
{
    const u8 *s = t->d + t->cmap;
    u32 g = 0;
    switch (t->cmapfmt) {
    case 0:
        g = cp < 256 ? s[6 + cp] : 0;
        break;
    case 6: {
        u32 first = be16(s + 6), cnt = be16(s + 8);
        g = cp >= first && cp - first < cnt ? be16(s + 10 + 2 * (cp - first)) : 0;
        break;
    }
    case 4: {
        if (cp > 0xFFFF)
            return 0;
        u32 seg = be16(s + 6) / 2;
        const u8 *end = s + 14, *start = end + 2 * seg + 2, *delta = start + 2 * seg, *ro = delta + 2 * seg;
        for (u32 i = 0; i < seg; i++) {
            if (cp > be16(end + 2 * i))
                continue;
            u32 st = be16(start + 2 * i);
            if (cp < st)
                return 0;
            u32 r = be16(ro + 2 * i);
            if (!r)
                g = (cp + be16(delta + 2 * i)) & 0xFFFF;
            else {
                u32 at = (u32)(ro + 2 * i - s) + r + 2 * (cp - st);
                if (at + 2 > t->cmapn)
                    return 0;
                g = be16(s + at);
                if (g)
                    g = (g + be16(delta + 2 * i)) & 0xFFFF;
            }
            break;
        }
        break;
    }
    case 12: {
        u32 lo = 0, hi = be32(s + 12);          /* the groups are in order (the format's rule): halved */
        while (lo < hi) {
            u32 mid = lo + (hi - lo) / 2;
            const u8 *e = s + 16 + 12 * mid;
            u32 a = be32(e), b = be32(e + 4);
            if (cp < a)
                hi = mid;
            else if (cp > b)
                lo = mid + 1;
            else {
                g = be32(e + 8) + (cp - a);
                break;
            }
        }
        break;
    }
    }
    return g < t->nglyphs ? g : 0;
}

const char *ttf_hmetrics(const struct ttf *t, unsigned gi, int *adv, int *lsb)
{
    *adv = *lsb = 0;
    if (gi >= t->nglyphs)
        return "no such glyph";
    const u8 *h = t->d + t->hmtx;
    u32 last = t->nhm - 1u;
    *adv = be16(h + 4 * (gi < t->nhm ? gi : last));
    if (gi < t->nhm)
        *lsb = (short)be16(h + 4 * gi + 2);
    else {
        u32 at = 4 * t->nhm + 2 * (gi - t->nhm);
        if (at + 2 > t->hmtxn)
            return "cut short in its hmtx table";
        *lsb = (short)be16(h + at);
    }
    return 0;
}

/* glyph gi's bytes in glyf -> its offset (from the file's start) and length; length 0: an empty glyph */
static const char *locate(const struct ttf *t, u32 gi, u32 *off, u32 *len)
{
    if (gi >= t->nglyphs)
        return "a glyph index past the glyphs";
    const u8 *l = t->d + t->loca;
    u32 a = t->loc32 ? be32(l + 4 * gi) : 2u * be16(l + 2 * gi);
    u32 b = t->loc32 ? be32(l + 4 * gi + 4) : 2u * be16(l + 2 * gi + 2);
    if (a > b || b > t->glyfn)
        return "a glyph outside its glyf table";
    *off = t->glyf + a;
    *len = b - a;
    return 0;
}

const char *ttf_box(const struct ttf *t, unsigned gi, int *x0, int *y0, int *x1, int *y1)
{
    u32 o, l;
    const char *why = locate(t, gi, &o, &l);
    *x0 = *y0 = *x1 = *y1 = 0;
    if (why || !l)
        return why;
    if (l < 10)
        return "a glyph cut short in its header";
    const u8 *g = t->d + o;
    *x0 = (short)be16(g + 2);
    *y0 = (short)be16(g + 4);
    *x1 = (short)be16(g + 6);
    *y1 = (short)be16(g + 8);
    return 0;
}

/* ---- the outline, scaled, as lines ---- */
struct edge { short x0, y0, x1, y1; };          /* 1/64 pixels, y up, the baseline at 0 */
struct work {
    struct edge e[MAXEDGE];
    int px[MAXPTS], py[MAXPTS];
    u8 pf[MAXPTS];
    int cnt[TMPW + 2];
    int cross[MAXCROSS];
    u8 pic[TTF_MAXH * (TMPW / 8)];
};
struct xf { int a, b, c, d, dx, dy; };          /* x' = (a x + c y) / 2^14 + dx, y' = (b x + d y) / 2^14 + dy */

struct ctx {
    const struct ttf *t;
    struct work *w;
    int ne, em64, emx64, upem;
    int parts, pts;                             /* the work done for this glyph (MAXPARTS, MAXWORK) */
    const char *why;
};

/* 256 pixels each way: a crossing's product of two differences stays within 32 bits */
static short clip16(int v) { return (short)(v > 16000 ? 16000 : v < -16000 ? -16000 : v); }
static void line(struct ctx *c, int x0, int y0, int x1, int y1)
{
    if (y0 == y1 && x0 == x1)
        return;
    if (c->ne >= MAXEDGE) {
        c->why = "a glyph with too many lines (past 2048)";
        return;
    }
    struct edge *e = &c->w->e[c->ne++];
    e->x0 = clip16(x0);
    e->y0 = clip16(y0);
    e->x1 = clip16(x1);
    e->y1 = clip16(y1);
}
static int iabs(int v) { return v < 0 ? -v : v; }
static void quad(struct ctx *c, int x0, int y0, int cx, int cy, int x1, int y1)
{
    int ddx = x0 - 2 * cx + x1, ddy = y0 - 2 * cy + y1;
    int dev = (iabs(ddx) > iabs(ddy) ? iabs(ddx) : iabs(ddy)) / 4;     /* the curve's distance from its chord */
    int n = 1;
    while (n < 16 && n * n * 4 < dev)           /* each piece within 4/64 of a pixel */
        n++;
    int px = x0, py = y0, nn = n * n;
    for (int i = 1; i <= n; i++) {
        int x = i == n ? x1 : x0 + (2 * i * n * (cx - x0) + i * i * ddx) / nn;
        int y = i == n ? y1 : y0 + (2 * i * n * (cy - y0) + i * i * ddy) / nn;
        line(c, px, py, x, y);
        px = x;
        py = y;
    }
}

static int lim(int v, int m) { return v > m ? m : v < -m ? -m : v; }
/* font units -> 1/64 pixels, rounded (|v| <= 131072, em <= 15360: within 32 bits) */
static int sc(const struct ctx *c, int v, int em)
{
    int p = v * em;
    return p >= 0 ? (p + c->upem / 2) / c->upem : -((-p + c->upem / 2) / c->upem);
}

static void contour(struct ctx *c, int s, int e)
{
    struct work *w = c->w;
    int n = e - s + 1;
    if (n < 2)
        return;
#define ON(i) (w->pf[s + (i)] & 1)
    int sx, sy, first, last;                    /* the start (on the curve) and the points after it */
    if (ON(0)) {
        sx = w->px[s];
        sy = w->py[s];
        first = 1;
        last = n - 1;
    } else if (ON(n - 1)) {
        sx = w->px[e];
        sy = w->py[e];
        first = 0;
        last = n - 2;
    } else {
        sx = (w->px[e] + w->px[s]) / 2;
        sy = (w->py[e] + w->py[s]) / 2;
        first = 0;
        last = n - 1;
    }
    int x = sx, y = sy, cx = 0, cy = 0, ctrl = 0;
    for (int i = first; i <= last + 1 && !c->why; i++) {
        int on = i > last || ON(i);
        int qx = i > last ? sx : w->px[s + i], qy = i > last ? sy : w->py[s + i];
        if (on) {
            if (ctrl)
                quad(c, x, y, cx, cy, qx, qy);
            else
                line(c, x, y, qx, qy);
            x = qx;
            y = qy;
            ctrl = 0;
        } else {
            if (ctrl) {
                int mx = (cx + qx) / 2, my = (cy + qy) / 2;
                quad(c, x, y, cx, cy, mx, my);
                x = mx;
                y = my;
            }
            cx = qx;
            cy = qy;
            ctrl = 1;
        }
    }
#undef ON
}

static void glyph(struct ctx *c, u32 gi, const struct xf *m, int depth)
{
    u32 o, l;
    const char *why = locate(c->t, gi, &o, &l);
    if (why) {
        c->why = why;
        return;
    }
    if (++c->parts > MAXPARTS) {                /* counted before an empty part: those cost time too */
        c->why = "a composite glyph with too many parts (past 256)";
        return;
    }
    if (!l)
        return;
    const u8 *p = c->t->d + o, *end = p + l;
#define NEED(k) do { if ((u32)(end - p) < (u32)(k)) { c->why = "a glyph cut short"; return; } } while (0)
    NEED(10);
    int nc = (short)be16(p);
    p += 10;
    if (nc >= 0) {
        struct work *w = c->w;
        NEED(2 * nc + 2);
        const u8 *ep = p;
        p += 2 * nc;
        u32 np = nc ? be16(ep + 2 * (nc - 1)) + 1u : 0;
        for (int i = 1; i < nc; i++)
            if (be16(ep + 2 * i) <= be16(ep + 2 * i - 2)) {
                c->why = "its contours' ends are not in order";
                return;
            }
        if (np > MAXPTS) {
            c->why = "a glyph with more than 1024 points";
            return;
        }
        if ((c->pts += (int)np) > MAXWORK) {
            c->why = "a composite glyph with too many points (past 16384)";
            return;
        }
        u32 il = be16(p);
        p += 2;
        NEED(il);
        p += il;                                /* the instructions: no hinting */
        for (u32 i = 0; i < np;) {
            NEED(1);
            u8 f = *p++, rep = 0;
            if (f & 8) {
                NEED(1);
                rep = *p++;
            }
            if (i + 1u + rep > np) {
                c->why = "its flags repeat past its points";
                return;
            }
            for (u32 k = 0; k <= rep; k++)
                w->pf[i++] = f;
        }
        for (int axis = 0; axis < 2; axis++) {
            int *v = axis ? w->py : w->px, acc = 0;
            for (u32 i = 0; i < np; i++) {
                u8 f = w->pf[i] >> (axis ? 1 : 0);
                if (f & 2) {
                    NEED(1);
                    acc += (f & 16) ? *p : -(int)*p;
                    p++;
                } else if (!(f & 16)) {
                    NEED(2);
                    acc += (short)be16(p);
                    p += 2;
                }
                if (acc > 65535 || acc < -65535) {
                    c->why = "a point far outside the em";
                    return;
                }
                v[i] = acc;
            }
        }
        for (u32 i = 0; i < np; i++) {          /* transformed and scaled */
            int x = w->px[i], y = w->py[i];
            int tx = ((m->a * x) >> 14) + ((m->c * y) >> 14) + m->dx;
            int ty = ((m->b * x) >> 14) + ((m->d * y) >> 14) + m->dy;
            w->px[i] = lim(sc(c, lim(tx, 131072), c->emx64), 1 << 19);    /* 8192 pixels: quad()'s products */
            w->py[i] = lim(sc(c, lim(ty, 131072), c->em64), 1 << 19);     /* stay within 2^30 */
        }
        for (int i = 0, s = 0; i < nc && !c->why; i++) {
            int e = be16(ep + 2 * i);
            contour(c, s, e);
            s = e + 1;
        }
        return;
    }
    if (nc != -1) {
        c->why = "an unknown glyph kind";
        return;
    }
    if (depth >= MAXDEPTH) {
        c->why = "composite glyphs nested too deep";
        return;
    }
    u16 f;
    do {                                        /* a composite: its parts, each moved and scaled */
        NEED(4);
        f = be16(p);
        u32 sub = be16(p + 2);
        p += 4;
        int a1, a2;
        if (f & 1) {
            NEED(4);
            a1 = (short)be16(p);
            a2 = (short)be16(p + 2);
            p += 4;
        } else {
            NEED(2);
            a1 = (signed char)p[0];
            a2 = (signed char)p[1];
            p += 2;
        }
        if (!(f & 2))
            a1 = a2 = 0;                        /* matched points (anchors): placed unmoved */
        struct xf k = { 16384, 0, 0, 16384, a1, a2 };
        if (f & 8) {
            NEED(2);
            k.a = k.d = (short)be16(p);
            p += 2;
        } else if (f & 0x40) {
            NEED(4);
            k.a = (short)be16(p);
            k.d = (short)be16(p + 2);
            p += 4;
        } else if (f & 0x80) {
            NEED(8);
            k.a = (short)be16(p);
            k.b = (short)be16(p + 2);
            k.c = (short)be16(p + 4);
            k.d = (short)be16(p + 6);
            p += 8;
        }
        struct xf n;                            /* the parent's after the part's, kept in range (2.14: within 2) */
        n.a = lim(((m->a * k.a) >> 14) + ((m->c * k.b) >> 14), 32767);
        n.b = lim(((m->b * k.a) >> 14) + ((m->d * k.b) >> 14), 32767);
        n.c = lim(((m->a * k.c) >> 14) + ((m->c * k.d) >> 14), 32767);
        n.d = lim(((m->b * k.c) >> 14) + ((m->d * k.d) >> 14), 32767);
        n.dx = lim(((m->a * k.dx) >> 14) + ((m->c * k.dy) >> 14) + m->dx, 65535);
        n.dy = lim(((m->b * k.dx) >> 14) + ((m->d * k.dy) >> 14) + m->dy, 65535);
        glyph(c, sub, &n, depth + 1);
        if (c->why)
            return;
    } while (f & 0x20);
#undef NEED
}

/* ---- crossings ---- */
static int ceil64(int v) { return -((-v) >> 6); }      /* ceil(v / 64), v of either sign */
static void sort(int *a, int n)
{
    for (int i = 1; i < n; i++) {
        int v = a[i], j = i;
        for (; j > 0 && a[j - 1] > v; j--)
            a[j] = a[j - 1];
        a[j] = v;
    }
}

/* the crossings of every line with the lines y = k*64+32 (rows) or x = k*64+32 (columns), k in [k0, k0 + nk), into
 * w->cross bucketed by k (w->cnt[k - k0] the start) -> 0, or the reason */
static const char *crossings(struct ctx *c, int cols, int k0, int nk)
{
    struct work *w = c->w;
    for (int i = 0; i <= nk; i++)
        w->cnt[i] = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < c->ne; i++) {
            const struct edge *e = &w->e[i];
            int a0 = cols ? e->x0 : e->y0, a1 = cols ? e->x1 : e->y1;     /* along the scan */
            int b0 = cols ? e->y0 : e->x0, b1 = cols ? e->y1 : e->x1;
            if (a0 == a1)
                continue;
            int lo = a0 < a1 ? a0 : a1, hi = a0 < a1 ? a1 : a0;
            int ka = ceil64(lo - 32), kb = ceil64(hi - 32) - 1;           /* centres in [lo, hi) */
            if (ka < k0)
                ka = k0;
            if (kb > k0 + nk - 1)
                kb = k0 + nk - 1;
            for (int k = ka; k <= kb; k++) {
                if (!pass) {
                    w->cnt[k - k0 + 1]++;
                    continue;
                }
                int at = k * 64 + 32;
                int b = b0 + (at - a0) * (b1 - b0) / (a1 - a0);
                w->cross[w->cnt[k - k0]++] = b * 2 + (a1 > a0);
            }
        }
        if (!pass) {
            for (int i = 0; i < nk; i++)
                w->cnt[i + 1] += w->cnt[i];
            if (w->cnt[nk] > MAXCROSS)
                return "a glyph too complex at this size (past 4096 crossings)";
        }                                       /* cnt[i]: bucket i's start, its end after pass 1 */
    }
    /* cnt[i] is now the end of bucket i (the start of i + 1); bucket i starts at cnt[i - 1] (0 for the first) */
    return 0;
}

const char *ttf_glyph(const struct ttf *t, unsigned gi, int em64, int emx64, int h, int base, void *work,
                      struct ttf_pic *p)
{
    struct work *w = work;
    struct ctx c = { t, w, 0, em64, emx64, t->upem, 0, 0, 0 };
    p->x0 = p->w = p->bpr = 0;
    p->h = h;
    p->adv = 0;
    if (h < 1 || h > TTF_MAXH || base < 0 || base >= h || em64 < 64 || em64 > 64 * 128 || emx64 < 64 || emx64 > 64 * 240)
        return "a size outside what is drawn";
    int adv, lsb;
    const char *why = ttf_hmetrics(t, gi, &adv, &lsb);
    if (why)
        return why;
    p->adv = (adv * emx64 / t->upem + 32) >> 6;
    if (p->adv > 255)
        return ttf_wide;
    struct xf id = { 16384, 0, 0, 16384, 0, 0 };
    glyph(&c, gi, &id, 0);
    if (c.why)
        return c.why;
    if (!c.ne)
        return 0;                               /* a space */
    int xmin = 30000, xmax = -30000;
    for (int i = 0; i < c.ne; i++) {
        const struct edge *e = &w->e[i];
        int a = e->x0 < e->x1 ? e->x0 : e->x1, b = e->x0 < e->x1 ? e->x1 : e->x0;
        if (a < xmin)
            xmin = a;
        if (b > xmax)
            xmax = b;
    }
    int cx0 = xmin >> 6, ncol = (xmax >> 6) - cx0 + 1;   /* the columns the outline touches */
    if (ncol > TMPW)
        return ttf_wide;
    for (u32 i = 0; i < sizeof w->pic; i++)
        w->pic[i] = 0;
#define SET(row, col) (w->pic[(row) * (TMPW / 8) + ((col) >> 3)] |= (u8)(0x80 >> ((col) & 7)))
    /* rows: k = base - row, the row's centre at y = k*64+32 */
    int k0 = base - h + 1;
    if ((why = crossings(&c, 0, k0, h)))
        return why;
    for (int i = 0; i < h; i++) {
        int s = i ? w->cnt[i - 1] : 0, n = w->cnt[i] - s, *a = w->cross + s, wind = 0, xa = 0;
        int row = base - (k0 + i);
        sort(a, n);
        for (int j = 0; j < n; j++) {
            int was = wind, x = a[j] >> 1;
            wind += (a[j] & 1) ? 1 : -1;
            if (!was && wind)
                xa = x;
            else if (was && !wind && x > xa) {
                int f = ceil64(xa - 32), l = (x - 32) >> 6;    /* centres in [xa, x]: on the edge is in */
                if (f > l && x - xa >= DROPMIN)          /* dropout: the pixel under the span's middle */
                    f = l = ((xa + x) / 2) >> 6;
                for (int col = f; col <= l; col++)
                    if (col - cx0 >= 0 && col - cx0 < ncol)
                        SET(row, col - cx0);
            }
        }
    }
    /* columns: the same inclusive fill (a centre on a horizontal edge is in: the rows' scan, half-open in y, misses
     * it), and the dropouts of a horizontal stroke thinner than a pixel between two row centres */
    if ((why = crossings(&c, 1, cx0, ncol)))
        return why;
    for (int i = 0; i < ncol; i++) {
        int s = i ? w->cnt[i - 1] : 0, n = w->cnt[i] - s, *a = w->cross + s, wind = 0, ya = 0;
        sort(a, n);
        for (int j = 0; j < n; j++) {
            int was = wind, y = a[j] >> 1;
            wind += (a[j] & 1) ? 1 : -1;
            if (!was && wind)
                ya = y;
            else if (was && !wind && y >= ya) {
                int f = ceil64(ya - 32), l = (y - 32) >> 6;     /* centres in [ya, y] */
                if (f > l && y - ya >= DROPMIN)
                    f = l = ((ya + y) / 2) >> 6;
                for (int k = f; k <= l; k++)
                    if (base - k >= 0 && base - k < h)
                        SET(base - k, i);
            }
        }
    }
#undef SET
    /* cut to the ink */
    int cmin = TMPW, cmax = -1;
    for (int r = 0; r < h; r++)
        for (int col = 0; col < ncol; col++)
            if (w->pic[r * (TMPW / 8) + (col >> 3)] & (0x80 >> (col & 7))) {
                if (col < cmin)
                    cmin = col;
                if (col > cmax)
                    cmax = col;
            }
    if (cmax < 0)
        return 0;
    if (cmax - cmin + 1 > TTF_MAXW)
        return ttf_wide;
    p->x0 = cx0 + cmin;
    p->w = cmax - cmin + 1;
    p->bpr = (p->w + 7) / 8;
    for (int r = 0; r < h; r++)
        for (int b = 0; b < p->bpr; b++)
            p->bits[r * p->bpr + b] = 0;
    for (int r = 0; r < h; r++)
        for (int col = cmin; col <= cmax; col++)
            if (w->pic[r * (TMPW / 8) + (col >> 3)] & (0x80 >> (col & 7)))
                p->bits[r * p->bpr + ((col - cmin) >> 3)] |= (u8)(0x80 >> ((col - cmin) & 7));
    return 0;
}
