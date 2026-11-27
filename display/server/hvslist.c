/* SPDX-License-Identifier: GPL-2.0-only */
/* The HVS's words, the EDID and the mode list (DESIGN D63, docs/d63-pistorm-gfx.md 5-7; hvslist.h). Read for the
 * facts from VideoCore.card's vc6.c (Michal Schulz) and Linux's vc4_regs.h; written here. Cold code: -Os. */
#include "hvslist.h"

typedef unsigned char u8;
typedef unsigned u32;

/* ---- the EDID (VESA E-EDID 1.4: block 0) ---- */
static void add_size(struct hvsedid *e, unsigned w, unsigned h)
{
    if (!w || !h || w > 8192 || h > 8192)
        return;
    for (int i = 0; i < e->n; i++)
        if (e->w[i] == w && e->h[i] == h)
            return;
    if (e->n < HVS_EDIDMAX) {
        e->w[e->n] = (unsigned short)w;
        e->h[e->n++] = (unsigned short)h;
    }
}
int hvs_edid(const u8 *b, unsigned n, struct hvsedid *e)
{
    static const u8 head[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    u8 *z = (u8 *)e;
    for (unsigned i = 0; i < sizeof *e; i++)
        z[i] = 0;
    e->pref = -1;
    if (n < 128)
        return -1;
    u8 sum = 0;
    for (int i = 0; i < 128; i++)
        sum = (u8)(sum + b[i]);
    for (int i = 0; i < 8; i++)
        if (b[i] != head[i])
            return -1;
    if (sum)
        return -1;
    unsigned id = (unsigned)b[8] << 8 | b[9];
    for (int i = 0; i < 3; i++) {
        unsigned c = id >> (10 - 5 * i) & 31;
        e->maker[i] = c >= 1 && c <= 26 ? (char)('A' + c - 1) : '?';
    }
    for (int d = 0; d < 4; d++) {       /* the four descriptors: timings, or a display descriptor (the name) */
        const u8 *p = b + 54 + 18 * d;
        if (p[0] | p[1]) {
            unsigned w = p[2] | (unsigned)(p[4] & 0xF0) << 4, h = p[5] | (unsigned)(p[7] & 0xF0) << 4;
            if (p[17] & 0x80)
                h *= 2;                 /* interlaced: the field's lines */
            int was = e->n;
            add_size(e, w, h);
            if (e->pref < 0 && d == 0 && e->n > was)
                e->pref = was;
        } else if (p[3] == 0xFC) {
            int k = 0;
            for (; k < 13 && p[5 + k] != 0x0A && p[5 + k] >= 0x20 && p[5 + k] < 0x7F; k++)
                e->name[k] = (char)p[5 + k];
            while (k > 0 && e->name[k - 1] == ' ')
                k--;
            e->name[k] = 0;
        }
    }
    static const unsigned short est[17][2] = {          /* bytes 35-37, bit 7 of 35 first */
        { 720, 400 }, { 720, 400 }, { 640, 480 }, { 640, 480 }, { 640, 480 }, { 640, 480 }, { 800, 600 }, { 800, 600 },
        { 800, 600 }, { 800, 600 }, { 832, 624 }, { 0, 0 }, { 1024, 768 }, { 1024, 768 }, { 1024, 768 },
        { 1280, 1024 }, { 1152, 870 } };
    for (int i = 0; i < 17; i++)
        if (b[35 + i / 8] >> (7 - i % 8) & 1)
            add_size(e, est[i][0], est[i][1]);
    for (int i = 0; i < 8; i++) {       /* the standard timings */
        u8 x = b[38 + 2 * i], a = b[39 + 2 * i];
        if ((x == 1 && a == 1) || !x)
            continue;
        unsigned w = (x + 31u) * 8, h;
        switch (a >> 6) {
        case 0: h = b[18] == 1 && b[19] < 3 ? w : w * 10 / 16; break;    /* 16:10 since EDID 1.3 */
        case 1: h = w * 3 / 4; break;
        case 2: h = w * 4 / 5; break;
        default: h = w * 9 / 16; break;
        }
        add_size(e, w, h);
    }
    return 0;
}

/* ---- the picture's size ---- */
unsigned hvs_bpr(unsigned w, unsigned bits) { return (w * (bits / 8) + 15) & ~15u; }
int hvs_format(unsigned bits) { return bits == 8 ? HVS_FMT8 : bits == 16 ? HVS_FMT16 : bits == 32 ? HVS_FMT32 : 0; }
int hvs_size_ok(unsigned w, unsigned h, unsigned bits, unsigned ow, unsigned oh, unsigned vc4)
{
    if (!hvs_format(bits) || w < HVS_MINW || h < HVS_MINH || w > ow || h > oh || w > 4096 || h > 4096)
        return 0;
    return vc4 > HVS_VC4_SPARE && hvs_bpr(w, bits) <= (vc4 - HVS_VC4_SPARE) / h;    /* (no 64-bit product) */
}

static const unsigned short common[][2] = {
    { 320, 256 }, { 640, 480 }, { 640, 512 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 960 },
    { 1280, 1024 }, { 1440, 900 }, { 1600, 1200 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 } };

static void put(struct hvsmode *m, int *n, int max, unsigned w, unsigned h, int flags, unsigned ow, unsigned oh,
                unsigned vc4)
{
    unsigned char bits = 0;
    for (unsigned k = 0; k < 3; k++)
        if (hvs_size_ok(w, h, 8u << k, ow, oh, vc4))
            bits |= (unsigned char)(1 << k);
    if (!bits)
        return;
    for (int i = 0; i < *n; i++)
        if (m[i].w == w && m[i].h == h) {
            m[i].flags |= (unsigned char)flags;
            return;
        }
    if (*n >= max)
        return;
    m[*n].w = (unsigned short)w;
    m[*n].h = (unsigned short)h;
    m[*n].bits = bits;
    m[(*n)++].flags = (unsigned char)flags;
}
int hvs_modes(unsigned ow, unsigned oh, unsigned vc4, const struct hvsedid *e, struct hvsmode *m, int max)
{
    int n = 0;
    if (max > HVS_MAXMODES)
        max = HVS_MAXMODES;
    put(m, &n, max, ow, oh, HVSM_NATIVE, ow, oh, vc4);
    if (e && e->pref >= 0)
        put(m, &n, max, e->w[e->pref], e->h[e->pref], HVSM_PREF, ow, oh, vc4);
    for (unsigned i = 0; i < sizeof common / sizeof common[0]; i++)
        put(m, &n, max, common[i][0], common[i][1], 0, ow, oh, vc4);
    for (int i = 0; e && i < e->n; i++)
        put(m, &n, max, e->w[i], e->h[i], 0, ow, oh, vc4);
    for (int i = 1; i < n; i++)         /* by area, then width */
        for (int j = i; j > 0; j--) {
            u32 a0 = (u32)m[j - 1].w * m[j - 1].h, a1 = (u32)m[j].w * m[j].h;
            if (a0 < a1 || (a0 == a1 && m[j - 1].w <= m[j].w))
                break;
            struct hvsmode t = m[j];
            m[j] = m[j - 1];
            m[j - 1] = t;
        }
    return n;
}

/* ---- the place and the scaling (VideoCore.card's rule: the larger factor that fits, centred) ---- */
void hvs_fit(unsigned w, unsigned h, unsigned ow, unsigned oh, struct hvsgeo *g)
{
    g->unity = w == ow && h == oh;
    if (g->unity) {
        g->scale = 0x10000;
        g->dw = w, g->dh = h, g->x = g->y = 0;
        return;
    }
    u32 sx = 0x10000u * w / ow, sy = 0x10000u * h / oh;
    g->scale = sx > sy ? sx : sy;
    if (!g->scale)
        g->scale = 1;
    g->dw = 0x10000u * w / g->scale;
    g->dh = 0x10000u * h / g->scale;
    if (g->dw > ow) g->dw = ow;
    if (g->dh > oh) g->dh = oh;
    g->x = (ow - g->dw) / 2;
    g->y = (oh - g->dh) / 2;
}

/* ---- the words (BCM2711) ---- */
#define CTL_VALID       (1u << 30)
#define CTL_WORDS(n)    (((u32)(n) & 0x3F) << 24)
#define CTL_UNITY       (1u << 15)
#define CTL_ORDER(o)    (((u32)(o) & 3) << 13)
#define CTL_ALPHA_EXP   (1u << 12)
#define CTL_RGB_EXP     (1u << 11)
#define POS_ALPHA_FIXED (1u << 30 | 0xFFF0u)    /* the alpha mode "fixed", fully opaque */
#define PPF(s)          ((u32)(s) << 8 | 0xC0000000u | 128u)
#define VC_ALIAS        0xC0000000u     /* the VideoCore's uncached view of the first 1 GB */
#define HVS_SCRATCH     0xDEADBEEFu     /* a word the HVS writes itself */

int hvs_list(const struct hvsplane *p, u32 *o)
{
    struct hvsgeo g;
    hvs_fit(p->w, p->h, p->ow, p->oh, &g);
    u32 ctl = (u32)(p->bits == 8 ? 13 | CTL_ORDER(3) : p->bits == 16 ? 4 | CTL_ORDER(2) : 7 | CTL_ORDER(2));
    int unity = g.unity && p->bits != 8, n = 1;     /* (a palette picture always through the scaler, as vc6.c) */
    o[n++] = (g.x & 0x3FFF) | (g.y & 0xFFF) << 16;
    o[n++] = POS_ALPHA_FIXED;
    if (!unity)
        o[n++] = g.dh << 16 | g.dw;
    o[n++] = p->h << 16 | p->w;
    o[n++] = HVS_SCRATCH;
    o[n++] = VC_ALIAS | p->addr;
    o[n++] = HVS_SCRATCH;
    o[n++] = p->bpr;
    if (!unity) {
        if (p->bits == 8)
            o[n++] = VC_ALIAS | HVS_DL_PAL << 2;
        o[n++] = 0;                     /* the line buffer */
        o[n++] = PPF(g.scale);
        o[n++] = PPF(g.scale);
        o[n++] = 0;
        for (int k = 0; k < 4; k++)
            o[n++] = HVS_DL_KERNEL;
    }
    o[0] = CTL_VALID | CTL_WORDS(n) | (unity ? CTL_UNITY : 0) | CTL_ALPHA_EXP | CTL_RGB_EXP | ctl;
    o[n++] = HVS_DL_END;
    return n;
}

void hvs_kernel(u32 out[11])
{
    u32 half[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 16; i++)        /* 0 for the first half of the phases, 255 for the second */
        half[i / 3] |= (i < 8 ? 0u : 255u) << (9 * (i % 3));
    half[5] |= half[5] << 9;
    for (int i = 0; i < 11; i++)
        out[i] = half[i < 6 ? i : 10 - i];
}

unsigned hvs_argb(unsigned c)
{
    return 0xFF000000u | ((c >> 8 & 15) * 17) << 16 | ((c >> 4 & 15) * 17) << 8 | (c & 15) * 17;
}

/* ---- text ---- */
static int number(const char **s, unsigned *v)
{
    const char *p = *s;
    unsigned base = 10, x = 0;
    int k = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        base = 16, p += 2;
    for (;; p++, k++) {
        unsigned d = *p >= '0' && *p <= '9' ? (unsigned)(*p - '0') : base == 16 && *p >= 'a' && *p <= 'f' ?
                     (unsigned)(*p - 'a' + 10) : base == 16 && *p >= 'A' && *p <= 'F' ? (unsigned)(*p - 'A' + 10) : 99;
        if (d >= base)
            break;
        if (x > (0xFFFFFFFFu - d) / base)
            return -1;
        x = x * base + d;
    }
    if (!k)
        return -1;
    *v = x;
    *s = p;
    return 0;
}
int hvs_parse_size(const char *s, unsigned *w, unsigned *h, unsigned *bits)
{
    unsigned b = 0;
    if (number(&s, w) || *s++ != 'x' || number(&s, h))
        return -1;
    if (*s == 'x') {
        s++;
        if (number(&s, &b) || !hvs_format(b))
            return -1;
    }
    if (*s)
        return -1;
    *bits = b;
    return 0;
}
/* ---- the Pi ---- */
#include "../../kernel/hwinfo.h"
unsigned hvs_rd(unsigned long a)
{
    unsigned v = __builtin_bswap32(*(volatile unsigned *)a);
#ifdef __m68k__
    __asm__ volatile("nop" ::: "memory");       /* Emu68 turns a nop into a barrier */
#endif
    return v;
}
void hvs_wr(unsigned long a, unsigned v)
{
    *(volatile unsigned *)a = __builtin_bswap32(v);
#ifdef __m68k__
    __asm__ volatile("nop" ::: "memory");
#endif
}
const char *hvs_find(const void *hwinfo, const void *hwdisp, struct hvspi *p)
{
    const struct hwinfo *h = hwinfo;
    const struct hwdisp *d = hwdisp;
    const char *no = h->magic != HWINFO_MAGIC ? "no hardware table" : !(h->flags & HW_DT) ? "no Emu68 device tree" :
                     !(h->flags & HW_BCM2711) ? "not a BCM2711 (Pi 4, CM4)" : !h->soc_base ? "no peripherals' address" :
                     !h->vc4_base || h->vc4_size < (1u << 20) ? "no vc4-mem" :
                     h->vc4_base + h->vc4_size > 0x40000000u || h->vc4_base + h->vc4_size < h->vc4_base ?
                     "vc4-mem is not below 1 GB" : !(d->flags & HWD_SIZE) ? "the boot did not learn the HDMI's size" :
                     d->w < HVS_MINW || d->h < HVS_MINH || d->w > 4096 || d->h > 4096 ? "the HDMI's size makes no sense" : 0;
    if (no)
        return no;
    p->soc = h->soc_base;
    p->hvs = h->soc_base + HVS_OFF;
    p->vc4 = h->vc4_base;
    p->vc4size = h->vc4_size;
    p->ow = d->w;
    p->oh = d->h;
    p->edid = d->flags & HWD_EDID ? d->edid : 0;
    return 0;
}
int hvs_running(const struct hvspi *p) { return (hvs_rd(p->hvs + HVS_DISPSTAT1) >> 30) >= 2; }
int hvs_put(const struct hvspi *p, const struct hvsplane *pl, unsigned at)
{
    unsigned o[40], k[11];
    int n = hvs_list(pl, o), bad = 0;
    hvs_kernel(k);
    hvs_wr(hvs_dl(p, HVS_DL_BLANK), HVS_DL_END);
    for (unsigned i = 0; i < 11; i++)
        hvs_wr(hvs_dl(p, HVS_DL_KERNEL + i), k[i]);
    for (int i = 0; i < n; i++)
        hvs_wr(hvs_dl(p, at + (unsigned)i), o[i]);
    bad |= hvs_rd(hvs_dl(p, HVS_DL_BLANK)) != HVS_DL_END;
    for (unsigned i = 0; i < 11; i++)
        bad |= hvs_rd(hvs_dl(p, HVS_DL_KERNEL + i)) != k[i];
    for (int i = 0; i < n; i++)
        bad |= hvs_rd(hvs_dl(p, at + (unsigned)i)) != o[i];
    return bad ? -1 : 0;
}
unsigned hvs_con(const struct hvspi *p)
{
    return hvs_rd(hvs_dl(p, HVS_DL_CONMAGIC)) == HVS_CON_MAGIC ? hvs_rd(hvs_dl(p, HVS_DL_CON)) : 0;
}
void hvs_con_keep(const struct hvspi *p, unsigned v)
{
    if (!v || hvs_ours(v) || v >= HVS_DLIST_WORDS)
        return;
    hvs_wr(hvs_dl(p, HVS_DL_CON), v);
    hvs_wr(hvs_dl(p, HVS_DL_CONMAGIC), HVS_CON_MAGIC);
}
