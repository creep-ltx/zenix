/* SPDX-License-Identifier: GPL-2.0-only */
/* show's decoders and renderer on the host (display/show/pic.c, DESIGN D50), under AddressSanitizer:
 *  - every picture tools/mkpics.py made (NAME.iff / NAME.gif with NAME.exp): its size, its mode in words and every
 *    pixel against the RGB the generator worked out;
 *  - every damaged file (bad-NAME with bad-NAME.why): refused with that reason, or "cut": shown with what is there;
 *  - the renderer: a picture in exactly the screen's pens comes out in those pens, nearest and dithered, whole and
 *    halved; dithering a grey between two pens gives both;
 *  - mutations of every good file (bytes changed, cut, grown): each decoded or refused, drawn shrunk and enlarged,
 *    nothing read or written outside, every allocation given back on a refusal and only the picture kept on success.
 * Usage: show-host-test CASEDIR [MUTATIONS] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "../display/show/pic.h"

static int fails, checks, live;
static void *al(unsigned n) { void *p = malloc(n ? n : 1); if (p) live++; return p; }
static void rel(void *p) { if (p) { live--; free(p); } }

static unsigned char *slurp(const char *path, unsigned *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(l ? (size_t)l : 1);
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = 0; }
    fclose(f);
    *n = (unsigned)l;
    return b;
}
static void bad(const char *what, const char *name, const char *detail)
{
    fails++;
    if (fails < 40) printf("FAIL %s: %s %s\n", name, what, detail ? detail : "");
}

/* draw every row and render it small: the mutations' check that a decoded picture is safe to show */
static void exercise(const struct pic *p)
{
    unsigned char *rb = malloc((size_t)p->w * 4 + 24);
    for (int y = 0; y < p->h; y++) pic_row(p, y, rb);
    static struct pens ps;
    static int init;
    unsigned short pal[16] = { 0x344, 0x112, 0xEEE, 0x223, 0x47B, 0xBBC, 0x455, 0xFFF,
                               0xCCD, 0xD54, 0x7C5, 0xEB4, 0x69E, 0xB7C, 0x5BC, 0x567 };
    if (!init) { pens_init(&ps, pal, 16); init = 1; }
    struct canvas cv = { p->w < 61 ? p->w : 61, p->h < 23 ? p->h : 23, 4, 0, 0 };
    cv.bpr = (unsigned)((cv.w + 15) / 16 * 2);
    cv.p = malloc(cv.bpr * 4u * (unsigned)cv.h);
    short *work = malloc((size_t)(cv.w + 2) * 6);
    unsigned short *xs = malloc((size_t)cv.w * 2);
    static int mode;
    pic_render(p, &cv, &ps, mode++ % 3, rb, work, xs);
    free(cv.p); free(work); free(xs);
    struct canvas up = { p->w * 2 < 300 ? p->w * 2 : 300, p->h * 2 < 90 ? p->h * 2 : 90, 3, 0, 0 };    /* enlarged */
    up.bpr = (unsigned)((up.w + 15) / 16 * 2);
    up.p = malloc(up.bpr * 3u * (unsigned)up.h);
    work = malloc((size_t)(up.w + 2) * 6);
    xs = malloc((size_t)up.w * 2);
    pic_render(p, &up, &ps, mode++ % 3, rb, work, xs);
    free(up.p); free(work); free(xs); free(rb);
}

static void one(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    unsigned n, en;
    unsigned char *f = slurp(path, &n);
    char ep[1024];
    snprintf(ep, sizeof ep, "%s", path);
    strcpy(strrchr(ep, '.'), ".exp");
    unsigned char *e = slurp(ep, &en);
    if (!f || !e) { bad("cannot read", name, 0); free(f); free(e); return; }
    int w, h;
    char mode[64] = "";
    unsigned char *nl = memchr(e, '\n', en);
    if (!nl || sscanf((char *)e, "%d %d", &w, &h) != 2) { bad("its .exp", name, 0); free(f); free(e); return; }
    const char *sp = strchr((char *)e, ' ');
    sp = sp ? strchr(sp + 1, ' ') : 0;
    if (sp) snprintf(mode, sizeof mode, "%.*s", (int)((char *)nl - sp - 1), sp + 1);
    struct pic p;
    const char *why = pic_decode(f, n, &p, al, rel);
    checks++;
    if (why) { bad("refused:", name, why); free(f); free(e); return; }
    char m[64];
    pic_mode(&p, m, sizeof m);
    if (p.w != w || p.h != h) { char d[64]; snprintf(d, sizeof d, "%dx%d, %dx%d expected", p.w, p.h, w, h); bad("size", name, d); }
    else if (strcmp(m, mode)) { char d[160]; snprintf(d, sizeof d, "\"%s\", \"%s\" expected", m, mode); bad("mode", name, d); }
    else if (en - (unsigned)(nl + 1 - e) != (unsigned)(w * h * 3)) bad("its .exp's length", name, 0);
    else {
        unsigned char *rb = malloc((size_t)w * 4 + 24);
        int diff = 0, fx = 0, fy = 0;
        for (int y = 0; y < h && !diff; y++) {
            pic_row(&p, y, rb);
            for (int x = 0; x < w; x++)
                if (memcmp(rb + x * 3, nl + 1 + ((size_t)y * w + x) * 3, 3)) { diff = 1; fx = x; fy = y; break; }
        }
        if (diff) {
            char d[96];
            const unsigned char *q = nl + 1 + ((size_t)fy * w + fx) * 3;
            pic_row(&p, fy, rb);
            snprintf(d, sizeof d, "pixel %d,%d is %02x%02x%02x, %02x%02x%02x expected", fx, fy, rb[fx * 3],
                     rb[fx * 3 + 1], rb[fx * 3 + 2], q[0], q[1], q[2]);
            bad("pixels:", name, d);
        }
        free(rb);
    }
    rel(p.data);
    if (live) bad("allocations left", name, 0);
    live = 0;
    free(f); free(e);
}

static void damaged(const char *dir, const char *name)
{
    char path[1024], wp[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(wp, sizeof wp, "%s", path);
    strcpy(strrchr(wp, '.'), ".why");
    unsigned n, wn;
    unsigned char *f = slurp(path, &n), *w = slurp(wp, &wn);
    if (!f || !w) { bad("cannot read", name, 0); free(f); free(w); return; }
    char want[128];
    snprintf(want, sizeof want, "%.*s", (int)wn, (char *)w);
    struct pic p;
    const char *why = pic_decode(f, n, &p, al, rel);
    checks++;
    if (!strcmp(want, "cut")) {
        if (why) bad("refused, not shown cut:", name, why);
        else if (!(p.flags & PIC_CUT)) bad("not marked cut", name, 0);
        else exercise(&p);
        if (!why) rel(p.data);
    } else if (!why) {
        bad("shown, not refused", name, want);
        rel(p.data);
    } else if (strncmp(why, want, strlen(want))) {
        char d[200];
        snprintf(d, sizeof d, "\"%s\", \"%s\" expected", why, want);
        bad("reason", name, d);
    }
    if (live) bad("allocations left", name, 0);
    live = 0;
    free(f); free(w);
}

/* the renderer: 4 x 2 blocks of 8 x 4 pixels in the 16 pens, one pen a block, and a grey between two pens */
static void render_tests(void)
{
    static unsigned short pal[16] = { 0x344, 0x112, 0xEEE, 0x223, 0x47B, 0xBBC, 0x455, 0xFFF,
                                      0xCCD, 0xD54, 0x7C5, 0xEB4, 0x69E, 0xB7C, 0x5BC, 0x567 };
    static struct pens ps;
    pens_init(&ps, pal, 16);
    for (int i = 0; i < 16; i++) {
        checks++;
        if (ps.lut[pal[i]] != i) { char d[48]; snprintf(d, sizeof d, "pen %d's colour gives %d", i, ps.lut[pal[i]]); bad("lut", "pens", d); }
    }
    struct pic p;
    memset(&p, 0, sizeof p);
    p.w = 64; p.h = 8; p.depth = 8; p.flags = PIC_CHUNKY; p.rowbytes = p.stride = 64; p.ncol = 16;
    for (int i = 0; i < 16; i++) {
        p.pal[i * 3] = (unsigned char)((pal[i] >> 8 & 15) * 17);
        p.pal[i * 3 + 1] = (unsigned char)((pal[i] >> 4 & 15) * 17);
        p.pal[i * 3 + 2] = (unsigned char)((pal[i] & 15) * 17);
    }
    unsigned char data[64 * 8];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 64; x++) data[y * 64 + x] = (unsigned char)(x / 8 + 8 * (y / 4));
    p.data = data;
    unsigned char rb[64 * 4 + 24];
    short work[(64 + 2) * 6];
    unsigned short xs[64];
    for (int pass = 0; pass < 9; pass++) {      /* nearest, Floyd-Steinberg, ordered; whole, halved, 37 across */
        int dith = pass % 3 == 1 ? PIC_FS : pass % 3 == 2 ? PIC_ORDERED : PIC_NEAREST, half = pass / 3;
        struct canvas cv = { half == 2 ? 37 : half ? 32 : 64, half == 1 ? 4 : 8, 4, 0, 0 };
        cv.bpr = (unsigned)((cv.w + 15) / 16 * 2);
        unsigned char *c = calloc(cv.bpr * 4u * (unsigned)cv.h, 1);
        cv.p = c;
        pic_render(&p, &cv, &ps, dith, rb, work, xs);
        int wrong = 0;
        for (int y = 0; y < cv.h; y++)
            for (int x = 0; x < cv.w; x++) {
                int pen = 0;
                for (int k = 0; k < 4; k++)
                    if (c[(size_t)y * cv.bpr * 4 + (size_t)k * cv.bpr + (size_t)(x >> 3)] & (0x80 >> (x & 7))) pen |= 1 << k;
                int sx = half == 2 ? (2 * x + 1) * 64 / 74 : half ? x * 2 : x, sy = half == 1 ? y * 2 : y;
                if (pen != data[sy * 64 + sx]) wrong++;
            }
        for (int y = 0; y < cv.h; y++)          /* the padding right of the picture stays 0 */
            for (int x = cv.w; x < (int)cv.bpr * 8; x++)
                for (int k = 0; k < 4; k++)
                    if (c[(size_t)y * cv.bpr * 4 + (size_t)k * cv.bpr + (size_t)(x >> 3)] & (0x80 >> (x & 7))) wrong++;
        checks++;
        if (wrong) { char d[64]; snprintf(d, sizeof d, "%d of %d pixels not their pen (pass %d)", wrong, cv.w * cv.h, pass); bad("render", "pens picture", d); }
        free(c);
    }
    /* a grey halfway between pen 2 (EEE) and pen 1 (112)... a mid grey: dithered it uses more than one pen, nearest one */
    unsigned char g[64 * 8];
    memset(g, 16, sizeof g);
    p.pal[16 * 3] = p.pal[16 * 3 + 1] = p.pal[16 * 3 + 2] = 0x90;
    p.ncol = 17;
    p.data = g;
    for (int dith = 0; dith < 3; dith++) {
        struct canvas cv = { 64, 8, 4, 8, 0 };
        unsigned char *c = calloc(8 * 4 * 8, 1);
        cv.p = c;
        pic_render(&p, &cv, &ps, dith, rb, work, xs);
        int used[16] = { 0 }, nu = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 64; x++) {
                int pen = 0;
                for (int k = 0; k < 4; k++)
                    if (c[(size_t)y * 32 + (size_t)k * 8 + (size_t)(x >> 3)] & (0x80 >> (x & 7))) pen |= 1 << k;
                if (!used[pen]++) nu++;
            }
        checks++;
        if (dith ? nu < 2 : nu != 1) { char d[48]; snprintf(d, sizeof d, "%d pens used (mode %d)", nu, dith); bad("render", "grey", d); }
        free(c);
    }
}

static unsigned rs = 12345;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: show-host-test CASEDIR [MUTATIONS]\n"); return 2; }
    int muts = argc > 2 ? atoi(argv[2]) : 20000;
    DIR *d = opendir(argv[1]);
    if (!d) { perror(argv[1]); return 1; }
    struct dirent *de;
    char *names[512];
    int nn = 0, ngood = 0, nbad = 0;
    while ((de = readdir(d)) && nn < 512) {
        const char *x = strrchr(de->d_name, '.');
        if (!x || (strcmp(x, ".iff") && strcmp(x, ".gif"))) continue;
        names[nn++] = strdup(de->d_name);
    }
    closedir(d);
    for (int i = 0; i < nn; i++)
        if (!strncmp(names[i], "bad-", 4)) { damaged(argv[1], names[i]); nbad++; }
        else { one(argv[1], names[i]); ngood++; }
    int f1 = fails;
    printf("%s %d pictures decoded and compared pixel by pixel, %d damaged files refused or cut as expected\n",
           f1 ? "FAIL" : "PASS", ngood, nbad);
    render_tests();
    {   /* audit 2026-11-14 D3: a HAM8 file of 7 planes: every screen plane taken from a plane the file has, each once */
        struct pic hp;
        memset(&hp, 0, sizeof hp);
        hp.flags = PIC_HAM8;
        for (int dp = 7; dp <= 8; dp++) {
            hp.depth = dp;
            int seen = 0, ok = 1;
            for (int k = 0; k < dp; k++) {
                int q = pic_plane(&hp, k);
                if (q < 0 || q >= dp || (seen & 1 << q)) ok = 0;
                else seen |= 1 << q;
            }
            ok = ok && pic_plane(&hp, 0) == dp - 2 && pic_plane(&hp, 1) == dp - 1 && pic_plane(&hp, 2) == 0;
            checks++;
            if (!ok) bad("HAM8's planes", dp == 7 ? "7 planes" : "8 planes", "a plane outside the file or twice");
        }
        hp.flags = 0; hp.depth = 5;
        checks++;
        if (pic_plane(&hp, 4) != 4) bad("planes in order", "5 planes", 0);
    }
    printf("%s the renderer: the pens' own colours in their pens (nearest, Floyd-Steinberg, ordered; whole, halved, 37 across with its padding 0), a grey in one pen nearest and in more ordered and Floyd-Steinberg\n",
           fails > f1 ? "FAIL" : "PASS");
    int f2 = fails, decoded = 0;
    for (int m = 0; m < muts; m++) {
        const char *nm = names[rnd() % (unsigned)nn];
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", argv[1], nm);
        unsigned n;
        unsigned char *f = slurp(path, &n);
        if (!f) continue;
        unsigned k = 1 + rnd() % 8, nf = n;
        int how = (int)(rnd() % 4);
        unsigned char *g = malloc(n + 64);
        memcpy(g, f, n);
        if (how == 0 && n) for (unsigned i = 0; i < k; i++) g[rnd() % n] = (unsigned char)rnd();
        else if (how == 1 && n) nf = rnd() % n;
        else if (how == 2 && n) for (unsigned i = 0; i < k; i++) g[rnd() % (n < 128 ? n : 128)] = (unsigned char)rnd();
        else { for (unsigned i = 0; i < 64; i++) g[n + i] = (unsigned char)rnd(); nf = n + rnd() % 64; }
        unsigned char *h = malloc(nf ? nf : 1);         /* exactly nf bytes: a read past them is caught */
        memcpy(h, g, nf);
        struct pic p;
        const char *why = pic_decode(h, nf, &p, al, rel);
        if (!why) {
            decoded++;
            if (live != 1) bad("allocations kept", nm, "(mutation)");
            exercise(&p);
            rel(p.data);
        } else if (live)
            bad("allocations left after a refusal", nm, why);
        live = 0;
        free(h); free(g); free(f);
    }
    printf("%s %d mutations: %d decoded and drawn, the rest refused; nothing outside, nothing left allocated\n",
           fails > f2 ? "FAIL" : "PASS", muts, decoded);
    for (int i = 0; i < nn; i++) free(names[i]);
    printf("show-host-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
