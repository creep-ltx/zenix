/* SPDX-License-Identifier: GPL-2.0-only */
/* show's picture decoders and renderer (DESIGN D50, docs/d50-viewers.md; display/show/pic.h). Written from the
 * public specifications: "EA IFF 85" and "ILBM IFF Interleaved Bitmap" (Electronic Arts, 1985; the AmigaOS 3.1
 * includes' CAMG mode bits), the Amiga Hardware Reference Manual's hold-and-modify and extra-half-brite, the AGA
 * notes' HAM8, and CompuServe's "GIF89a" (1990). The knowledge base's amiga/iff-ilbm-viewer.md names the real files'
 * oddities this follows (runs across plane rows, 4-bit CMAPs, CAMG cleaning, one nested FORM). */
#include "pic.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
/* the file is built small (-Os); the per-pixel loops fast */
#define HOT __attribute__((optimize("O2")))

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 le16(const u8 *p) { return (u32)p[1] << 8 | p[0]; }
static int id(const u8 *p, const char *s) { return p[0] == (u8)s[0] && p[1] == (u8)s[1] && p[2] == (u8)s[2] && p[3] == (u8)s[3]; }
/* a division the compiler must not turn into a 64-bit multiply (built -mtune=68020, which does that for a constant
 * divisor; a 68060 has no 64-bit muls.l) */
__attribute__((noipa)) static u32 udiv(u32 a, u32 b) { return a / b; }
static void zero(void *d, u32 n) { u8 *q = d; while (n--) *q++ = 0; }
static void copy(void *d, const void *s, u32 n) { u8 *q = d; const u8 *r = s; while (n--) *q++ = *r++; }

const char *pic_kind(const unsigned char *f, unsigned n)
{
    if (n >= 12 && id(f, "FORM") && id(f + 8, "ILBM"))
        return "ILBM";
    if (n >= 6 && id(f, "GIF8") && (f[4] == '7' || f[4] == '9') && f[5] == 'a')
        return "GIF";
    return 0;
}

static void grey(struct pic *p, int n)
{
    for (int i = 0; i < n; i++)
        p->pal[i * 3] = p->pal[i * 3 + 1] = p->pal[i * 3 + 2] = (u8)(n > 1 ? i * 255 / (n - 1) : 0);
    p->ncol = n;
}

/* ================================================================ ILBM */
/* ByteRun1 as one stream over the whole BODY: a run or a literal may go on into the next plane's row */
struct br { const u8 *s, *e; u32 lit, run; u8 rb; };
static int unpack(struct br *b, u8 *o, u32 n)
{
    while (n) {
        if (b->lit) {
            u32 k = b->lit < n ? b->lit : n;
            if ((u32)(b->e - b->s) < k)
                return -1;
            copy(o, b->s, k);
            b->s += k; o += k; n -= k; b->lit -= k;
        } else if (b->run) {
            u32 k = b->run < n ? b->run : n;
            for (u32 i = 0; i < k; i++) o[i] = b->rb;
            o += k; n -= k; b->run -= k;
        } else {
            if (b->s >= b->e)
                return -1;
            u8 c = *b->s++;
            if (c < 128)
                b->lit = (u32)c + 1;
            else if (c > 128) {
                if (b->s >= b->e)
                    return -1;
                b->run = 257u - c;
                b->rb = *b->s++;
            }                                   /* 128: nothing */
        }
    }
    return 0;
}

static u8 maskrow[PIC_MAXW / 8 + 2];

static const char *ilbm(const u8 *f, u32 n, struct pic *p, void *(*alloc)(unsigned), void (*release)(void *), int nest)
{
    u32 len = be32(f + 4);
    if (len < 4)
        return "damaged (its FORM is empty)";
    if (len > n - 8)
        len = n - 8;                            /* a FORM longer than the file: what is there */
    const u8 *s = f + 12, *e = f + 8 + len, *bmhd = 0, *cmap = 0, *camg = 0, *body = 0, *inner = 0;
    u32 ncmap = 0, nbody = 0, ninner = 0;
    while (s + 8 <= e) {
        u32 cl = be32(s + 4);
        const u8 *d = s + 8;
        if (cl > (u32)(e - d))
            cl = (u32)(e - d);
        if (id(s, "BMHD") && cl >= 20) bmhd = d;
        else if (id(s, "CMAP")) { cmap = d; ncmap = cl; }
        else if (id(s, "CAMG") && cl >= 4) camg = d;
        else if (id(s, "BODY")) { body = d; nbody = cl; }
        else if (id(s, "FORM") && cl >= 4 && id(d, "ILBM") && !inner) { inner = s; ninner = cl + 8; }
        if (cl & 1) cl++;
        if (cl > (u32)(e - d))
            break;
        s = d + cl;
    }
    if (!bmhd && inner && !nest)                /* FORM ILBM { ANNO, FORM ILBM { ... } } */
        return ilbm(inner, ninner, p, alloc, release, 1);
    if (!bmhd)
        return "damaged (no BMHD)";
    int w = (int)be16(bmhd), h = (int)be16(bmhd + 2), planes = bmhd[8], masking = bmhd[9], comp = bmhd[10];
    int xa = bmhd[14], ya = bmhd[15];
    if (!w || !h)
        return body ? "damaged (no size)" : "a palette, no picture";
    if (w > PIC_MAXW || h > PIC_MAXH)
        return "too big (4096 x 4096 at most)";
    if (!((planes >= 1 && planes <= 8) || planes == 24))
        return "not shown: its planes (1-8 or 24)";
    if (masking > 3)
        return "damaged (its masking)";
    if (comp > 1)
        return "not shown: its compression (none and ByteRun1)";
    if (!body)
        return "a palette, no picture (no BODY)";
    u32 rb = (u32)((w + 15) / 16) * 2, stride = rb * (u32)planes;
    if (stride * (u32)h > PIC_MAXBYTES)
        return "too big for memory (4 MB decoded)";
    zero(p, sizeof *p);
    p->w = w; p->h = h; p->depth = planes; p->rowbytes = rb; p->stride = stride; p->fmt = "ILBM";
    /* the palette */
    if (cmap && planes != 24) {
        u32 q = udiv(ncmap, 3);
        int nc = (int)(q > 256 ? 256 : q), four = nc > 0;
        copy(p->pal, cmap, (u32)nc * 3);
        for (int i = 0; i < nc * 3; i++)
            if (p->pal[i] & 0x0F) four = 0;
        if (four)                               /* a 4-bit CMAP (old painters): $F0 is white */
            for (int i = 0; i < nc * 3; i++) p->pal[i] |= (u8)(p->pal[i] >> 4);
        p->ncol = nc;
    }
    /* the mode: CAMG cleaned as the old readers did; the resolution from it only on the native monitors */
    u32 c = camg ? be32(camg) : 0;
    int have = camg != 0;
    if (have) {
        p->camg = c;
        if (c & 0xFFFF0000u) {
            if (!(c & 0x1000))
                have = 0;                       /* junk in the high word (DigiView) */
        } else
            c &= ~(u32)(0x1000 | 0x4000 | 0x0100 | 0x0002 | 0x2000);
    }
    u32 mon = c & 0xFFFF1000u;
    int native = have && (c >> 16) < 0x10;
    int std = have && (mon == 0 || mon == 0x11000 || mon == 0x21000 || mon == 0x71000 || mon == 0x91000 || mon == 0xA1000);
    int ham = 0, ehb = 0;
    if (native) {
        ham = (c & 0x800) != 0;
        ehb = (c & 0x80) != 0;
    }
    if (std) {
        if (c & 0x8000) p->flags |= PIC_HIRES;
        if (c & 0x20) p->flags |= PIC_SHRES;
        if (c & 0x4) p->flags |= PIC_LACE;
    } else {
        if (w >= 1280) p->flags |= PIC_SHRES;
        else if (w >= 640 || (xa && ya && xa * 10 < ya * 7)) p->flags |= PIC_HIRES;
        if (h >= 400 || (xa && ya && xa * 10 > ya * 14)) p->flags |= PIC_LACE;
    }
    if (!have) {
        p->flags |= PIC_NOCAMG;
        if (planes == 6 && cmap && p->ncol <= 16) ham = 1;        /* DigiView's HAM6 */
        else if (planes == 6 && cmap && p->ncol <= 32) ehb = 1;
    }
    if (planes == 24)
        p->flags |= PIC_RGB;
    else if (ham && (planes == 5 || planes == 6))
        p->flags |= PIC_HAM6;
    else if (ham && (planes == 7 || planes == 8))
        p->flags |= PIC_HAM8;
    else if (ehb && planes == 6)
        p->flags |= PIC_EHB;
    if (planes != 24 && !p->ncol)
        grey(p, (p->flags & PIC_HAM6) ? 16 : (p->flags & PIC_HAM8) ? 64 : 1 << (planes > 8 ? 8 : planes));
    if (p->flags & PIC_EHB) {                   /* the upper 32 are always the halves (3.1's datatype got it wrong) */
        for (int i = 0; i < 32; i++)
            for (int k = 0; k < 3; k++) p->pal[(32 + i) * 3 + k] = (u8)(p->pal[i * 3 + k] >> 1);
        p->ncol = 64;
    }
    /* the body */
    p->data = alloc(stride * (u32)h);
    if (!p->data)
        return "out of memory";
    zero(p->data, stride * (u32)h);
    struct br b = { body, body + nbody, 0, 0, 0 };
    int y;
    for (y = 0; y < h; y++) {
        u8 *row = p->data + (u32)y * stride;
        if (comp) {
            if (unpack(&b, row, stride) < 0 || (masking == 1 && unpack(&b, maskrow, rb) < 0))
                break;
        } else {
            u32 need = stride + (masking == 1 ? rb : 0);
            if ((u32)(b.e - b.s) < need)
                break;
            copy(row, b.s, stride);
            b.s += need;
        }
    }
    if (y < h) {
        if (!y) {
            release(p->data);
            p->data = 0;
            return "damaged (its BODY is cut short)";
        }
        zero(p->data + (u32)y * stride, stride * (u32)(h - y));    /* a partly unpacked row: colour 0 */
        p->flags |= PIC_CUT;
    }
    return 0;
}

/* ================================================================ GIF */
struct bits { const u8 *f; u32 n, pos, left, acc; int nb, end; };
static int getbyte(struct bits *g)
{
    if (g->end)
        return -1;
    if (!g->left) {
        if (g->pos >= g->n || !(g->left = g->f[g->pos++])) {
            g->end = 1;                         /* the terminator, or the file's end */
            return -1;
        }
    }
    if (g->pos >= g->n) {
        g->end = 1;
        return -1;
    }
    g->left--;
    return g->f[g->pos++];
}
static int getcode(struct bits *g, int size)
{
    while (g->nb < size) {
        int c = getbyte(g);
        if (c < 0)
            return -1;
        g->acc |= (u32)c << g->nb;
        g->nb += 8;
    }
    int v = (int)(g->acc & ((1u << size) - 1));
    g->acc >>= size;
    g->nb -= size;
    return v;
}

static const char *gif(const u8 *f, u32 n, struct pic *p, void *(*alloc)(unsigned), void (*release)(void *))
{
    if (n < 13)
        return "damaged (cut short in its header)";
    zero(p, sizeof *p);
    p->fmt = "GIF";
    u32 pos = 13, fl = f[10];
    if (fl & 0x80) {
        u32 sz = 2u << (fl & 7);
        if (pos + sz * 3 > n)
            return "damaged (cut short in its colours)";
        copy(p->pal, f + pos, sz * 3);
        p->ncol = (int)sz;
        pos += sz * 3;
    }
    for (;;) {
        if (pos >= n)
            return "damaged (no picture in it)";
        u8 b = f[pos++];
        if (b == 0x21) {                        /* an extension: its sub-blocks skipped */
            pos++;
            for (;;) {
                if (pos >= n)
                    return "damaged (cut short in an extension)";
                u32 l = f[pos++];
                if (!l)
                    break;
                pos += l;
            }
            continue;
        }
        if (b == 0x3B)
            return "no picture in it";
        if (b != 0x2C)
            return "damaged (an unknown block)";
        break;
    }
    if (pos + 10 > n)
        return "damaged (cut short in its image)";
    int w = (int)le16(f + pos + 4), h = (int)le16(f + pos + 6);
    u32 ifl = f[pos + 8];
    pos += 9;
    if (!w || !h)
        return "damaged (no size)";
    if (w > PIC_MAXW || h > PIC_MAXH)
        return "too big (4096 x 4096 at most)";
    if (ifl & 0x80) {
        u32 sz = 2u << (ifl & 7);
        if (pos + sz * 3 > n)
            return "damaged (cut short in its colours)";
        copy(p->pal, f + pos, sz * 3);
        p->ncol = (int)sz;
        pos += sz * 3;
    }
    if (!p->ncol)
        grey(p, 256);
    if (pos >= n)
        return "damaged (cut short in its image)";
    int mcs = f[pos++];
    if (mcs < 2 || mcs > 8)
        return "damaged (its code size)";
    if ((u32)w * (u32)h > PIC_MAXBYTES)
        return "too big for memory (4 MB decoded)";
    p->w = w; p->h = h; p->depth = 8; p->rowbytes = p->stride = (u32)w;
    p->flags = PIC_CHUNKY | PIC_SQUARE;
    u8 *tab = alloc(4096 * 2 + 4096 + 4097);
    if (!tab)
        return "out of memory";
    p->data = alloc((u32)w * (u32)h);
    if (!p->data) {
        release(tab);
        return "out of memory";
    }
    zero(p->data, (u32)w * (u32)h);
    u16 *prefix = (u16 *)tab;
    u8 *suffix = tab + 8192, *stack = tab + 8192 + 4096;
    struct bits g = { f, n, pos, 0, 0, 0, 0 };
    int clear = 1 << mcs, eoi = clear + 1, next = clear + 2, size = mcs + 1, old = -1, first = 0;
    u32 total = (u32)w * (u32)h, k = 0;
    int x = 0, y = 0, pass = 0, inter = (ifl & 0x40) != 0;
    static const u8 pstart[4] = { 0, 4, 2, 1 }, pstep[4] = { 8, 8, 4, 2 };
    const char *why = 0;
    while (k < total) {
        int code = getcode(&g, size);
        if (code < 0 || code == eoi)
            break;
        if (code == clear) {
            size = mcs + 1;
            next = clear + 2;
            old = -1;
            continue;
        }
        int sp = 0, in = code;
        if (old < 0) {
            if (code > clear) { why = "damaged (a code before its table)"; break; }
            stack[sp++] = (u8)code;
            first = code;
        } else {
            if (code > next || (code == next && next >= 4096)) { why = "damaged (a code past its table)"; break; }
            if (code == next) {                 /* the code being made: old's string and its first again */
                stack[sp++] = (u8)first;
                code = old;
            }
            while (code > eoi && sp < 4096) {
                stack[sp++] = suffix[code];
                code = prefix[code];
            }
            if (code >= clear) { why = "damaged (its table)"; break; }
            first = code;
            stack[sp++] = (u8)code;
            if (next < 4096) {
                prefix[next] = (u16)old;
                suffix[next] = (u8)first;
                next++;
                if (next == 1 << size && size < 12)
                    size++;
            }
        }
        old = in;
        while (sp && k < total) {               /* the string, first byte first */
            p->data[(u32)y * (u32)w + (u32)x] = stack[--sp];
            k++;
            if (++x == w) {
                x = 0;
                if (!inter)
                    y++;
                else {
                    y += pstep[pass];
                    while (y >= h && pass < 3) {
                        pass++;
                        y = pstart[pass];
                    }
                }
                if (y >= h)
                    y = h - 1;
            }
        }
    }
    release(tab);
    if (k < total) {
        if (!k || why) {
            release(p->data);
            p->data = 0;
            return why ? why : "damaged (no pixels)";
        }
        p->flags |= PIC_CUT;
    }
    return 0;
}

const char *pic_decode(const unsigned char *f, unsigned n, struct pic *p, void *(*alloc)(unsigned),
                       void (*release)(void *))
{
    zero(p, sizeof *p);
    const char *k = pic_kind(f, n);
    if (!k)
        return "not a picture show reads (IFF ILBM, GIF)";
    return k[0] == 'I' ? ilbm(f, n, p, alloc, release, 0) : gif(f, n, p, alloc, release);
}

/* ================================================================ rows */
static u32 bt[256][2];                  /* a plane's byte -> its 8 pixels as 0/1 bytes (two longs, in memory order) */
static void bt_init(void)
{
    if (bt[255][0])
        return;
    for (u32 b = 0; b < 256; b++)
        for (int k = 0; k < 8; k++)
            ((u8 *)bt[b])[k] = (u8)(b >> (7 - k) & 1);
}
/* planes p0 .. p0 + np - 1 of row (rb bytes each) -> idx: one byte a pixel (rb * 8 bytes, 4-byte aligned) */
HOT static void chunky(const u8 *row, u32 rb, int p0, int np, u32 *idx)
{
    for (u32 i = 0; i < rb * 2; i++)
        idx[i] = 0;
    for (int k = 0; k < np; k++) {
        const u8 *pl = row + (u32)(p0 + k) * rb;
        u32 *o = idx;
        for (u32 i = 0; i < rb; i++, o += 2) {
            const u32 *t = bt[pl[i]];
            o[0] |= t[0] << k;
            o[1] |= t[1] << k;
        }
    }
}

/* row y's colour numbers (not HAM, not 24 planes): a pointer to w bytes (the GIF's own row, or buf's, 4-byte aligned
 * room for w + 16) */
HOT static const u8 *index_row(const struct pic *p, int y, u8 *buf)
{
    const u8 *row = p->data + (u32)y * p->stride;
    if (p->flags & PIC_CHUNKY)
        return row;
    u32 *idx = (u32 *)(((unsigned long)buf + 3) & ~3ul);
    bt_init();
    chunky(row, p->rowbytes, 0, p->depth, idx);
    return (const u8 *)idx;
}

HOT void pic_row(const struct pic *p, int y, unsigned char *rgb)
{
    u32 w = (u32)p->w;
    u32 *idx = (u32 *)(((unsigned long)(rgb + 3 * w) + 3) & ~3ul);
    const u8 *row = p->data + (u32)y * p->stride, *ix;
    if (p->flags & PIC_CHUNKY)
        ix = row;
    else {
        bt_init();
        if (p->flags & PIC_RGB) {
            for (int g = 0; g < 3; g++) {
                chunky(row, p->rowbytes, g * 8, 8, idx);
                const u8 *c = (const u8 *)idx;
                for (u32 x = 0; x < w; x++) rgb[x * 3 + (u32)g] = c[x];
            }
            return;
        }
        chunky(row, p->rowbytes, 0, p->depth, idx);
        ix = (const u8 *)idx;
    }
    const u8 *pal = p->pal;
    if (p->flags & (PIC_HAM6 | PIC_HAM8)) {
        int h8 = (p->flags & PIC_HAM8) != 0;
        u32 r = pal[0], g = pal[1], b = pal[2];         /* every row starts from colour 0 */
        for (u32 x = 0; x < w; x++) {
            u32 c = ix[x], ctl = h8 ? c >> 6 : c >> 4 & 3, d = h8 ? c & 63 : c & 15;
            if (ctl == 0) { const u8 *q = pal + d + d + d; r = q[0]; g = q[1]; b = q[2]; }
            else if (h8) {                              /* HAM8: the top 6 bits of the gun, its low 2 kept */
                u32 v = d << 2;
                if (ctl == 1) b = v | (b & 3); else if (ctl == 2) r = v | (r & 3); else g = v | (g & 3);
            } else {
                u32 v = (d << 4) + d;
                if (ctl == 1) b = v; else if (ctl == 2) r = v; else g = v;
            }
            *rgb++ = (u8)r; *rgb++ = (u8)g; *rgb++ = (u8)b;
        }
        return;
    }
    for (u32 x = 0; x < w; x++) {
        u32 v = ix[x];
        const u8 *c = pal + v + v + v;
        *rgb++ = c[0]; *rgb++ = c[1]; *rgb++ = c[2];
    }
}

static int put(char *o, unsigned osz, int at, const char *s)
{
    while (*s && at + 1 < (int)osz) o[at++] = *s++;
    o[at] = 0;
    return at;
}
int pic_plane(const struct pic *p, int k)
{
    if (!(p->flags & PIC_HAM8) || p->depth < 2)
        return k;
    return k < 2 ? p->depth - 2 + k : k - 2;
}

void pic_mode(const struct pic *p, char *o, unsigned osz)
{
    int at = 0;
    if (!osz) return;
    o[0] = 0;
    if (p->flags & PIC_HAM6) at = put(o, osz, at, "HAM6");
    else if (p->flags & PIC_HAM8) at = put(o, osz, at, "HAM8");
    else if (p->flags & PIC_EHB) at = put(o, osz, at, "EHB");
    else if (p->flags & PIC_RGB) at = put(o, osz, at, "24-bit");
    else {
        char n[8];
        int c = p->flags & PIC_CHUNKY ? p->ncol : 1 << p->depth, k = 0;
        char d[4];
        int m = 0;
        do { int t = (int)udiv((u32)c, 10); d[m++] = (char)('0' + c - t * 10); c = t; } while (c && m < 3);
        while (m) n[k++] = d[--m];
        n[k] = 0;
        at = put(o, osz, at, n);
        at = put(o, osz, at, " colours");
    }
    if (p->flags & PIC_SHRES) at = put(o, osz, at, ", super-hires");
    else if (p->flags & PIC_HIRES) at = put(o, osz, at, ", hires");
    if (p->flags & PIC_LACE) at = put(o, osz, at, ", lace");
    if (p->flags & PIC_CUT) put(o, osz, at, ", cut short");
}

/* ================================================================ the pens and the renderer */
void pens_init(struct pens *ps, const unsigned short *pal12, int n)
{
    if (n > 16) n = 16;
    if (n < 1) n = 1;
    ps->n = n;
    for (int i = 0; i < n; i++) {
        ps->rgb[i][0] = (u8)((pal12[i] >> 8 & 15) * 17);
        ps->rgb[i][1] = (u8)((pal12[i] >> 4 & 15) * 17);
        ps->rgb[i][2] = (u8)((pal12[i] & 15) * 17);
    }
    for (u32 c = 0; c < 4096; c++) {           /* weighted by the eye: green most, blue least */
        int r = (int)(c >> 8) * 17, g = (int)(c >> 4 & 15) * 17, b = (int)(c & 15) * 17, best = 0;
        u32 bd = ~0u;
        for (int i = 0; i < n; i++) {
            int dr = r - ps->rgb[i][0], dg = g - ps->rgb[i][1], db = b - ps->rgb[i][2];
            u32 d = (u32)(3 * dr * dr + 4 * dg * dg + 2 * db * db);
            if (d < bd) { bd = d; best = i; }
        }
        ps->lut[c] = (u8)best;
    }
}

static u8 clampt[1024];                 /* clampt[384 + v]: v held to 0..255, v from -384 to 639 */
static const u32 spread[16] = {        /* a pen's bit k at bit 8k: eight pens shifted in make the planes' bytes */
    0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001, 0x00010100, 0x00010101,
    0x01000000, 0x01000001, 0x01000100, 0x01000101, 0x01010000, 0x01010001, 0x01010100, 0x01010101 };
/* the ordered dither's thresholds (Bayer 4 x 4), centred, about a quarter of a gun's range across */
static const signed char bayer[4][4] = { { -30, 2, -22, 10 }, { 18, -14, 26, -6 }, { -18, 14, -26, 6 },
                                         { 30, -2, 22, -10 } };

static u8 otab[16 * 256];               /* a colour number's pen at each of the pattern's places (pic_render) */
static void put8(u8 *row, unsigned bpr, int depth, int i, u32 acc)
{
    for (int k = 0; k < depth; k++, row += bpr, acc >>= 8)
        row[i] = (u8)acc;
}

HOT void pic_render(const struct pic *p, struct canvas *cv, const struct pens *ps, int dither,
                    unsigned char *rowbuf, short *work, unsigned short *xs)
{
    int cw = cv->w, ch = cv->h, last = -1, depth = cv->depth;
    unsigned bpr = cv->bpr;
    u32 rs = bpr * (u32)depth;
    if (!clampt[383 + 256 + 1]) {
        for (int v = -384; v < 640; v++)
            clampt[384 + v] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    const u8 *ct = clampt + 384, *lut = ps->lut;
    for (int x = 0; x < cw; x++)
        xs[x] = (u16)(((u32)(2 * x + 1) * (u32)p->w) / (u32)(2 * cw));
    for (int i = 0; i < (cw + 2) * 3; i++)
        work[i] = 0;
    /* a picture of colour numbers, not Floyd-Steinberg: each number's pen for each of the 16 places of the pattern,
     * worked out once, so a pixel is one look-up. The pattern's strength is the colour's distance from its nearest
     * pen (at most 32, the pattern's full strength): a colour that is a pen stays that pen, one between two is mixed */
    int tab = dither != PIC_FS && !(p->flags & (PIC_HAM6 | PIC_HAM8 | PIC_RGB));
    if (tab) {
        int n = p->flags & PIC_CHUNKY ? 256 : 1 << p->depth;
        for (int i = 0; i < n; i++) {
            const u8 *c = p->pal + i + i + i;
            int pen = lut[(c[0] & 0xF0) << 4 | (c[1] & 0xF0) | c[2] >> 4], amp = 0;
            if (dither == PIC_ORDERED)
                for (int k = 0; k < 3; k++) {
                    int dd = c[k] - ps->rgb[pen][k];
                    if (dd < 0) dd = -dd;
                    if (dd > amp) amp = dd;
                }
            if (amp > 32) amp = 32;
            for (int t = 0; t < 16; t++) {
                int d = bayer[t >> 2][t & 3] * amp >> 5;   /* (a 68060 has no 64-bit divide) */
                otab[t << 8 | i] = lut[(ct[c[0] + d] & 0xF0) << 4 | (ct[c[1] + d] & 0xF0) | ct[c[2] + d] >> 4];
            }
        }
    }
    for (int y = 0; y < ch; y++) {
        int sy = (int)(((u32)(2 * y + 1) * (u32)p->h) / (u32)(2 * ch));
        u8 *row = cv->p + (u32)y * rs;
        u32 acc = 0;
        int x = 0;
        if (tab) {
            const u8 *ix = index_row(p, sy, rowbuf);
            const u8 *o0 = otab + ((y & 3) << 10), *o1 = o0 + 256, *o2 = o1 + 256, *o3 = o2 + 256;
            const u16 *xp = xs;
            for (; x + 8 <= cw; x += 8, xp += 8) {
                acc = spread[o0[ix[xp[0]]]] << 7 | spread[o1[ix[xp[1]]]] << 6 | spread[o2[ix[xp[2]]]] << 5 |
                      spread[o3[ix[xp[3]]]] << 4 | spread[o0[ix[xp[4]]]] << 3 | spread[o1[ix[xp[5]]]] << 2 |
                      spread[o2[ix[xp[6]]]] << 1 | spread[o3[ix[xp[7]]]];
                put8(row, bpr, depth, x >> 3, acc);
            }
            acc = 0;
            for (; x < cw; x++)
                acc = acc << 1 | spread[otab[((y & 3) << 10 | (x & 3) << 8) + ix[xs[x]]]];
        } else if (sy != last) {
            pic_row(p, sy, rowbuf);
            last = sy;
        }
        if (tab)
            ;
        else if (dither == PIC_FS) {
            short *e = work;
            int cr = 0, cg = 0, cb = 0, pr = 0, pg = 0, pb = 0, qr = 0, qg = 0, qb = 0;
            for (; x < cw; x++, e += 3) {
                const u8 *s = rowbuf + xs[x] + xs[x] + xs[x];
                int r = ct[s[0] + ((e[0] + cr + 8) >> 4)], g = ct[s[1] + ((e[1] + cg + 8) >> 4)],
                    b = ct[s[2] + ((e[2] + cb + 8) >> 4)];
                int pen = lut[(r & 0xF0) << 4 | (g & 0xF0) | b >> 4];
                const u8 *c = ps->rgb[pen];
                int er = r - c[0], eg = g - c[1], eb = b - c[2];
                /* the shares in shifts and adds: a 68020's multiply takes 28 cycles or more */
                int r2 = er + er, g2 = eg + eg, b2 = eb + eb;
                if (x) { e[-3] = (short)(pr + r2 + er); e[-2] = (short)(pg + g2 + eg); e[-1] = (short)(pb + b2 + eb); }
                pr = qr + r2 + r2 + er; pg = qg + g2 + g2 + eg; pb = qb + b2 + b2 + eb;
                qr = er; qg = eg; qb = eb;
                cr = (int)((unsigned)er << 3) - er; cg = (int)((unsigned)eg << 3) - eg; cb = (int)((unsigned)eb << 3) - eb;
                acc = acc << 1 | spread[pen];
                if ((x & 7) == 7) { put8(row, bpr, depth, x >> 3, acc); acc = 0; }
            }
            e[-3] = (short)pr; e[-2] = (short)pg; e[-1] = (short)pb;
        } else {
            const signed char *t = dither == PIC_ORDERED ? bayer[y & 3] : (const signed char *)"\0\0\0\0";
            for (; x + 8 <= cw; x += 8) {
                for (int k = 0; k < 8; k++) {
                    u32 sx = xs[x + k];
                    const u8 *s = rowbuf + sx + sx + sx;
                    int d = t[k & 3];
                    acc = acc << 1 | spread[lut[(ct[s[0] + d] & 0xF0) << 4 | (ct[s[1] + d] & 0xF0) | ct[s[2] + d] >> 4]];
                }
                put8(row, bpr, depth, x >> 3, acc);
                acc = 0;
            }
            for (; x < cw; x++) {
                u32 sx = xs[x];
                const u8 *s = rowbuf + sx + sx + sx;
                int d = t[x & 3];
                acc = acc << 1 | spread[lut[(ct[s[0] + d] & 0xF0) << 4 | (ct[s[1] + d] & 0xF0) | ct[s[2] + d] >> 4]];
            }
        }
        int i = cw >> 3;
        if (cw & 7) {                           /* the last byte's pixels to its left, the rest 0 */
            put8(row, bpr, depth, i, acc << (8 - (cw & 7)));
            i++;
        }
        for (; i < (int)bpr; i++)               /* the row's padding */
            put8(row, bpr, depth, i, 0);
    }
}
