/* SPDX-License-Identifier: GPL-2.0-only */
/* The AmigaOS bitmap font reader (amifont.h; the format in docs/d38-desktop.md "Fonts", from the RKM Libraries'
 * "Graphics Library and Text" and the includes' libraries/diskfont.h and graphics/text.h).
 *
 * A font file is an AmigaDOS executable: HUNK_HEADER, one HUNK_CODE (or HUNK_DATA) and its HUNK_RELOC32, HUNK_END.
 * The hunk holds "moveq #n,d0; rts", the DiskFontHeader (its node, DFH_ID 0x0f80, the name) and at offset 58 the
 * TextFont, whose pointers (tf_CharData, tf_CharLoc, tf_CharSpace, tf_CharKern; a ColorTextFont's planes) are
 * offsets in the hunk that HUNK_RELOC32 relocates. Read here without loading: a pointer must be one the relocation
 * table names (an absolute address is refused), and is then an offset into the hunk. */
#include "amifont.h"

#define HUNK_NAME       0x3E8
#define HUNK_CODE       0x3E9
#define HUNK_DATA       0x3EA
#define HUNK_RELOC32    0x3EC
#define HUNK_SYMBOL     0x3F0
#define HUNK_DEBUG      0x3F1
#define HUNK_END        0x3F2
#define HUNK_HEADER     0x3F3
#define HUNK_DREL32     0x3F7   /* V37's short form under its first number */
#define HUNK_RELOC32SHORT 0x3FC

#define DFH_ID          0x0f80
#define TF              58      /* the TextFont in the hunk (4 bytes of code, the DiskFontHeader's 54) */
#define TF_END          (TF + 52)       /* struct TextFont's 52 bytes */
#define CTF_END         (TF + 96)       /* struct ColorTextFont's */
#define FPF_REVPATH     0x04
#define FPF_PROPORTIONAL 0x20
#define FSF_COLORFONT   0x40

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

/* ---- the contents file: FontContentsHeader (FCH_ID 0x0f00, TFCH_ID 0x0f02, an outline font's 0x0f03), then entries
 * of 260 bytes: the file name (256 bytes; a TFontContents' last 2 are its tag count, the tags at the name's end), the
 * size (UWORD), the style and the flags (UBYTEs) ---- */
static int name_ok(const char *s)
{
    if (!*s || *s == '/')
        return 0;
    for (const char *p = s; *p; p++)
        if ((u8)*p < 0x20 || (u8)*p > 0x7E || *p == ':' || *p == '\\')
            return 0;
    for (const char *c = s; *c;) {                      /* no "." or ".." part, no empty one */
        const char *e = c;
        while (*e && *e != '/')
            e++;
        if (e == c || (e - c == 1 && c[0] == '.') || (e - c == 2 && c[0] == '.' && c[1] == '.'))
            return 0;
        c = *e ? e + 1 : e;
    }
    return 1;
}
static void put_num(char *b, unsigned sz, unsigned v)
{
    unsigned n = 0;
    while (b[n])
        n++;
    char d[8];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 8);
    if (n && n + 1 < sz)
        b[n++] = ' ';
    while (k && n + 1 < sz)
        b[n++] = d[--k];
    b[n] = 0;
}

const char *af_contents(const u8 *d, unsigned n, int ysize, char *file, unsigned filesz, char *sizes, unsigned sizessz)
{
    if (sizes && sizessz)
        sizes[0] = 0;
    if (n < 4)
        return "too short for a font contents file";
    u16 id = be16(d), num = be16(d + 2);
    if (id != 0x0f00 && id != 0x0f02 && id != 0x0f03)
        return "not a font contents file (no FCH_ID)";
    if (!num)
        return "it lists no sizes";
    if (n > AF_MAXCONTENTS || num > 64)
        return "it lists more than 64 sizes";
    if (4u + 260u * num > n)
        return "cut short (fewer entries than it says)";
    int best = -1;
    for (int i = 0; i < num; i++) {
        const u8 *e = d + 4 + 260 * i;
        unsigned room = 256;
        if (id != 0x0f00) {
            unsigned tags = be16(e + 254);
            if (tags * 8u > 254)
                return "an entry's tags are longer than its name field";
            room = 254 - tags * 8;
        }
        unsigned k = 0;
        while (k < room && e[k])
            k++;
        if (k == room)
            return "an entry's file name has no end";
        u16 ys = be16(e + 256);
        if (sizes && sizessz)
            put_num(sizes, sizessz, ys);
        if (ys != ysize)
            continue;
        if (best < 0 || (e[258] == 0 && d[4 + 260 * best + 258] != 0))
            best = i;
    }
    if (best < 0)
        return "no such size";
    const u8 *e = d + 4 + 260 * best;
    unsigned k = 0;
    while (e[k])
        k++;
    if (k + 1 > filesz)
        return "its file name is too long";
    for (unsigned i = 0; i <= k; i++)
        file[i] = (char)e[i];
    if (!name_ok(file))
        return "its file name is not a plain relative path";
    return 0;
}

/* ---- the font file ---- */
#define P_DATA  1               /* the pointers that must be relocated when not 0 (bits of a mask) */
#define P_LOC   2
#define P_SPACE 4
#define P_KERN  8
#define P_PLANE 16              /* << plane: a ColorTextFont's ctf_CharData[0..7] */
static int ptr_bit(u32 off)
{
    switch (off) {
    case TF + 34: return P_DATA;
    case TF + 40: return P_LOC;
    case TF + 44: return P_SPACE;
    case TF + 48: return P_KERN;
    }
    if (off >= TF + 64 && off < TF + 96 && !((off - TF - 64) & 3))
        return P_PLANE << ((off - TF - 64) / 4);
    return 0;
}

const char *af_load(const u8 *d, unsigned n, struct af_font *f, void *(*alloc)(unsigned))
{
    u8 *zero = (u8 *)f;
    for (unsigned i = 0; i < sizeof *f; i++)
        zero[i] = 0;
    if (n > AF_MAXFILE)
        return "larger than 256 KB";
    n &= ~3u;
    if (n < 24 || be32(d) != HUNK_HEADER)
        return "not an AmigaDOS executable (no HUNK_HEADER)";
    u32 nl = n / 4;
#define L(i) be32(d + 4 * (i))
    u32 i = 1;
    for (;;) {                                          /* resident library names: none in a font, skipped */
        if (i >= nl)
            return "cut short in its header";
        u32 c = L(i++);
        if (!c)
            break;
        if (c > nl - i)
            return "cut short in its header";
        i += c;
    }
    if (i + 3 > nl)
        return "cut short in its header";
    u32 tab = L(i), first = L(i + 1), last = L(i + 2);
    i += 3;
    if (tab != 1 || first != 0 || last != 0)
        return "more than one hunk: not a font";
    if (i >= nl)
        return "cut short in its header";
    if ((L(i++) >> 30) == 3)                            /* memory flags 3: an extra word of attributes */
        i++;
    const u8 *h = 0;
    u32 hl = 0, relocated = 0, ends = 0;
    while (i < nl && !ends) {
        u32 t = L(i++) & 0x3FFFFFFF;
        switch (t) {
        case HUNK_NAME:
        case HUNK_DEBUG: {
            if (i >= nl || L(i) > nl - i - 1)
                return "cut short in a hunk";
            i += 1 + L(i);
            break;
        }
        case HUNK_CODE:
        case HUNK_DATA: {
            if (h)
                return "more than one hunk: not a font";
            if (i >= nl || L(i) > nl - i - 1)
                return "cut short in its hunk";
            hl = L(i) * 4;
            h = d + 4 * (i + 1);
            i += 1 + L(i);
            break;
        }
        case HUNK_RELOC32:
            if (!h)
                return "a relocation before its hunk";
            for (;;) {
                if (i >= nl)
                    return "cut short in a relocation";
                u32 c = L(i++);
                if (!c)
                    break;
                if (i >= nl || c > nl - i - 1)
                    return "cut short in a relocation";
                if (L(i++) != 0)
                    return "a relocation to another hunk";
                for (u32 k = 0; k < c; k++, i++) {
                    u32 o = L(i);
                    if (hl < 4 || o > hl - 4)
                        return "a relocation outside its hunk";
                    relocated |= (u32)ptr_bit(o);
                }
            }
            break;
        case HUNK_RELOC32SHORT:
        case HUNK_DREL32: {
            if (!h)
                return "a relocation before its hunk";
            u32 w = i * 2, nw = nl * 2;                 /* in 16-bit words */
#define W(j) be16(d + 2 * (j))
            for (;;) {
                if (w >= nw)
                    return "cut short in a relocation";
                u32 c = W(w++);
                if (!c)
                    break;
                if (w >= nw || c > nw - w - 1)
                    return "cut short in a relocation";
                if (W(w++) != 0)
                    return "a relocation to another hunk";
                for (u32 k = 0; k < c; k++, w++) {
                    u32 o = W(w);
                    if (hl < 4 || o > hl - 4)
                        return "a relocation outside its hunk";
                    relocated |= (u32)ptr_bit(o);
                }
            }
#undef W
            i = (w + 1) / 2;                            /* padded to a long word */
            break;
        }
        case HUNK_SYMBOL:
            for (;;) {
                if (i >= nl)
                    return "cut short in its symbols";
                u32 c = L(i++) & 0xFFFFFF;
                if (!c)
                    break;
                if (c + 1 > nl - i)
                    return "cut short in its symbols";
                i += c + 1;
            }
            break;
        case HUNK_END:
            ends = 1;
            break;
        default:
            return "a hunk type a font does not have";
        }
    }
#undef L
    if (!h)
        return "no code or data hunk";
    if (hl < TF_END)
        return "its hunk is too short for a TextFont";
    if (be16(h + 18) != DFH_ID)
        return "not a font (no DFH_ID)";
    const u8 *t = h + TF;
    u32 ys = be16(t + 20), xs = be16(t + 24), base = be16(t + 26);
    u8 style = t[22], flags = t[23], lo = t[32], hi = t[33];
    if (!ys || ys > AF_MAXH)
        return ys ? "taller than 64 lines" : "a height of 0";
    if (flags & FPF_REVPATH)
        return "a right-to-left font (FPF_REVPATH): not read";
    if (lo > hi)
        return "its first character is after its last";
    if (xs > 255)
        return "wider than 255 pixels";
    u32 nent = (u32)(hi - lo) + 2;                      /* the characters and the default glyph after them */
    u32 ptr[4], need[4] = { 0, nent * 4, nent * 2, nent * 2 };
    static const int bits[4] = { P_DATA, P_LOC, P_SPACE, P_KERN };
    u32 mod = be16(t + 38);
    need[0] = mod * ys;
    for (int k = 0; k < 4; k++) {
        ptr[k] = be32(t + (k == 0 ? 34 : 36 + 4 * k));
        if (!ptr[k])
            continue;
        if (!(relocated & (u32)bits[k]))
            return "a pointer that is not relocated (an absolute address)";
        if (ptr[k] > hl || need[k] > hl - ptr[k] || (k && (ptr[k] & 1)))
            return "a table outside its hunk";
    }
    if (!ptr[1])
        return "no character locations (tf_CharLoc)";
    /* the strikes: tf_CharData, or a ColorTextFont's planes (read as one: a pixel set in any plane is drawn) */
    const u8 *plane[8];
    int np = 0;
    if (style & FSF_COLORFONT) {
        if (hl < CTF_END)
            return "its hunk is too short for a ColorTextFont";
        int depth = t[54];
        if (depth < 1 || depth > 8)
            return "a colour font of a depth not 1 to 8";
        for (int k = 0; k < depth; k++) {
            u32 o = be32(t + 64 + 4 * k);
            if (!o)
                continue;
            if (!(relocated & ((u32)P_PLANE << k)))
                return "a pointer that is not relocated (an absolute address)";
            if (o > hl || need[0] > hl - o)
                return "a plane outside its hunk";
            plane[np++] = h + o;
        }
        if (!np)
            return "a colour font with no planes";
    } else {
        if (!ptr[0])
            return "no character data (tf_CharData)";
        plane[np++] = h + ptr[0];
    }
    if (!mod)
        return "a row of 0 bytes (tf_Modulo)";
    f->h = (u16)ys;
    f->base = (u16)(base < ys ? base : ys - 1);         /* the RKM's rule: at most tf_YSize - 1 */
    f->xsize = (u16)xs;
    f->style = style;
    f->flags = flags;
    f->lo = lo;
    f->hi = hi;
    f->prop = (flags & FPF_PROPORTIONAL) && ptr[2];
    f->colour = (style & FSF_COLORFONT) != 0;
    u32 total = 0;
    for (u32 e = 0; e < nent; e++) {                    /* every entry checked, the default glyph's too */
        const u8 *l = h + ptr[1] + 4 * e;
        u32 off = be16(l), w = be16(l + 2);
        if (w > AF_MAXW)
            return "a glyph wider than 64 pixels";
        if (off + w > mod * 8)
            return "a glyph outside its row";
        int kern = ptr[3] ? (short)be16(h + ptr[3] + 2 * e) : 0;
        int space = ptr[2] ? (short)be16(h + ptr[2] + 2 * e) : (int)xs;
        if (kern < -64 || kern > 64)
            return "a kerning past 64 pixels";
        int adv = f->prop ? space + kern : (int)xs;
        if (adv < 0 || adv > 255)
            return "a glyph's width is negative or past 255";
        if (e == nent - 1)
            break;                                      /* the default glyph: the 8 x 8 stands in instead */
        u32 c = lo + e;
        if (c < 32 || (c >= 0x7F && c < 0xA0) || (!w && !adv))
            continue;                                   /* control codes, and an empty slot: not the font's */
        struct af_glyph *g = &f->g[c];
        g->w = (u8)w;
        g->bpr = (u8)((w + 7) / 8);
        g->kern = (signed char)kern;
        g->adv = (u8)adv;
        g->have = 1;
        g->off = (u16)total;                            /* checked below before any use */
        total += g->bpr * ys;
        if (total > AF_MAXBYTES)
            return "its glyphs take more than 64 KB";
        f->nglyphs++;
    }
    f->nbytes = total;
    if (!alloc)
        return 0;                                       /* checked only (ZINE's diskfont loads the file itself) */
    f->bits = total ? alloc(total) : 0;
    if (total && !f->bits)
        return "no memory for its glyphs";
    for (u32 c = 0; c < 256; c++) {
        struct af_glyph *g = &f->g[c];
        if (!g->have || !g->bpr)
            continue;
        u32 off = be16(h + ptr[1] + 4 * (c - lo));
        u8 *o = f->bits + g->off;
        for (u32 y = 0; y < ys; y++, o += g->bpr) {
            for (u32 b = 0; b < g->bpr; b++)
                o[b] = 0;
            for (u32 x = 0; x < g->w; x++) {
                u32 bit = off + x, set = 0;
                for (int k = 0; k < np; k++)
                    set |= plane[k][y * mod + (bit >> 3)] >> (7 - (bit & 7)) & 1;
                if (set)
                    o[x >> 3] |= (u8)(0x80 >> (x & 7));
            }
        }
    }
    return 0;
}
