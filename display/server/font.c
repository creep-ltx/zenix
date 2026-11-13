/* SPDX-License-Identifier: GPL-2.0-only */
/* displayd's fonts (DESIGN D38 5, docs/d38-desktop.md "Fonts").
 *
 * Font 0, the 8 x 8: the console's own, held once in the server - Topaz 8 as the kernel took it from the ROM
 * (CON_GRAB copies it: never shipped) for Latin-1, and the Unicode supplement /usr/share/consolefonts/zenix-8x8.zcf
 * (ZCF1, DESIGN D28 7, the format in docs/frozen-interfaces.md) for the rest. A code point with no glyph draws the
 * box, as the console. Its checks are the kernel's (confont.c): a damaged file is not used. Terminals' cells.
 *
 * Font 1 (DPYF_UI), the desktop's: an AmigaOS bitmap font named by /etc/zdesk/screen's "font NAME SIZE", read from
 * /usr/share/fonts/amiga/NAME.font and the file it names for SIZE (amifont.c checks and converts it once, at the
 * start: a packed picture a glyph, in fast RAM). Window titles, and what clients draw with it (zdesk's bar, menus,
 * labels). A code point it lacks is drawn by the 8 x 8, its baseline on the font's. A run of text is put together in
 * a mask in fast RAM, then written into the bitmap a byte a plane (bm_mask), so chip RAM is written once and read
 * only at a run's two ends. Without the setting, or when the file cannot be read (one line on standard error), font
 * 1 is font 0. */
#include "dpyd.h"
#include "amifont.h"
#include "../ttf/ttfont.h"

static u8 topaz[224 * 8];
static u8 *zcf;                         /* the file (checked), or 0 */
static u32 zglyphs;
static const u8 box[8] = { 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00 };

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

static int zcf_ok(const u8 *d, u32 n)
{
    if (n < 32 + 512 || memcmp(d, "ZCF1", 4) || be16(d + 4) != 32 || be16(d + 6) || d[8] != 8 || d[9] != 8)
        return 0;
    u32 g = be16(d + 10), pages = be16(d + 12);
    if (g < 3 || g > 4096 || pages > 255 || be32(d + 16) != n || n != 32 + 512 + 256 * pages + 8 * g)
        return 0;
    for (int i = 0; i < 512; i++)
        if (d[32 + i] > pages)
            return 0;
    for (u32 i = 0; i < pages * 128; i++) {
        u16 e = be16(d + 32 + 512 + 2 * i);
        if (e && !(e >= 32 && e <= 126) && !(e >= 160 && e <= 255) && !(e >= 256 && e < 256 + g))
            return 0;
    }
    zglyphs = g;
    return 1;
}

void font_init(const u8 *t)
{
    memcpy(topaz, t, sizeof topaz);
    int fd = sys_open("/usr/share/consolefonts/zenix-8x8.zcf", O_RDONLY);
    if (fd < 0)
        return;
    struct ustat st;
    if (sys_fstat(fd, &st) == 0 && st.size <= 98848 && (zcf = malloc(st.size))) {
        u32 got = 0;
        while (got < st.size) {
            int r = sys_read(fd, zcf + got, st.size - got);
            if (r <= 0)
                break;
            got += (u32)r;
        }
        if (got != st.size || !zcf_ok(zcf, st.size)) {
            dprintf(2, "displayd: /usr/share/consolefonts/zenix-8x8.zcf is damaged: not used\n");
            free(zcf);
            zcf = 0;
        }
    }
    sys_close(fd);
}

const u8 *font_glyph(u32 cp)
{
    if (cp >= 32 && cp < 256 && !(cp >= 0x7F && cp < 0xA0))
        return topaz + (cp - 32) * 8;
    if (!zcf || cp > 0xFFFF)
        return zcf ? zcf + 32 + 512 + 256 * be16(zcf + 12) : box;
    u8 page = zcf[32 + (cp >> 7)];
    if (!page)
        return zcf + 32 + 512 + 256 * be16(zcf + 12);   /* glyph 0: the font's box */
    u16 e = be16(zcf + 32 + 512 + 256 * (u32)(page - 1) + 2 * (cp & 127));
    if (!e)
        return zcf + 32 + 512 + 256 * be16(zcf + 12);
    if (e < 256)
        return topaz + (e - 32) * 8;
    return zcf + 32 + 512 + 256 * be16(zcf + 12) + 8 * (u32)(e - 256);
}

/* ---- the desktop's font ---- */
#define MB 84                           /* the mask's bytes a row: a strip of 672 pixels (wider text in strips) */
static struct af_font *ufp;             /* malloc'd at load (2 KB): nothing without a font */
#define uf (*ufp)
#define uf_on (ufp != 0)
static u8 *mbuf;                        /* uf.h rows of MB bytes */
static char uf_name[32];

#define COLD __attribute__((cold, optimize("Os")))   /* load-time code: small (the drawing below is -O2) */
static void *fast_alloc(unsigned n) { return malloc(n); }
/* a whole file into memory (at most max bytes) -> the buffer (free it), or 0 with *why */
COLD static u8 *slurp(const char *path, u32 max, u32 *n, const char **why)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        *why = "not there";
        return 0;
    }
    struct ustat st;
    u8 *b = 0;
    int r = sys_fstat(fd, &st);
    if (r < 0 || st.size > max) {
        *why = r < 0 ? "cannot be read" : "too large";
        sys_close(fd);
        return 0;
    }
    if (!(b = malloc(st.size ? st.size : 1))) {
        *why = "no memory to read it";
        sys_close(fd);
        return 0;
    }
    u32 got = 0;
    while (got < st.size) {
        int r = sys_read(fd, b + got, st.size - got);
        if (r <= 0)
            break;
        got += (u32)r;
    }
    sys_close(fd);
    if (got != st.size) {
        free(b);
        *why = "cannot be read";
        return 0;
    }
    *n = got;
    return b;
}

/* A TrueType font (DESIGN D51, docs/d51-truetype.md): /usr/share/fonts/truetype/NAME.ttf, drawn once a size by
 * ttf2font into the Amiga font file /var/cache/fonts/NAME/SIZE, whose stamp names the .ttf's size and time and the
 * renderer's version. A missing or stale file is made now (ttf2font, waited for: the screen stands still while it
 * draws, once), then read as any Amiga font. -> the file (free it) and path its name; or 0, *why, path the .ttf's */
COLD static u8 *ttf_cached(const char *name, int size, char *path, u32 *len, const char **why)
{
    char ttf[64], sz[4];
    struct ustat ts;
    snprintf(ttf, sizeof ttf, "/usr/share/fonts/truetype/%s.ttf", name);
    snprintf(path, 64, "/var/cache/fonts/%s/%d", name, size);
    if (sys_stat(ttf, &ts) < 0) {
        strcpy(path, "/usr/share/fonts/amiga/");
        strcat(path, name);
        strcat(path, ".font");                  /* neither: said as the Amiga font's name, as before D51 */
        *why = "not there";
        return 0;
    }
    u8 *d = slurp(path, AF_MAXFILE, len, why);
    if (d && ttf_stamp_ok(d, *len, ts.size, ts.mtime, size))
        return d;
    free(d);
    snprintf(sz, sizeof sz, "%d", size);
    char *argv[] = { "/bin/ttf2font", "-q", ttf, sz, 0 };
    int io[3] = { -1, 2, 2 };
    u32 t0 = sys_ticks();
    int pid = sys_spawn(argv[0], argv, io);
    if (pid < 0) {
        strcpy(path, ttf);
        *why = "ttf2font cannot be started";
        return 0;
    }
    sys_wait(pid);      /* its status is not the test: the size's file may be written when NAME.font was not */
    d = slurp(path, AF_MAXFILE, len, why);
    if (!d || !ttf_stamp_ok(d, *len, ts.size, ts.mtime, size)) {
        free(d);
        strcpy(path, ttf);
        *why = "ttf2font could not draw it";
        return 0;
    }
    if (d)
        dprintf(1, "displayd: %s drawn at %d lines in %u.%02u s\n", ttf, size, (sys_ticks() - t0) / 100,
                (sys_ticks() - t0) % 100);
    return d;
}

/* said in one line; the font in use stays (at the start the 8 x 8) -> -ENOENT (no such file), -ENOMEM, -EINVAL */
COLD static int refused(const char *path, const char *why)
{
    dprintf(2, "displayd: %s: %s: %s\n", path, why, ufp ? "the font in use stays" : "the built-in 8x8 is used");
    return !strcmp(why, "not there") ? -ENOENT : !strncmp(why, "no memory", 9) ? -ENOMEM : -EINVAL;
}
/* the 8 x 8 from now on (DPY_FONT_SET with no name, D38 "Preferences") */
COLD void font_ui_drop(void)
{
    if (ufp) {
        free(ufp->bits);
        free(ufp);
    }
    free(mbuf);
    ufp = 0;
    mbuf = 0;
}
COLD int font_ui_load(const char *name, int size)
{
    static const char dir[] = "/usr/share/fonts/amiga/";
    char path[sizeof dir + 32 + 256], file[256];
    const char *why = 0;
    u32 n = strlen(name);
    int ok = n && n < 26 && size > 0 && size <= AF_MAXH;
    for (u32 i = 0; i < n; i++)
        if ((u8)name[i] <= ' ' || (u8)name[i] > 0x7E || name[i] == '/' || name[i] == ':')
            ok = 0;
    if (!ok || (name[0] == '.' && (n == 1 || (n == 2 && name[1] == '.'))))
        return refused(name, "not a font's name and size");
    strcpy(path, dir);
    strcat(path, name);
    strcat(path, ".font");
    u32 len;
    u8 *d = slurp(path, AF_MAXCONTENTS, &len, &why);
    if (d) {
        why = af_contents(d, len, size, file, sizeof file, 0, 0);
        free(d);
        if (why)
            return refused(path, why);
        strcpy(path + sizeof dir - 1, file);    /* file: at most 255 bytes, checked by af_contents */
        d = slurp(path, AF_MAXFILE, &len, &why);
    } else if (!strcmp(why, "not there"))
        d = ttf_cached(name, size, path, &len, &why);   /* no Amiga font of that name: a TrueType one (D51) */
    if (!d)
        return refused(path, why);
    struct af_font *f = malloc(sizeof *f);
    why = f ? af_load(d, len, f, fast_alloc) : "no memory";
    free(d);
    u8 *m = why ? 0 : malloc((u32)f->h * MB);
    if (!why && !m) {
        free(f->bits);
        why = "no memory";
    }
    if (why) {
        free(f);
        return refused(path, why);
    }
    if (ufp) {
        free(ufp->bits);
        free(ufp);
    }
    free(mbuf);
    ufp = f;
    mbuf = m;
    strncpy(uf_name, name, 26);
    uf_name[26] = 0;
    n = strlen(uf_name);
    uf_name[n++] = ' ';
    if (size >= 10) uf_name[n++] = (char)('0' + size / 10);
    uf_name[n++] = (char)('0' + size % 10);
    uf_name[n] = 0;
    dprintf(1, "displayd: font %s: %u lines, %s, %u glyphs in %u bytes%s\n", uf_name, uf.h,
            uf.prop ? "proportional" : "fixed width", uf.nglyphs, uf.nbytes + (u32)uf.h * MB + (u32)sizeof uf,
            uf.colour ? " (a colour font, drawn in one colour)" : "");
    return 0;
}

int font_id(int f) { return f == DPYF_UI && uf_on ? 1 : 0; }
int font_h(int f) { return f && uf_on ? uf.h : 8; }

/* the next code point of n bytes of UTF-8 at *i (a broken sequence: its first byte, as Latin-1) */
static u32 next_cp(const u8 *s, u32 n, u32 *i)
{
    u32 cp = s[*i];
    int k = cp < 0x80 ? 1 : cp >= 0xC2 && cp <= 0xDF ? 2 : cp >= 0xE0 && cp <= 0xEF ? 3 : cp >= 0xF0 && cp <= 0xF4 ? 4 : 1;
    if (k > 1 && *i + (u32)k <= n) {
        int ok = 1;
        for (int j = 1; j < k; j++)
            if ((s[*i + j] & 0xC0) != 0x80)
                ok = 0;
        if (ok) {
            cp &= (u32)(0x7F >> k);
            for (int j = 1; j < k; j++)
                cp = cp << 6 | (s[*i + j] & 0x3F);
        } else
            k = 1;
    } else
        k = 1;
    *i += (u32)k;
    return cp;
}
static int adv_of(u32 cp) { return cp < 256 && uf.g[cp].have ? uf.g[cp].adv : 8; }

int font_w(int f, const u8 *s, u32 n)
{
    int w = 0;
    for (u32 i = 0; i < n;) {
        u32 cp = next_cp(s, n, &i);
        w += f && uf_on ? adv_of(cp) : 8;
    }
    return w;
}
COLD u32 font_fit(int f, const u8 *s, u32 n, int maxw, int *w)
{
    int x = 0;
    u32 i = 0;
    while (i < n) {
        u32 j = i, cp = next_cp(s, n, &j);
        int a = f && uf_on ? adv_of(cp) : 8;
        if (x + a > maxw)
            break;
        x += a;
        i = j;
    }
    *w = x;
    return i;
}

COLD void font_info(int f, struct dpyfont *o)
{
    int u = f && uf_on;
    o->id = (unsigned short)(u ? DPYF_UI : 0);
    o->height = (unsigned short)(u ? uf.h : 8);
    o->baseline = (unsigned short)(u ? uf.base : 6);
    o->xsize = (unsigned short)(u ? uf.xsize : 8);
    o->flags = (unsigned short)(u ? (uf.prop ? DPYFF_PROP : 0) : DPYFF_BUILTIN);
    o->other = 8;
    memset(o->name, 0, sizeof o->name);
    if (u)
        strcpy(o->name, uf_name);
    for (int c = 0; c < 256; c++)
        o->adv[c] = (u8)(u ? adv_of((u32)c) : 8);
}

/* a glyph's picture into the mask: rows of bpr bytes, its row gr0 at the mask's row 0, its left edge at pixel px of the
 * mask (may be left of it); rows rows, nb bytes of the mask in use */
static void put_glyph(const u8 *bits, int bpr, int gh, int gr0, int px, int rows, int nb)
{
    for (int j = 0; j < bpr; j++, px += 8) {
        if (px <= -8 || px >= nb * 8)
            continue;
        int q = px >> 3, sh = px & 7;           /* px < 0: q -1, only the right part lands (in byte 0) */
        u8 *m = mbuf;
        for (int r = 0; r < rows; r++, m += MB) {
            int gr = gr0 + r;
            if (gr < 0 || gr >= gh)
                continue;
            u8 v = bits[gr * bpr + j];
            if (!v)
                continue;
            if (q >= 0)
                m[q] |= (u8)(v >> sh);
            if (sh && q + 1 < nb)
                m[q + 1] |= (u8)(v << (8 - sh));
        }
    }
}

/* n bytes of UTF-8 at (x, y) in font f, fg on bg (TA_*), clipped to clip and the bitmap: font 0 a glyph at a time
 * (bm_text, a terminal's cells), font 1 through the mask */
void font_run(struct bm *b, int x, int y, int f, const u8 *s, u32 n, int fg, int bg, int attr, const struct rect *clip)
{
    if (!(f && uf_on)) {
        for (u32 i = 0; i < n;) {
            u32 cp = next_cp(s, n, &i);
            if (x + 8 > clip->x0 && x < clip->x1)
                bm_text(b, x, y, font_glyph(cp), fg, bg, attr, clip);
            x += 8;
        }
        return;
    }
    if (attr & TA_INV) { int t = fg; fg = bg; bg = t; }
    struct rect c = *clip;
    if (c.x0 < 0) c.x0 = 0;
    if (c.y0 < 0) c.y0 = 0;
    if (c.x1 > b->w) c.x1 = (s16)b->w;
    if (c.y1 > b->h) c.y1 = (s16)b->h;
    if (c.y0 < y) c.y0 = (s16)y;
    if (c.y1 > y + uf.h) c.y1 = (s16)(y + uf.h);
    if (c.x0 >= c.x1 || c.y0 >= c.y1 || !b->p)
        return;
    int r0 = c.y0 - y, rows = c.y1 - c.y0, ul = uf.base + 1 < uf.h ? uf.base + 1 : uf.h - 1;
    for (int sx = c.x0; sx < c.x1;) {           /* a strip of at most MB bytes */
        int bx0 = sx >> 3, ex = (bx0 + MB) * 8 < c.x1 ? (bx0 + MB) * 8 : c.x1, nb = ((ex + 7) >> 3) - bx0;
        int left = bx0 * 8;
        memset(mbuf, 0, (u32)rows * MB);
        int px = x;
        for (u32 i = 0; i < n && px - 64 < ex;) {
            u32 cp = next_cp(s, n, &i);
            if (cp < 256 && uf.g[cp].have) {
                const struct af_glyph *g = &uf.g[cp];
                int gx = px + g->kern;
                if (g->bpr && gx + g->w > left && gx < ex)
                    put_glyph(uf.bits + g->off, g->bpr, uf.h, r0, gx - left, rows, nb);
                px += g->adv;
            } else {                            /* the 8 x 8 stands in, its baseline (line 6) on the font's */
                if (px + 8 > left && px < ex)
                    put_glyph(font_glyph(cp), 1, 8, r0 - (uf.base - 6), px - left, rows, nb);
                px += 8;
            }
            nglyphs++;
        }
        if (attr & (TA_BOLD | TA_UNDER))
            for (int r = 0; r < rows; r++) {
                u8 *m = mbuf + r * MB;
                if ((attr & TA_UNDER) && r0 + r == ul)
                    memset(m, 0xFF, (u32)nb);
                else if (attr & TA_BOLD)        /* smeared one pixel right, as the 8 x 8's bold */
                    for (int q = nb - 1; q >= 0; q--)
                        m[q] = (u8)(m[q] | m[q] >> 1 | (q ? m[q - 1] << 7 : 0));
            }
        bm_mask(b, bx0, c.y0, rows, mbuf, MB, sx, ex, fg, bg);
        sx = ex;
    }
}
