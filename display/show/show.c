/* SPDX-License-Identifier: GPL-2.0-only */
/* show: Zenix Desktop's picture viewer (DESIGN D50, docs/d50-viewers.md, docs/man/show.txt). A client of the display
 * protocol (D38 4) like files and term: one window showing an IFF ILBM or a GIF (display/show/pic.c decodes it) in
 * the screen's pens (an ordered pattern as strong as a colour is far from its pen, Floyd-Steinberg, or the nearest
 * pen; pic.c's renderer), shrunk to fit the window or at its real size on the
 * glass (panned); next and previous picture in the folder; keys configurable in ~/.config/show/keys (display/keys.h,
 * files' format and parser). The picture is drawn as planar images in strips (DPY_IMAGE: a client's images are at
 * most 64 KB of chip RAM, so a strip is made, drawn and freed), from a canvas kept in fast RAM for exposes. */
#include "libz.h"
#include "../display.h"
#include "../keys.h"
#include "pic.h"

typedef unsigned char u8;
typedef unsigned short u16;

#define MAXFILE (8u << 20)              /* a file read whole */
#define MAXCANVAS (1u << 20)            /* the real size's canvas in fast RAM (larger: shrunk to fit it) */
#define STRIP 16384                     /* bytes of one image strip (chip RAM, freed after each) */

static int verbose, win = -1, ww, wh, quit, depth = 4, fitmode = 1, dither = PIC_ORDERED, helping;
static unsigned short pal12[16];
static struct pens PS;
static int bgpen, fgpen;

/* ================================================================ keys and actions */
enum { A_NONE, A_NEXT, A_PREV, A_FIRST, A_LAST, A_ZOOM, A_DITHER, A_LEFT, A_RIGHT, A_UP, A_DOWN, A_HELP, A_QUIT,
       A_SCREEN, NACT };
static const char *const actname[NACT] = { "none", "next", "prev", "first", "last", "zoom", "dither", "left",
                                           "right", "up", "down", "help", "quit", "screen" };
static const char defaults[] =
    "; show - the picture viewer of Zenix Desktop: the keys (man show). Edit freely; show reads this at start.\n"
    ";\n"
    "; One key a line: KEYS ACTION [ARGUMENT]. KEYS are typed characters, or\n"
    "; names in <>: <Up> <Down> <Left> <Right>, <S-Up> (Shift) ..., <C-d> (Ctrl),\n"
    "; <Return> <Esc> <Tab> <Space> <Del> <BS> <Help> <F1> .. <F10>.\n"
    "; Two keys in a row make a prefix. ACTION none takes a key away.\n"
    "; The format is files' and Ahiru.keys's.\n"
    ";\n"
    "; left and right move a picture wider than the window; when its width\n"
    "; fits they go to the previous and next picture. screen shows the\n"
    "; picture on a screen of its own, in its own mode and colours (DESIGN D55).\n"
    "\n[main]\n"
    "n next\n<Space> next\np prev\n<BS> prev\ngg first\nG last\n"
    "z zoom\nd dither\ns screen\n"
    "h left\n<Left> left\nl right\n<Right> right\nk up\n<Up> up\nj down\n<Down> down\n"
    "? help\n<Help> help\nq quit\n<Esc> quit\n";
#define MAXB 64
static struct kbind kb[MAXB];
static struct keyset K = { actname, NACT, kb, 0, MAXB, "" };

/* ================================================================ the folder */
static char dir[ZPATH_MAX];
static char **names;
static int nnames, cur = -1;
static int ci_cmp(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    for (; *x && tolower((u8)*x) == tolower((u8)*y); x++, y++)
        ;
    int d = tolower((u8)*x) - tolower((u8)*y);
    return d ? d : strcmp(*(char *const *)a, *(char *const *)b);
}
/* the folder's names for next and previous -> 0, or -1 when it could not be read whole (what was read is used) */
static int folder(const char *base)
{
    int err = 0;
    nnames = dir_list_some(dir, &names, &err);
    if (nnames < 0) { nnames = 0; names = 0; err = 1; }
    int k = 0;
    for (int i = 0; i < nnames; i++)            /* hidden names left out, as ls and files do */
        if (names[i][0] == '.') free(names[i]);
        else names[k++] = names[i];
    nnames = k;
    if (names) names[k] = 0;
    qsort(names, (size_t)nnames, sizeof *names, ci_cmp);
    cur = -1;
    for (int i = 0; i < nnames; i++)
        if (!strcmp(names[i], base)) cur = i;
    return err ? -1 : 0;
}

/* ================================================================ the picture */
static struct pic P;
static int have;                        /* P decoded */
static char why[64], name[64], title[DPY_TITLE];
static struct canvas CV;
static int cw, ch, ox, oy;              /* the canvas, and where its top left is in the window (negative: panned) */
static int shrunk;                      /* the real size was over MAXCANVAS: shown smaller (said in the title) */

static void *zalloc(unsigned n) { return malloc(n); }
static void zfree(void *p) { free(p); }

static void natural(int *nw, int *nh)
{
    int lace = dpy_h >= 400;
    long w = P.w, h = P.h;
    if (P.flags & PIC_SHRES) w = (w + 1) / 2;
    else if (!(P.flags & (PIC_HIRES | PIC_SQUARE))) w *= 2;
    int tall = (P.flags & (PIC_LACE | PIC_SQUARE)) != 0;      /* lines half as tall as a hires line */
    if (lace && !tall) h *= 2;
    else if (!lace && tall) h = (h + 1) / 2;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *nw = (int)w;
    *nh = (int)h;
}
/* the size to draw at: fit (shrunk into the window, never enlarged) or real (the canvas's memory at most MAXCANVAS) */
static void layout(void)
{
    int nw, nh;
    natural(&nw, &nh);
    int mw = fitmode ? ww : 4096, mh = fitmode ? wh : 4096;
    while (!fitmode && (u32)((mw + 15) / 16 * 2) * (u32)depth * (u32)mh > MAXCANVAS)
        mw = mw * 3 / 4, mh = mh * 3 / 4;
    shrunk = !fitmode && (nw > mw || nh > mh);
    cw = nw;
    ch = nh;
    if (cw > mw || ch > mh) {
        if ((long)nw * mh > (long)nh * mw) { cw = mw; ch = (int)((long)nh * mw / nw); }
        else { ch = mh; cw = (int)((long)nw * mh / nh); }
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
}
static void place(void)
{
    if (cw <= ww) ox = (ww - cw) / 2;
    else {
        if (ox > 0) ox = 0;
        if (ox < ww - cw) ox = ww - cw;     /* any column: draw_rect takes the strips from the word below */
    }
    if (ch <= wh) oy = (wh - ch) / 2;
    else {
        if (oy > 0) oy = 0;
        if (oy < wh - ch) oy = wh - ch;
    }
}
static int render(void)
{
    free(CV.p);
    CV.p = 0;
    if (!have || win < 0)
        return -1;
    layout();
    CV.w = cw; CV.h = ch; CV.depth = depth;
    CV.bpr = (unsigned)((cw + 15) / 16 * 2);
    CV.p = malloc(CV.bpr * (u32)depth * (u32)ch);
    u8 *rb = malloc((u32)P.w * 4 + 24);
    short *work = malloc((u32)(cw + 2) * 6);
    unsigned short *xs = malloc((u32)cw * 2);
    int ok = CV.p && rb && work && xs;
    if (ok) {
        u32 t0 = sys_ticks();
        pic_render(&P, &CV, &PS, dither, rb, work, xs);
        if (verbose)
            dprintf(1, "show: rendered %d x %d in %u ms (%s)\n", cw, ch, (sys_ticks() - t0) * 10,
                    dither == PIC_FS ? "Floyd-Steinberg" : dither == PIC_ORDERED ? "ordered" : "nearest");
    }
    free(rb); free(work); free(xs);
    if (!ok) {
        free(CV.p);
        CV.p = 0;
        snprintf(why, sizeof why, "out of memory for %d x %d", cw, ch);
        return -1;
    }
    return 0;
}

/* ================================================================ drawing */
static void fill(int x0, int y0, int x1, int y1, const int *r)
{
    if (x0 < r[0]) x0 = r[0];
    if (y0 < r[1]) y0 = r[1];
    if (x1 > r[2]) x1 = r[2];
    if (y1 > r[3]) y1 = r[3];
    if (x0 < x1 && y0 < y1)
        dpy_fill(win, x0, y0, x1 - x0, y1 - y0, bgpen);
}
static void lines(const char *const *t, int n)
{
    for (int i = 0; i < n && 8 + i * 10 + 8 <= wh; i++) {
        char b[128];
        int k = snprintf(b, sizeof b, "%s", t[i]);
        if (k > (int)sizeof b - 1) k = (int)sizeof b - 1;
        int cols = (ww - 16) / 8;
        if (k > cols) k = cols > 0 ? cols : 0;
        dpy_text(win, 8, 8 + i * 10, fgpen, bgpen, 0, b, k);
    }
}
static void draw_help(void)
{
    dpy_fill(win, 0, 0, ww, wh, bgpen);
    static char buf[40][48];
    const char *t[40];
    int n = 0;
    t[n++] = "show: the keys (~/.config/show/keys)";
    for (int i = 0; i < K.n && n < 40; i++) {
        if (!K.b[i].act) continue;
        char a[12], b[12] = "";
        keys_name(K.b[i].k1, a);
        if (K.b[i].k2) keys_name(K.b[i].k2, b);
        snprintf(buf[n], sizeof buf[n], "%s%s  %s %s", a, b, actname[K.b[i].act], K.b[i].arg);
        t[n] = buf[n];
        n++;
    }
    lines(t, n);
    dpy_flush();
}
static void draw_failed(void)
{
    dpy_fill(win, 0, 0, ww, wh, bgpen);
    const char *t[2] = { name, why };
    lines(t, 2);
    dpy_flush();
}
/* the window's part r (x0, y0, x1, y1): the canvas where it is, the background around it */
static void draw_rect(const int *r)
{
    if (helping) { draw_help(); return; }
    if (!have || !CV.p) { draw_failed(); return; }      /* not read, or not rendered (why says) */
    fill(0, 0, ww, oy, r);                                  /* above, below, left, right of the canvas */
    fill(0, oy + ch, ww, wh, r);
    fill(0, oy, ox, oy + ch, r);
    fill(ox + cw, oy, ww, oy + ch, r);
    int x0 = ox > r[0] ? ox : r[0], x1 = ox + cw < r[2] ? ox + cw : r[2];
    int y0 = oy > r[1] ? oy : r[1], y1 = oy + ch < r[3] ? oy + ch : r[3];
    if (x0 >= x1 || y0 >= y1) {
        dpy_flush();
        return;
    }
    /* strips of at most 640 x 256 (DPY_IMAGE's largest; a card's window can be wider) and STRIP bytes, columns from a
     * 16-pixel word of the canvas */
    u8 *st = malloc(STRIP);
    int fail = 0;
    for (int cx0 = x0 - ox; !fail && cx0 < x1 - ox; ) {
        int a = cx0 & ~15, cx1 = x1 - ox < a + 640 ? x1 - ox : a + 640, iw = cx1 - a;
        u32 wpr = (u32)(iw + 15) / 16 * 2;
        int rows = (int)(STRIP / (wpr * (u32)depth));
        if (rows > 256) rows = 256;
        if (rows < 1) rows = 1;
        for (int y = y0; st && y < y1; ) {
            int n = y1 - y < rows ? y1 - y : rows;
            for (int p = 0; p < depth; p++)
                for (int k = 0; k < n; k++)
                    memcpy(st + ((u32)p * (u32)n + (u32)k) * wpr,
                           CV.p + (u32)(y - oy + k) * CV.bpr * (u32)depth + (u32)p * CV.bpr + (u32)(a >> 3), wpr);
            int id = dpy_image_new(iw, n, depth, 0, st);
            if (id < 0) {
                if (id != -EINVAL && rows > 1) { rows /= 2; continue; }     /* chip RAM short: smaller strips */
                fail = id;
                break;
            }
            dpy_image(win, id, cx0 - a, 0, cx1 - cx0, n, ox + cx0, y);
            dpy_flush();
            dpy_image_free(id);
            y += n;
        }
        cx0 = cx1;
    }
    free(st);
    if (!st || fail) {                  /* said in the title, never silently half a picture */
        snprintf(title, sizeof title, "%s: %s", name, !st || fail == -ENOSPC || fail == -ENOMEM ?
                 "no memory to draw it" : "the server refused it");
        dpy_title(win, title);
        if (verbose) dprintf(1, "show: say: %s\n", title);
    }
    dpy_flush();
}
static void draw_all(void)
{
    int r[4] = { 0, 0, ww, wh };
    draw_rect(r);
    if (verbose && have)
        dprintf(1, "show: shown %s %d x %d at %d %d in %d x %d (%s)\n", name, cw, ch, ox, oy, ww, wh,
                fitmode ? "fit" : "real size");
}
static void set_title(const char *pre)
{
    char m[48] = "";
    if (have) pic_mode(&P, m, sizeof m);
    if (have && shrunk)
        snprintf(title, sizeof title, "%s(smaller) %s %dx%d %s", pre, name, P.w, P.h, m);
    else if (have)
        snprintf(title, sizeof title, "%s%s %dx%d %s", pre, name, P.w, P.h, m);
    else
        snprintf(title, sizeof title, "%s%s", pre, name);
    dpy_title(win, title);
}

/* ================================================================ loading */
/* the file at path: 1 shown (or its failure shown), 0 not a picture (skipped by next/previous) */
static int load(const char *path, const char *nm, int skip)
{
    u8 head[12];
    struct ustat st;
    int plain = sys_stat(path, &st) == 0 && (st.mode & S_IFMT) == S_IFREG;
    if (skip && !plain)
        return 0;                       /* a folder, a device, a FIFO: never opened (it could wait for ever) */
    int fd = plain ? sys_open(path, O_RDONLY) : -1;
    int n = fd < 0 ? -1 : sys_read(fd, head, sizeof head);
    if (skip && (n < 0 || !pic_kind(head, (unsigned)n))) {
        if (fd >= 0) sys_close(fd);
        return 0;
    }
    if (have) { free(P.data); have = 0; }
    free(CV.p);
    CV.p = 0;
    snprintf(name, sizeof name, "%s", nm);
    why[0] = 0;
    u8 *f = 0;
    if (!plain)
        snprintf(why, sizeof why, "not a plain file");
    else if (fd < 0)
        snprintf(why, sizeof why, "%s", zwhy("cannot open"));
    else if (sys_fstat(fd, &st) < 0 || (st.mode & S_IFMT) != S_IFREG)
        snprintf(why, sizeof why, "not a plain file");
    else if (st.size > MAXFILE)
        snprintf(why, sizeof why, "too big (8 MB at most)");
    else if (!(f = malloc(st.size ? st.size : 1)))
        snprintf(why, sizeof why, "out of memory for the file");
    else {
        u32 got = 0;
        sys_lseek(fd, 0, 0);
        while (got < st.size) {
            int r = sys_read(fd, f + got, st.size - got);
            if (r <= 0) break;
            got += (u32)r;
        }
        const char *w = got < st.size ? "cannot read it all" : pic_decode(f, got, &P, zalloc, zfree);
        if (w)
            snprintf(why, sizeof why, "%s", w);
        else
            have = 1;
    }
    free(f);
    if (fd >= 0) sys_close(fd);
    if (verbose) {
        char m[48] = "";
        if (have) pic_mode(&P, m, sizeof m);
        if (have)
            dprintf(1, "show: picture %s %d %d %s\n", nm, P.w, P.h, m);
        else
            dprintf(1, "show: picture %s: %s\n", nm, why);
    }
    ox = oy = 0;
    render();                           /* (not before the window exists: main renders then) */
    return 1;
}
static int load_at(int i, int skip)
{
    char *p = path_join(dir, names[i]);
    int r = p ? load(p, names[i], skip) : 0;
    free(p);
    return r;
}
static void step(int d)
{
    for (int i = cur + d; i >= 0 && i < nnames; i += d)
        if (load_at(i, 1)) {
            cur = i;
            place();
            set_title("");
            draw_all();
            return;
        }
    set_title(d > 0 ? "(the last) " : "(the first) ");
    if (verbose) dprintf(1, "show: say: %s\n", d > 0 ? "the last picture" : "the first picture");
}

/* ================================================================ a screen of its own (DESIGN D55, docs/d55-screens.md
 * section 11): the picture's mode and palette, its planes copied into chip RAM (a row padded to 8 bytes) */
static int onscr, spl, sbpr, srows, sw, sh, sx, sy, lace_on;
static u8 *smem;
static void scr_off(void)
{
    if (!onscr)
        return;
    if (dpy_scr_close() >= 0)           /* (displayd lets go of the planes before it answers) */
        sys_memfree(smem);              /* (else they stay ours until we end: freeing them now would be refused) */
    smem = 0;
    onscr = 0;
    if (verbose) dprintf(1, "show: screen closed\n");
}
static void scr_say(const char *w)
{
    char t[80];
    snprintf(t, sizeof t, "(no screen of its own: %s) %s", w, name);
    dpy_title(win, t);
    if (verbose) dprintf(1, "show: say: no screen of its own: %s\n", w);
}
static void scr_on(void)
{
    scr_off();
    if (!have) return;
    if (P.flags & (PIC_RGB | PIC_SHRES)) { scr_say(P.flags & PIC_RGB ? "24-bit" : "super-hires"); return; }
    int chunky = (P.flags & PIC_CHUNKY) != 0, hires = (P.flags & PIC_HIRES) != 0, lace = (P.flags & PIC_LACE) != 0;
    int d = P.depth;
    if (chunky)
        for (d = 1; d < 8 && (1 << d) < P.ncol; d++)
            ;
    int base = dpy_h >= 400 ? dpy_h / 2 : dpy_h, maxh = base << lace;
    sw = hires ? 640 : 320;
    sh = P.h < maxh ? P.h : maxh;
    if (lace) sh &= ~1;
    if (sh < 2) sh = 2;
    int pb = (P.w + 7) / 8;
    spl = ((pb > sw / 8 ? pb : sw / 8) + 7) & ~7;
    srows = P.h > sh ? P.h : sh;
    sbpr = spl * d;
    if (sbpr > 65528) { scr_say("too wide for a screen"); return; }
    u32 bytes = (u32)sbpr * (u32)srows;
    if (!(smem = sys_memalloc_chip(bytes))) { scr_say("no chip RAM"); return; }
    memset(smem, 0, bytes);
    int perm[8];                        /* HAM8: the file's control planes (its top two) are the hardware's 1 and 2 */
    for (int k = 0; k < 8; k++)
        perm[k] = (P.flags & PIC_HAM8) ? (k < 2 ? 6 + k : k - 2) : k;
    for (int y = 0; y < P.h; y++) {
        u8 *o = smem + (u32)y * (u32)sbpr;
        if (!chunky) {
            u32 n = P.rowbytes < (unsigned)spl ? P.rowbytes : (unsigned)spl;
            for (int k = 0; k < d; k++)
                memcpy(o + k * spl, P.data + (u32)y * P.stride + (u32)perm[k] * P.rowbytes, n);
        } else
            for (int x = 0; x < P.w; x++) {
                int pen = P.data[(u32)y * P.stride + (u32)x];
                for (int k = 0; k < d; k++)
                    if (pen >> k & 1)
                        o[k * spl + (x >> 3)] |= (u8)(0x80 >> (x & 7));
            }
    }
    static u32 pal[256];
    int n = (P.flags & PIC_HAM8) ? 64 : (P.flags & PIC_HAM6) ? 16 : (P.flags & PIC_EHB) ? 32 : 1 << d;
    if (n > P.ncol && !(P.flags & PIC_EHB)) n = P.ncol;
    for (int i = 0; i < n; i++)
        pal[i] = (u32)P.pal[3 * i] << 16 | (u32)P.pal[3 * i + 1] << 8 | P.pal[3 * i + 2];
    struct dpyscr s;
    memset(&s, 0, sizeof s);
    s.w = (u16)sw; s.h = (u16)sh; s.depth = (u16)d;
    s.flags = (u16)((lace ? DPYSF_LACE : 0) | (P.flags & (PIC_HAM6 | PIC_HAM8) ? DPYSF_HAM : 0) |
                    (P.flags & PIC_EHB ? DPYSF_EHB : 0));
    s.planes = (u32)smem; s.bytes = bytes; s.pstride = (u32)spl; s.bpr = (u16)sbpr; s.ncol = (u16)n;
    s.palette = (u32)pal;
    int r = dpy_screen(&s);
    if (r < 0) {
        sys_memfree(smem);
        smem = 0;
        scr_say(r == -ENODEV ? "not a mode of this display" : zstrerror(r) ? zstrerror(r) : "refused");
        return;
    }
    onscr = 1;
    lace_on = lace;
    sx = sy = 0;
    if (verbose) {
        char m[48];
        pic_mode(&P, m, sizeof m);
        dprintf(1, "show: screen %d x %d, %d planes (%s)\n", sw, sh, d, m);
    }
}
static void scr_pan(int dx, int dy)     /* dx in fetch units of 8 bytes, dy in lines */
{
    int mx = spl - sw / 8, my = srows - sh;
    if (lace_on)
        my &= ~1;
    sx += dx * 8;
    sy += lace_on ? dy & ~1 : dy;       /* interlaced: whole frame lines, so the fields keep their order */
    if (sx > mx) sx = mx;
    if (sx < 0) sx = 0;
    if (sy > my) sy = my;
    if (sy < 0) sy = 0;
    dpy_scr_set(SS_PLANES, (u32)sy * (u32)sbpr + (u32)sx, 0, 0);
}
static void scr_key(const struct dpyev *e)
{
    if (e->type == EV_BUTTON) {
        if (!(e->code & 0x80)) scr_off();
        return;
    }
    if (e->type != EV_KEY || (e->code & 0x80))
        return;
    int c = e->ch[0], raw = e->code & 0x7F;
    if (c == 's' || c == 'q' || raw == 0x45)
        scr_off();
    else if (c == 'n' || c == ' ' || c == 'p' || raw == 0x41) {
        int was = cur;
        step(c == 'n' || c == ' ' ? 1 : -1);
        if (cur != was) scr_on();
    } else if (raw == 0x4E || c == 'l') scr_pan(1, 0);
    else if (raw == 0x4F || c == 'h') scr_pan(-1, 0);
    else if (raw == 0x4D || c == 'j') scr_pan(0, sh / 4);
    else if (raw == 0x4C || c == 'k') scr_pan(0, -(sh / 4));
}

static void pan(int dx, int dy)
{
    int px = ox, py = oy;
    ox += dx;
    oy += dy;
    place();
    if (ox != px || oy != py)
        draw_all();
}
static void act(int a)
{
    int sx = (ww / 4 + 15) & ~15, sy = wh / 4 ? wh / 4 : 1;
    switch (a) {
    case A_NEXT: step(1); break;
    case A_PREV: step(-1); break;
    case A_FIRST: { int c = cur; cur = -1; step(1); if (cur < 0) cur = c; break; }
    case A_LAST: { int c = cur; cur = nnames; step(-1); if (cur == nnames) cur = c; break; }
    case A_ZOOM:
        fitmode ^= 1;
        render();
        place();
        set_title("");
        draw_all();
        break;
    case A_DITHER:                      /* ordered, Floyd-Steinberg, the nearest pen, again */
        dither = dither == PIC_ORDERED ? PIC_FS : dither == PIC_FS ? PIC_NEAREST : PIC_ORDERED;
        render();
        draw_all();
        break;
    case A_LEFT: if (have && cw > ww) pan(sx, 0); else step(-1); break;
    case A_RIGHT: if (have && cw > ww) pan(-sx, 0); else step(1); break;
    case A_UP: if (have && ch > wh) pan(0, sy); break;
    case A_DOWN: if (have && ch > wh) pan(0, -sy); break;
    case A_HELP: helping = 1; draw_help(); break;
    case A_QUIT: quit = 1; break;
    case A_SCREEN: scr_on(); break;
    }
}
static int pending;
static void key(const struct dpyev *e)
{
    if (e->code & 0x80)
        return;
    int k = keys_code(e);
    if (!k)
        return;
    if (helping) {
        helping = 0;
        draw_all();
        return;
    }
    const struct kbind *b;
    if (pending) {
        int k1 = pending;
        pending = 0;
        if ((b = keys_find(&K, k1, k))) act(b->act);
        return;
    }
    if (keys_prefix(&K, k)) { pending = k; return; }
    if ((b = keys_find(&K, k, 0)))
        act(b->act);
}
static const char menus[] = "Show\tNext\tPrevious\tFit or real size\tDither\tOwn screen\tKeys\tQuit";

/* ================================================================ the screen's pens */
static int pens(void)
{
    unsigned short p[16];
    int d = dpy_palette(p, 16);
    if (d < 1)
        return d < 0 ? d : -EINVAL;
    memcpy(pal12, p, sizeof p);
    depth = d > 4 ? 4 : d;
    u32 t0 = sys_ticks();
    pens_init(&PS, pal12, 1 << depth);
    if (verbose)
        dprintf(1, "show: pens of %d colours in %u ms\n", 1 << depth, (sys_ticks() - t0) * 10);
    int lo = 1 << 30, hi = -1;
    for (int i = 0; i < PS.n; i++) {
        int l = 3 * PS.rgb[i][0] + 6 * PS.rgb[i][1] + PS.rgb[i][2];
        if (l < lo) { lo = l; bgpen = i; }
        if (l > hi) { hi = l; fgpen = i; }
    }
    return 0;
}

static void handle(const struct dpyev *e)
{
    if (onscr && e->win == 0 && (e->type == EV_KEY || e->type == EV_BUTTON || e->type == EV_MOTION ||
                                 e->type == EV_FOCUS)) {
        scr_key(e);                     /* the screen's events (D55: win 0) */
        return;
    }
    switch (e->type) {
    case EV_RESIZE:
        if (e->w != ww || e->h != wh) {
            ww = e->w;
            wh = e->h;
            if (fitmode)
                render();
        }
        place();
        break;
    case EV_EXPOSE: {
        int r[4] = { e->x, e->y, e->x + e->w, e->y + e->h };
        draw_rect(r);
        if (verbose) {
            struct dpyinfo i;
            if (dpy_info(win, &i) == 0)
                dprintf(1, "show: window %d %d %d %d\n", i.ix, i.iy, ww, wh);
        }
        break;
    }
    case EV_SCREEN:
        pens();                         /* (a failure keeps the pens it had) */
        render();
        place();
        set_title("");
        draw_all();
        break;
    case EV_KEY: key(e); break;
    case EV_BUTTON:
        if ((e->code & 0x7F) == 1 && !(e->code & 0x80)) {
            if (helping) { helping = 0; draw_all(); }
            else step(e->qual & (GQ_LSHIFT | GQ_RSHIFT) ? -1 : 1);
        }
        break;
    case EV_MENU:
        if (e->code == 0) {
            static const u8 m[] = { A_NEXT, A_PREV, A_ZOOM, A_DITHER, A_SCREEN, A_HELP, A_QUIT };
            if (e->arg < sizeof m) act(m[e->arg]);
        }
        break;
    case EV_CLOSE: quit = 1; break;
    }
}

int main(int argc, char **argv)
{
    const char *file = 0;
    int own = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-s")) own = 1;
        else if (!strcmp(argv[i], "-n")) dither = PIC_NEAREST;
        else if (!strcmp(argv[i], "-f")) dither = PIC_FS;
        else if (argv[i][0] != '-' && !file) file = argv[i];
        else { dprintf(2, "usage: show [-v] [-s] [-n | -f] PICTURE\n"); return 2; }
    if (!file) { dprintf(2, "usage: show [-v] [-s] [-n | -f] PICTURE\n"); return 2; }
    char full[ZPATH_MAX];
    if (zrealpath(file, full, sizeof full) < 0) {
        dprintf(2, "show: %s: %s\n", file, zwhy("cannot resolve"));
        return 1;
    }
    char *sl = strrchr(full, '/');
    snprintf(dir, sizeof dir, "%.*s", sl == full ? 1 : (int)(sl - full), full);
    int r = dpy_open("show");
    if (r < 0) {
        dprintf(2, "show: the desktop: %s\n", zstrerror(r) ? zstrerror(r) : "not reachable");
        return 1;
    }
    if (keys_load(&K, defaults, sizeof defaults - 1, "show") && verbose)
        dprintf(1, "show: wrote ~/.config/show/keys\n");
    if ((r = pens()) < 0) {
        dprintf(2, "show: the screen's colours: %s\n", zstrerror(r) ? zstrerror(r) : "not given");
        dpy_close();
        return 1;
    }
    ww = dpy_w - 24;
    wh = dpy_h - 48;
    load(full, sl + 1, 0);
    int nw = 320, nh = 128;
    if (have) {
        natural(&nw, &nh);
        if (nw < 160) nw = 160;
        if (nh < 40) nh = 40;
    } else if (why[0])
        dprintf(2, "show: %s: %s\n", file, why);
    struct dpywin d;
    memset(&d, 0, sizeof d);
    d.kind = WK_NORMAL;
    d.x = d.y = -1;
    d.w = (u16)(nw < ww ? nw : ww);
    d.h = (u16)(nh < wh ? nh : wh);
    d.minw = 64;
    d.minh = 32;
    win = dpy_newwin(&d);
    if (win < 0) {
        dprintf(2, "show: no window: %s\n", zstrerror(win) ? zstrerror(win) : "refused");
        dpy_close();
        return 1;
    }
    ww = d.w;
    wh = d.h;
    dpy_menu(win, menus, sizeof menus - 1);
    set_title("");
    render();
    place();
    draw_all();
    if (folder(sl + 1) < 0) {
        dpy_title(win, "(the folder was not read whole)");
        if (verbose) dprintf(1, "show: say: the folder was not read whole\n");
    }
    if (K.bad[0]) {
        dpy_title(win, K.bad);              /* until the next picture's title */
        if (verbose) dprintf(1, "show: say: %s\n", K.bad);
    }
    if (own)
        scr_on();
    while (!quit) {
        struct dpyev e;
        int got = dpy_next(&e, -1);
        if (got < 0)
            break;
        if (got)
            handle(&e);
        if (dpy_lost())
            draw_all();
    }
    scr_off();
    dpy_close();
    return have ? 0 : 1;
}
