/* SPDX-License-Identifier: GPL-2.0-only */
/* zintro: the Zenix intro (DESIGN D55 section 13, docs/d55-screens.md; docs/man/zintro.txt). A screen of its own
 * (DPY_SCREEN: lores, 4 planes, two pictures in its chip RAM), drawn by the CPU each frame into the picture not shown,
 * then flipped (SS_PLANES); a Copper list of a colour a line for the bars, the bands' palettes and the scroller's
 * shading, whose colours change in place each frame (SS_COLOURS); a ProTracker module of our own through sndd. All of
 * it made by tools/mkintro.py (zintro-data.h). Esc, q or a click: back to the desktop. */
#include "libz.h"
#include "../display.h"
#include "../../user/sndlib.h"
#include "../../user/modrep.h"
#include "zintro-data.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef short s16;
typedef int s32;

#define DEPTH 4
#define PL 40                           /* a plane's line, lores */
#define BPR (PL * DEPTH)                /* a line of the interleaved picture */
#define NSTAR 96
#define LEAD_MS 100                     /* the music is made this far ahead of its time */

static int H;                           /* the screen's lines: 256 PAL, 200 NTSC */
static int M0, M1, S0;                  /* the stars' band, the scroller's first line */
static u8 *mem;                         /* two pictures */
static u32 picsz;
static int verbose;

/* ---- the Copper list: a WAIT and colour 0 every line; the bands' palettes; the scroller's colour 1 a line ---- */
#define MAXCOP 640
static struct dpycop cop[MAXCOP];
static int ncop;
static u32 colv[MAXCOP];                /* the CO_COLOURs' values, in list order (SS_COLOURS sends them all) */
static int ncolv;
static int bgidx[150];                  /* each second line's colour 0 in colv */
static int scridx[32];                  /* the scroller's lines' colour 1 in colv */
static void C(int op, int a, u32 v)
{
    struct dpycop *c = &cop[ncop++];
    c->op = (u16)op;
    c->a = (u16)a;
    c->b = (u16)(v >> 16);
    c->c = (u16)v;
    if (op == CO_COLOUR)
        colv[ncolv++] = v;
}
static const u32 midpal[7] = { 0x384048, 0x7888A0, 0xE0F0FF, 0xB01828, 0xE02838, 0xE02838, 0xFF4050 };
static void cop_make(void)               /* colour 0 every second line (the bars in steps of two: half the colours to
                                         * send each frame); the stars' band's colours and the scroller's colour 1 */
{
    ncop = ncolv = 0;
    for (int y = 0; y < H; y++) {
        int k = y - (M0 - 8), sc = y >= S0 && y < S0 + 32;
        int extra = (k >= 0 && k < 8 && !(k & 1)) || sc;
        if ((y & 1) && !extra)
            continue;
        C(CO_WAIT, y, 0);
        if (!(y & 1)) {
            bgidx[y >> 1] = ncolv;
            C(CO_COLOUR, 0, 0);
        }
        if (k >= 0 && k < 8 && !(k & 1))        /* colours 1-7, two a line, on lines M0-8, -6, -4, -2 */
            for (int j = 1 + k; j <= 2 + k && j <= 7; j++)
                C(CO_COLOUR, j, midpal[j - 1]);
        if (sc) {
            scridx[y - S0] = ncolv;
            C(CO_COLOUR, 1, 0);
        }
    }
}

/* ---- colours by the frame ---- */
static u32 mix(u32 a, u32 b, int f)     /* a + (b - a) * f / 256, a gun at a time */
{
    u32 o = 0;
    for (int s = 0; s < 24; s += 8) {
        int x = (int)(a >> s & 255), y = (int)(b >> s & 255);
        o |= (u32)(x + (y - x) * f / 256) << s;
    }
    return o;
}
static u32 bgbase[300], barc[3][12], grad[256];
static void colours_init(void)          /* the tables, once: a frame then only picks from them */
{
    static const u32 bar[3] = { 0x30C8E0, 0x8C98B0, 0xC02030 };
    for (int y = 0; y < H; y++)
        bgbase[y] = mix(0x04070A, 0x0C1018, y * 256 / H);
    for (int i = 0; i < 3; i++)
        for (int d = 0; d < 12; d++)
            barc[i][d] = mix(0x070A10, bar[i], (12 - d) * 21);
    for (int p = 0; p < 256; p++) {     /* the scroller: toxic green through white and a dark green */
        int s = sine[p] / 64;
        grad[p] = s > 0 ? mix(0x3CFF5A, 0xF0FFF0, s) : mix(0x3CFF5A, 0x0E4A18, -s);
    }
}
static void colours(u32 t)
{
    for (int y = 0; y < H / 2; y++)
        colv[bgidx[y]] = bgbase[2 * y];
    for (int i = 0; i < 3; i++) {       /* each bar over the 22 lines around its centre; the later in front */
        int c = (H / 2 + (H / 2 - 16) * sine[(t * (u32)(3 + 2 * i) + (u32)i * 85) & 255] / 16384) >> 1;
        for (int d = -5; d <= 5; d++)
            if (c + d >= 0 && c + d < H / 2)
                colv[bgidx[c + d]] = barc[i][2 * (d < 0 ? -d : d)];
    }
    for (int r = 0; r < 32; r++)
        colv[scridx[r]] = grad[(r * 8 + (int)t * 4) & 255];
}

/* ---- the logo, unpacked into both pictures ---- */
static void unpack(const u8 *p, u32 n, u8 *o, u32 max)
{
    u32 i = 0, k = 0;
    while (i < n && k < max) {
        int c = (signed char)p[i++];
        if (c >= 0) {
            for (int j = 0; j <= c && i < n && k < max; j++) o[k++] = p[i++];
        } else if (c != -128) {
            u8 v = i < n ? p[i++] : 0;
            for (int j = 0; j < 1 - c && k < max; j++) o[k++] = v;
        }
    }
}

/* ---- the stars: three speeds, planes 0 and 1 of the middle band ---- */
static struct { int x, y, sp; } st[NSTAR];
static struct { u32 off[NSTAR]; u8 bit[NSTAR]; int n; } drawn[2];
static u32 seed = 0x2F6E2B1;
static u32 rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 16; }
static void stars(u8 *pic, int b, int steps)
{
    for (int i = 0; i < drawn[b].n; i++) {
        u8 m = (u8)~drawn[b].bit[i];
        pic[drawn[b].off[i]] &= m;
        pic[drawn[b].off[i] + PL] &= m;
    }
    drawn[b].n = 0;
    for (int i = 0; i < NSTAR; i++) {
        st[i].x -= st[i].sp * steps;
        if (st[i].x < 0) {
            st[i].x += 320 << 8;
            st[i].y = M0 + (int)(rnd() % (u32)(M1 - M0));
        }
        int x = st[i].x >> 8, pen = st[i].sp >> 8;
        u32 off = (u32)st[i].y * BPR + (u32)(x >> 3);
        u8 bit = (u8)(0x80 >> (x & 7));
        if (pen & 1) pic[off] |= bit;
        if (pen & 2) pic[off + PL] |= bit;
        drawn[b].off[drawn[b].n] = off;
        drawn[b].bit[drawn[b].n++] = bit;
    }
}

/* ---- the shape: two tetrahedra through each other (a stellated octahedron), plane 2 ---- */
static const s16 vtx[8][3] = { { 1, 1, 1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, 1 },
                               { -1, -1, -1 }, { -1, 1, 1 }, { 1, -1, 1 }, { 1, 1, -1 } };
static const u8 edge[12][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 }, { 2, 3 },
                                { 4, 5 }, { 4, 6 }, { 4, 7 }, { 5, 6 }, { 5, 7 }, { 6, 7 } };
static u8 *band;                         /* plane 2 of the stars' band, in fast RAM: drawn here, copied in whole */
static int bx0 = 320, by0 = 0, bx1 = -1, by1 = -1;   /* what was drawn in it last (x1 < x0: nothing) */
/* a line into the band: a pointer and a bit stepped along it (no multiply a pixel), clipped to the band first */
static void line(int x0, int y0, int x1, int y1)
{
    if (y0 > y1) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (y1 < M0 || y0 >= M1 || (x0 < 0 && x1 < 0) || (x0 > 319 && x1 > 319))
        return;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 - y0, sx = x0 < x1 ? 1 : -1, e = dx - dy;
    int n = dx > dy ? dx : dy;
    for (int i = 0; i <= n; i++) {      /* the few points outside the band or the screen are passed over */
        if ((unsigned)x0 < 320 && y0 >= M0 && y0 < M1) {
            u8 *p = band + (u32)(y0 - M0) * PL + (u32)(x0 >> 3);
            u8 m = (u8)(0x80 >> (x0 & 7));
            /* the rest of the line while it stays inside: no tests a pixel but the step */
            int stop = n - i;
            for (int j = 0; j <= stop; j++) {
                *p |= m;
                int e2 = 2 * e;
                if (e2 > -dy) {
                    e -= dy;
                    x0 += sx;
                    if (sx > 0) { m >>= 1; if (!m) { m = 0x80; p++; } }
                    else { m <<= 1; if (!m) { m = 1; p--; } }
                    if ((unsigned)x0 >= 320) return;
                }
                if (e2 < dx) {
                    e += dx;
                    y0++;
                    p += PL;
                    if (y0 >= M1) return;
                }
            }
            return;
        }
        int e2 = 2 * e;
        if (e2 > -dy) { e -= dy; x0 += sx; }
        if (e2 < dx) { e += dx; y0++; }
    }
}
static struct { int x0, y0, x1, y1; } was[2] = { { 1, 0, 0, 0 }, { 1, 0, 0, 0 } };  /* each picture's shape as
                                         * copied in (x1 < x0: none) */
static void band_clear(int x0, int y0, int x1, int y1)  /* in fast RAM, whole longs */
{
    for (int y = y0; y <= y1; y++) {
        u32 *l = (u32 *)(band + (u32)(y - M0) * PL);
        for (int k = x0 >> 5; k <= x1 >> 5; k++)
            l[k] = 0;
    }
}
static void shape(u8 *pic, int b, u32 t)
{
    if (bx1 >= bx0)                     /* last frame's lines out of the band */
        band_clear(bx0, by0, bx1, by1);
    int sa = sine[(t * 2) & 255], ca = sine[(t * 2 + 64) & 255], sb = sine[(t * 3) & 255], cb = sine[(t * 3 + 64) & 255];
    int cx = 160 + 80 * sine[(t / 2) & 255] / 16384, cy = (M0 + M1) / 2, R = (M1 - M0) / 4;
    int px[8], py[8];
    int x0 = 320, y0 = M1, x1 = -1, y1 = -1;
    for (int i = 0; i < 8; i++) {
        int x = vtx[i][0] * R, y = vtx[i][1] * R, z = vtx[i][2] * R;
        int x2 = (x * ca - z * sa) >> 14, z2 = (x * sa + z * ca) >> 14;     /* about y */
        int y2 = (y * cb - z2 * sb) >> 14, z3 = (y * sb + z2 * cb) >> 14;   /* about x */
        int d = 4 * R + z3;
        px[i] = cx + x2 * 4 * R / d;
        py[i] = cy + y2 * 4 * R / d * 5 / 6;    /* lores pixels are a little taller than wide */
        if (px[i] < x0) x0 = px[i];
        if (px[i] > x1) x1 = px[i];
        if (py[i] < y0) y0 = py[i];
        if (py[i] > y1) y1 = py[i];
    }
    for (int e = 0; e < 12; e++)
        line(px[edge[e][0]], py[edge[e][0]], px[edge[e][1]], py[edge[e][1]]);
    bx0 = x0 < 0 ? 0 : x0;
    bx1 = x1 > 319 ? 319 : x1;
    by0 = y0 < M0 ? M0 : y0;
    by1 = y1 >= M1 ? M1 - 1 : y1;
    if (by0 > by1 || bx0 > bx1)         /* (never: the shape is a quarter of the band; kept safe) */
        bx0 = 320, bx1 = -1, by0 = M0, by1 = M0 - 1;
    /* into the picture: the rows and longs of this shape and of the one this picture held (two frames ago) */
    int ux0 = bx0, uy0 = by0, ux1 = bx1, uy1 = by1;
    if (bx1 < bx0)
        ux0 = 320, ux1 = -1, uy0 = M1, uy1 = M0 - 1;
    if (was[b].x1 >= was[b].x0) {
        if (was[b].x0 < ux0) ux0 = was[b].x0;
        if (was[b].y0 < uy0) uy0 = was[b].y0;
        if (was[b].x1 > ux1) ux1 = was[b].x1;
        if (was[b].y1 > uy1) uy1 = was[b].y1;
    }
    for (int y = uy0; y <= uy1; y++) {
        u32 *d = (u32 *)(pic + (u32)y * BPR + 2 * PL);
        const u32 *s = (const u32 *)(band + (u32)(y - M0) * PL);
        for (int k = ux0 >> 5; k <= ux1 >> 5; k++)
            d[k] = s[k];
    }
    was[b].x0 = bx0; was[b].y0 = by0; was[b].x1 = bx1; was[b].y1 = by1;
}

/* ---- the scroller: one plane, shifted in fast RAM, copied in ---- */
static u32 sbuf[32][11];                /* 44 bytes a line: 40 shown, 4 where the new columns come in */
static const char *tp = scroll_text;
static int col;                         /* the column of the letter coming in (0-15, then a gap) */
static const u8 *glyph(char c)
{
    for (int i = 0; i < FONT_N; i++)
        if (font_chars[i] == c)
            return font16 + 32 * i;
    return font16;                      /* (' ' sorts first) */
}
static void scroller(u8 *pic, int steps)
{
    for (int s = 0; s < steps; s++) {
        for (int r = 0; r < 32; r++) {
            u32 *l = sbuf[r];
            for (int i = 0; i < 10; i++)
                l[i] = l[i] << 2 | l[i + 1] >> 30;
            l[10] <<= 2;
        }
        for (int k = 0; k < 2; k++) {   /* two new columns */
            const u8 *g = glyph(*tp);
            for (int r = 0; r < 32 && col < 16; r++) {
                u16 bits = (u16)(g[2 * (r / 2)] << 8 | g[2 * (r / 2) + 1]);
                if (bits >> (15 - col) & 1)
                    sbuf[r][10] |= 1u << (1 - k);
            }
            if (++col >= 20) {
                col = 0;
                if (!*++tp)
                    tp = scroll_text;
            }
        }
    }
    for (int r = 0; r < 32; r++)
        memcpy(pic + (u32)(S0 + r) * BPR, sbuf[r], PL);
}

/* ---- the music: a frames stream at sndd (as Lua's zenix module does) ---- */
static struct mod m;
static u8 *modd;
static int sh = -1, nosound;
static unsigned eclock = 709379;
static u32 tdue;
static int timing;
static int snd_up(void)
{
    if (sh >= 0 || nosound)
        return sh >= 0;
    struct sndinfo si;
    int r = snd_info(&si, 0);
    if (r >= 0) {
        eclock = si.eclock;
        r = snd_open(0, SNDF_FRAMES, 64, 0);
    }
    if (r >= 0) {
        sh = r;
        unsigned off = mod_bankoff(&m);
        r = snd_bank(sh, modd + off, (MOD_BYTES - off + 1) & ~1u);
        if (r < 0) { snd_close(sh); sh = -1; }
    }
    if (r < 0) {
        dprintf(2, "zintro: no music (%s)\n", snd_why(r));
        nosound = 1;
        return 0;
    }
    timing = 0;
    return 1;
}
static int ahead(u32 t, u32 now) { return (int)(t - now) > 0; }
static void frame(struct sndframe *f)
{
    memset(f, 0, sizeof *f);
    f->ticks = mod_ticklen(&m, eclock);
    mod_tick(&m);
    if (m.ended) {
        mod_goto(&m, 0);
        m.ended = 0;
    }
    for (int c = 0; c < 4; c++) {
        const struct modout *o = &m.out[c];
        struct sndch *x = &f->ch[c];
        x->flags = SNDC_VOL;
        x->volume = (u16)o->vol;
        if (o->period >= SND_FMINPER && o->period <= SND_FMAXPER) {
            x->flags |= SNDC_PER;
            x->period = (u16)o->period;
        }
        if (o->flags & (MOD_TRIG | MOD_LOOP)) {
            x->loop = o->looplen >= 2 ? o->loop : SND_SILENT;
            x->looplen = o->looplen >= 2 ? o->looplen : 2;
        }
        if (o->flags & MOD_TRIG) {
            x->flags |= SNDC_TRIG;
            x->start = o->len >= 2 ? o->start : SND_SILENT;
            x->len = o->len >= 2 ? o->len : 2;
        } else if (o->flags & MOD_LOOP)
            x->flags |= SNDC_LOOP;
    }
}
static void music(void)
{
    if (!snd_up())
        return;
    u32 now = sys_hrtime(), lead = now + eclock / 1000 * LEAD_MS;
    if (!timing || !ahead(tdue, now)) {
        tdue = now;
        timing = 1;
    }
    static struct sndframe f[4];
    while (!ahead(tdue, lead)) {
        int n = 0;
        do {
            frame(&f[n]);
            tdue += f[n].ticks;
            n++;
        } while (n < 4 && !ahead(tdue, lead));
        int r = snd_frames(sh, f, (unsigned)n);
        if (r < 0) {
            snd_close(sh);
            sh = -1;
            if (r != -ECONNRESET)
                nosound = 1;            /* (sndd gone: opened again next time, the music from where it is) */
            return;
        }
    }
}

static const char USAGE[] = "usage: zintro [-v] [-q]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-q")) nosound = 1;
        else if (argv[i][0] == '-') zbadopt(argv[i], USAGE);
        else zusage(USAGE);
    }
    int r = dpy_open("zintro");
    if (r < 0) {
        dprintf(2, "zintro: the desktop: %s\n", zstrerror(r) ? zstrerror(r) : "cannot connect");
        return 1;
    }
    H = dpy_h >= 400 ? dpy_h / 2 : dpy_h;
    M0 = 84;
    S0 = H - 44;
    M1 = S0 - 8;
    picsz = (u32)BPR * (u32)H;
    mem = sys_memalloc_chip(2 * picsz);
    modd = malloc(MOD_BYTES + 2);
    band = calloc(1, (u32)(M1 - M0) * PL);
    if (!mem || !modd || !band) {
        dprintf(2, "zintro: no memory (%u KB of chip RAM)\n", (2 * picsz) >> 10);
        dpy_close();
        return 1;
    }
    memset(mem, 0, 2 * picsz);
    static u8 lg[LOGO_BYTES];
    unpack(logo_pk, sizeof logo_pk, lg, sizeof lg);
    for (int p = 0; p < 2; p++)
        memcpy(mem + (u32)p * picsz + 8u * BPR, lg, LOGO_BYTES);
    unpack(mod_pk, sizeof mod_pk, modd, MOD_BYTES);
    modd[MOD_BYTES] = modd[MOD_BYTES + 1] = 0;
    char why[64];
    if (mod_load(&m, modd, MOD_BYTES, why) < 0) {
        dprintf(2, "zintro: the module: %s\n", why);
        nosound = 1;
    }
    for (int i = 0; i < NSTAR; i++) {
        st[i].x = (int)(rnd() % 320) << 8;
        st[i].y = M0 + (int)(rnd() % (u32)(M1 - M0));
        st[i].sp = (1 + i % 3) << 8;
    }
    cop_make();
    colours_init();
    struct dpyscr s;
    memset(&s, 0, sizeof s);
    s.w = 320;
    s.h = (u16)H;
    s.depth = DEPTH;
    s.flags = DPYSF_BACK;               /* the list first, then in front */
    s.planes = (u32)mem;
    s.bytes = 2 * picsz;
    s.pstride = PL;
    s.bpr = BPR;
    s.ncop = (u16)ncop;                 /* the room it needs (displayd keeps that much for it) */
    s.ncol = 16;
    s.palette = (u32)logo_pal;
    r = dpy_screen(&s);
    int bad = -1;
    if (r >= 0) {
        colours(0);
        r = dpy_scr_set(SS_COPPER, (u32)cop, (u32)ncop, &bad);
    }
    if (r >= 0)
        r = dpy_scr_set(SS_FRONT, 1, 0, 0);
    if (r < 0) {
        dprintf(2, "zintro: a screen of its own: %s%s\n",
                r == -ENODEV ? "it needs the chipset's display (the desktop is on a graphics card)" :
                zstrerror(r) ? zstrerror(r) : "refused", bad >= 0 ? " (the Copper list)" : "");
        dpy_close();
        return 1;
    }
    if (verbose) {
        volatile u32 k = 0;             /* the CPU's pace here, to read the times below against */
        u32 c0 = sys_hrtime();
        for (u32 i = 0; i < 20000; i++) k += i;
        (void)k;
        dprintf(1, "zintro: screen 320x%d, %d Copper instructions, %d colours a frame; 20,000 loops in %u us\n", H,
                ncop, ncolv, (sys_hrtime() - c0) * 1000 / 709);
    }
    int front = 1, quit = 0, b = 1;     /* picture 0 is shown first; we draw into 1 */
    u32 frames = 0, late = 0, t = 0, shown = (u32)dpy_vcount(), start = shown, before = shown - 1;
    u32 tdraw = 0, tcall = 0;           /* E-clock ticks drawing, and in the two requests (-v) */
    static u32 tpart[6];                /* stars, shape, scroller, colours, SS_COLOURS, SS_PLANES */
    while (!quit) {
        u32 now = (u32)dpy_vwait(shown);    /* until the last picture is up: the other one is ours */
        u32 steps = now - before;           /* frames since the last drawing: the motion keeps its speed */
        if (!steps || steps > 8)
            steps = 1;
        before = now;
        music();
        struct dpyev e;
        while (dpy_next(&e, 0) > 0) {
            if (e.type == EV_KEY && !(e.code & 0x80) && (e.code == 0x45 || e.ch[0] == 'q'))
                quit = 1;
            else if (e.type == EV_BUTTON && !(e.code & 0x80))
                quit = 1;
            else if (e.type == EV_FOCUS && e.win == 0)
                front = e.arg;
        }
        if (quit)
            break;
        t += steps;
        if (!front) {                   /* behind the desktop: the music goes on, nothing is drawn */
            shown = (u32)dpy_vwait(now + 1);
            continue;
        }
        u32 t0 = sys_hrtime();
        u8 *pic = mem + (u32)b * picsz;
        stars(pic, b, (int)steps);
        u32 ta = sys_hrtime();
        shape(pic, b, t);
        u32 tb = sys_hrtime();
        scroller(pic, (int)steps);
        u32 tc = sys_hrtime();
        colours(t);
        u32 t1 = sys_hrtime();
        dpy_scr_set(SS_COLOURS, 0 | (u32)ncolv << 16, (u32)colv, 0);
        u32 td = sys_hrtime();
        r = dpy_scr_set(SS_PLANES, (u32)b * picsz, 0, 0);
        tdraw += t1 - t0;
        tcall += sys_hrtime() - t1;
        tpart[0] += ta - t0; tpart[1] += tb - ta; tpart[2] += tc - tb; tpart[3] += t1 - tc;
        tpart[4] += td - t1; tpart[5] += sys_hrtime() - td;
        if (r < 0)
            break;
        if ((u32)r > shown + 1)
            late += (u32)r - shown - 1;     /* frames the last picture stayed up longer than one */
        shown = (u32)r;
        b ^= 1;
        frames++;
        music();
    }
    u32 end = (u32)dpy_vcount();
    dpy_scr_close();
    if (sh >= 0)
        snd_close(sh);
    dpy_close();
    if (verbose) {
        dprintf(1, "zintro: %u frames drawn in %u, %u late, music %s\n", frames, end - start, late,
                nosound ? "off" : "on");
        if (frames)                     /* E-clock ticks to microseconds (PAL's 709,379 Hz; NTSC's within 1%) */
            dprintf(1, "zintro: a frame: drawing %u us (stars %u, shape %u, scroller %u, colours %u), the server's "
                    "two requests %u us (SS_COLOURS %u, SS_PLANES %u)\n", tdraw / frames * 1000 / 709,
                    tpart[0] / frames * 1000 / 709, tpart[1] / frames * 1000 / 709, tpart[2] / frames * 1000 / 709,
                    tpart[3] / frames * 1000 / 709, tcall / frames * 1000 / 709, tpart[4] / frames * 1000 / 709,
                    tpart[5] / frames * 1000 / 709);
    }
    return 0;
}
