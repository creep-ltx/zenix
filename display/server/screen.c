/* SPDX-License-Identifier: GPL-2.0-only */
/* displayd's screens of their own (DESIGN D55, docs/d55-screens.md): a client's own mode, palette and planes (its
 * chip RAM, held by the kernel while shown: CON_HOLD), and a Copper list of Zenix's own instructions that is checked
 * here and translated into Copper words in displayd's chip RAM. Shown by pointing the changing part's jump word at a
 * screen buffer's trampoline (cop_show): still one word. Two buffers a screen; colours change in place (a colour word
 * can do no harm), pointers and the list's shape only by a flip after cop_settle. */
#include "dpyd.h"

#ifndef DIW_V
#define DIW_V 0x2C
#endif
#ifndef F3L_DDFSTOP
#define F3L_DDFSTOP 0xB0                /* lores at the 64-bit fetch (section 7: the screen test measures it) */
#endif
#define NSCR 4
#define SCR_BUDGET (160u << 10)         /* the lists' chip RAM, all screens */
#define PERLINE 14                      /* moves the Copper is sure to make in a line, any mode (section 5) */
#define VBLINE 40                       /* ... in a line of the vertical blank */
#define MAXMOVES (4096 - 64 - 3)        /* the kernel's contract less displayd's fixed part and a trampoline */

struct ptr { u16 at; u16 pad; s32 rel0, rel1; };       /* a pointer's high word in the list; its offsets from off */
struct scrn {
    int used, cl, slot;
    u16 w, h, depth, flags, bpr, ncop, ncol, npal, fetch, slace, hires;
    s16 mod0;                           /* the default modulo: bpr - fetch */
    u32 planes, bytes, pstride, off;
    s32 lo, hi;                         /* the whole list's fetch, relative to off: [lo, hi) */
    u32 pal[256];
    struct dpycop *list, *spare;        /* the instructions, nlist of them; a new list is checked in spare */
    int nlist;
    u16 *buf[2][2];                     /* [buffer][field]: the lists (chip RAM) */
    u32 room;                           /* each list's words */
    u32 chip;                           /* their bytes, for the budget */
    void *chipmem;                      /* the one region all of it is in (this record, the lists, the arrays): no
                                         * growth of displayd's heap, whose regions its images need too */
    u32 gen, bgen[2];                   /* the shape's version, each buffer's */
    int cur;                            /* the buffer that is complete and current (the one shown in front) */
    u16 *tramp[2][2];
    struct ptr *ptrs;                   /* depth + the CO_PLANEs */
    int nptr;
    u16 (*colat)[2];                    /* each instruction's colour words (hi, lo; 0: not a colour) */
    u16 *cidx;                          /* the CO_COLOURs' instruction indexes, in order (SS_COLOURS' fast path) */
    int ncidx;
    u16 palat[256][2];
};
static struct scrn *scrs[NSCR];         /* allocated while open: no memory taken for screens never opened */
static int front = -1, last = -1;       /* the screen in front; the one in front last (Left Amiga+N brings it back) */
static u32 chip_lists;
static int nofocus = 1;                 /* the console is not shown (GEV_RELEASE) */

int scr_shown(void) { return front >= 0; }

static struct scrn *of(int cl)
{
    for (int i = 0; i < NSCR; i++)
        if (scrs[i] && scrs[i]->cl == cl)
            return scrs[i];
    return 0;
}

static void tell(struct scrn *s, int type, int code, int arg, int x, int y, u16 qual, const u8 *ch)
{
    struct dpyev e;
    memset(&e, 0, sizeof e);
    e.type = (u8)type;
    e.code = (u8)code;
    e.arg = (u16)arg;
    e.qual = qual;
    e.x = (s16)x;
    e.y = (s16)y;
    if (ch)
        memcpy(e.ch, ch, 4);
    ev_to(s->cl, &e);
}

/* ---- the frame from which a change shows: count, beam, count again (section 4) ---- */
static int when(void)
{
    u32 c1, c2;
    int b;
    do {
        c1 = (u32)sys_vblank(CONV_ASK);
        b = dpy_beam() + DIW_V;
        c2 = (u32)sys_vblank(CONV_ASK);
    } while (c1 != c2);
    if ((int)c1 < 0)
        return 0;
    return (int)((c1 + (b < 16 ? 2u : 1u)) & 0x7FFFFFFFu);
}

/* ---- the chips' facts for a screen (section 7) ---- */
static int ddf_start(const struct scrn *s)
{
    int sc = s->flags & DPYSF_SCROLL;
    if (aga)
        return s->hires ? (sc ? 0x28 : 0x38) : (sc ? 0x18 : 0x38);
    return s->hires ? (sc ? 0x38 : 0x3C) : (sc ? 0x30 : 0x38);
}
static int ddf_stop(const struct scrn *s) { return aga ? (s->hires ? 0xC0 : F3L_DDFSTOP) : (s->hires ? 0xD4 : 0xD0); }
static u16 con0(const struct scrn *s)
{
    u16 v = (u16)(0x0200 | (s->hires ? 0x8000 : 0) | (s->flags & DPYSF_HAM ? 0x0800 : 0) | (s->slace ? 4 : 0));
    return s->depth == 8 ? (u16)(v | 0x0010) : (u16)(v | (u16)(s->depth << 12));
}
static u16 rgb12(u32 c) { return (u16)((c >> 12 & 0xF00) | (c >> 8 & 0x0F0) | (c >> 4 & 0x00F)); }
static u16 rgblo(u32 c) { return (u16)((c >> 8 & 0xF00) | (c >> 4 & 0x0F0) | (c & 0x00F)); }
/* the hardware modulo for a client's modulo m (interlaced: each field passes over the other's line) */
static s32 hwmod(const struct scrn *s, s32 m) { return s->slace ? s->fetch + 2 * m : m; }
static int hold_ok(s32 v) { return v >= -32768 && v <= 32767; }
static u32 align(void) { return aga ? 7u : 1u; }

/* ---- the check (section 5): the list's lines, its Copper time, every plane's fetch; also its words' count ---- */
struct chk {
    s32 p0[8], p1[8];                   /* each plane's pointer at its segment's start, relative to off (fields 0, 1) */
    int s[8];                           /* the segment's first frame line */
    s32 m[2];                           /* the modulos (client's values): odd planes (plane 0, 2...), even */
    s32 lo, hi;
};
static void seg(const struct scrn *s, struct chk *k, int pl, int to)
{
    int n = to - k->s[pl];
    if (n <= 0)
        return;
    s32 mod = k->m[pl & 1], a = s->slace ? 2 * (s->fetch + mod) : s->fetch + mod;
    for (int f = 0; f <= s->slace; f++) {
        s32 p = f ? k->p1[pl] : k->p0[pl], e = (s32)(n - 1) * a;
        s32 lo = p + (e < 0 ? e : 0), hi = p + (e > 0 ? e : 0) + s->fetch;
        if (lo < k->lo) k->lo = lo;
        if (hi > k->hi) k->hi = hi;
        if (f) k->p1[pl] = p + (s32)n * a; else k->p0[pl] = p + (s32)n * a;
    }
    k->s[pl] = to;
}
static int base_moves(const struct scrn *s);
static int ddf_start(const struct scrn *s);
static int hpos(const struct scrn *s, int b) { return b ? 0x40 + (s->hires ? b / 4 : b / 2) : 0; }
/* the moves the Copper is sure to make from a WAIT at (line, hp) until one at (L, hp2): what is left of its own line
 * before the fetch's end ($D9) and PERLINE for each whole line between (the next WAIT's pointers and modulos are
 * written from (L - 1, $D9), so the group must be done by then) */
static int budget(int line, int hp, int L, int hp2)
{
    if (L == line)
        return (hp2 - hp) / 8;
    int own = hp < 0xD9 ? (0xD9 - hp) / 8 : 0;
    return (own < PERLINE ? own : PERLINE) + PERLINE * (L - line - 1);
}
/* -> the translated list's instructions (moves and waits), or -1 with *bad the instruction's index (-1: the whole) */
static int check(const struct scrn *s, const struct dpycop *l, int n, s32 *lo, s32 *hi, int *bad)
{
    struct chk k;
    int H = s->h >> s->slace, ncol = aga ? 256 : 32;
    k.lo = 0x7FFFFFFF;
    k.hi = -0x7FFFFFFF;
    k.m[0] = k.m[1] = s->mod0;
    for (int p = 0; p < s->depth; p++) {
        k.p0[p] = (s32)((u32)p * s->pstride);
        k.p1[p] = k.p0[p] + s->fetch + s->mod0;
        k.s[p] = 0;
    }
    int line = -1, x = 0, pm = 1, pmmoves = 0, grp = 0, pre = 0, words = 0, wrapped = 0, beam;
    int pmmax = ((0xE2 - 0xD9) + ddf_start(s)) / 4 - 1;     /* moves between the line above's $D9 and DDFSTRT */
    if (pmmax > 12)
        pmmax = 12;
    *bad = -1;
    for (int i = 0; i < n; i++) {
        const struct dpycop *c = &l[i];
        *bad = i;
        switch (c->op) {
        case CO_WAIT: {
            int L = c->a >> s->slace;
            if (c->a >= s->h || c->b >= s->w || L < line || (L == line && c->b < x))
                return -1;
            if (line < 0)
                pre += grp;
            else if (grp > budget(line, hpos(s, x), L, hpos(s, c->b)))
                return -1;              /* the Copper could not make these moves before the next WAIT */
            grp = 0;
            pm = c->b == 0 && L > line; /* pointers and modulos: only at a new line's start */
            pmmoves = 0;
            /* the translation: a WAIT for the end of the line above (pointers and modulos), then the line's */
            beam = DIW_V + L;
            if (pm) {
                if (beam - 1 >= 256 && !wrapped) { wrapped = 1; words++; }
                words++;
            }
            if (beam >= 256 && !wrapped) { wrapped = 1; words++; }
            words++;
            line = L;
            x = c->b;
            break;
        }
        case CO_COLOUR:
            if (c->a >= ncol)
                return -1;
            pm = 0;
            grp += aga ? 4 : 1;
            words += aga ? 4 : 1;
            break;
        case CO_SCROLL:
            pm = 0;
            grp++;
            words++;
            break;
        case CO_PLANE:
        case CO_MOD: {
            if (!pm)
                return -1;              /* only at a line's start (after a WAIT with b 0) or before the first WAIT */
            int L = line < 0 ? 0 : line;
            if (c->op == CO_PLANE) {
                s32 rel = (s32)((u32)c->b << 16 | c->c);
                if (c->a >= s->depth || ((u32)rel & align()) || rel < 0 || (u32)rel >= s->bytes)
                    return -1;
                seg(s, &k, c->a, L);
                k.p0[c->a] = rel;
                k.p1[c->a] = rel + s->fetch + k.m[c->a & 1];
                pmmoves += 2;
            } else {
                s32 m0 = (s16)c->a, m1 = (s16)c->b;
                if (((u32)m0 & align()) || ((u32)m1 & align()) || !hold_ok(hwmod(s, m0)) || !hold_ok(hwmod(s, m1)))
                    return -1;
                for (int p = 0; p < s->depth; p++)
                    seg(s, &k, p, L);
                k.m[0] = m0;            /* (the pointers go on from where they are, in both fields) */
                k.m[1] = m1;
                pmmoves += 2;
            }
            if (pmmoves > pmmax)
                return -1;              /* they would not all be written before the line's first fetch */
            grp += 2;
            words += 2;
            break;
        }
        default:
            return -1;
        }
    }
    *bad = -1;
    if (line >= 0 && grp > budget(line, hpos(s, x), H, 0))
        return -1;                      /* (the end's moves start at (H - 1, $D9), as a pointer group would) */
    if (line < 0)
        pre += grp;
    for (int p = 0; p < s->depth; p++)
        seg(s, &k, p, H);
    *lo = k.lo;
    *hi = k.hi;
    if (pre + base_moves(s) > VBLINE * (DIW_V - 2))
        return -1;                      /* more than the vertical blank can hold before the picture */
    return words;
}

/* the base part's and the tail's instructions (section 4) */
static int base_moves(const struct scrn *s)
{
    int n = (aga ? 3 : 0) + 7 + 2 + 2 * s->depth;
    n += aga ? ((s->npal + 31) / 32) * 2 + 2 * s->npal + 1 : s->npal;
    return n;
}
static int tail_moves(const struct scrn *s) { (void)s; return 2 + 5 + 8 + 1; }

/* ---- the translation into one list (buffer b, field f); rec: record the words' places (one buffer's first build of
 * a shape: the places are the same in every buffer and field) ---- */
static int wrapped;
static u16 *wait_to(u16 *c, int beam, int hp)
{
    if (beam >= 256 && !wrapped) {
        *c++ = 0xFFDF; *c++ = 0xFFFE;
        wrapped = 1;
    }
    *c++ = (u16)((beam & 0xFF) << 8 | (hp & 0xFE) | 1);
    *c++ = 0xFFFE;
    return c;
}
static void build(struct scrn *s, int b, int f, int rec)
{
    u16 *c0 = s->buf[b][f], *c = c0;
    u32 pa = s->planes + s->off;
    s32 m[2] = { s->mod0, s->mod0 };
#define MOVE(r, v) do { *c++ = (u16)(r); *c++ = (u16)(v); } while (0)
#define PTR(k, R0, R1) do { s32 r0_ = (R0), r1_ = (R1); u32 a_ = pa + (u32)(f ? r1_ : r0_); \
        if (rec) { s->ptrs[s->nptr].at = (u16)(c - c0 + 1); s->ptrs[s->nptr].rel0 = r0_; s->ptrs[s->nptr].rel1 = r1_; s->nptr++; } \
        MOVE(0x0E0 + 4 * (k), a_ >> 16); MOVE(0x0E2 + 4 * (k), a_); } while (0)
    wrapped = 0;
    if (rec)
        s->nptr = 0;
    if (aga) {
        MOVE(0x1FC, 3);                 /* FMODE: 64-bit planes, 16-pixel sprites (the kernel's contract) */
        MOVE(0x106, 0x0C00);
        MOVE(0x10C, 0x0011);
    }
    MOVE(0x100, con0(s));
    MOVE(0x102, 0);
    MOVE(0x104, aga && !(s->flags & DPYSF_EHB) ? 0x0224 : 0x0024);  /* KILLEHB unless EHB (AGA) */
    MOVE(0x108, hwmod(s, s->mod0));
    MOVE(0x10A, hwmod(s, s->mod0));
    MOVE(0x092, ddf_start(s));
    MOVE(0x094, ddf_stop(s));
    u32 z = (u32)scr_zero;
    MOVE(0x120, z >> 16);               /* sprite 0 empty: no pointer on a screen */
    MOVE(0x122, z);
    if (aga) {
        for (int bk = 0; bk * 32 < s->npal; bk++) {
            int e = bk * 32 + 32 < s->npal ? bk * 32 + 32 : s->npal;
            MOVE(0x106, 0x0C00 | bk << 13);
            for (int i = bk * 32; i < e; i++) {
                if (rec) s->palat[i][0] = (u16)(c - c0 + 1);
                MOVE(0x180 + 2 * (i & 31), rgb12(s->pal[i]));
            }
            MOVE(0x106, 0x0E00 | bk << 13);
            for (int i = bk * 32; i < e; i++) {
                if (rec) s->palat[i][1] = (u16)(c - c0 + 1);
                MOVE(0x180 + 2 * (i & 31), rgblo(s->pal[i]));
            }
        }
        MOVE(0x106, 0x0C00);
    } else
        for (int i = 0; i < s->npal; i++) {
            if (rec) s->palat[i][0] = (u16)(c - c0 + 1);
            MOVE(0x180 + 2 * i, rgb12(s->pal[i]));
        }
    for (int k = 0; k < s->depth; k++) {
        s32 r0 = (s32)((u32)k * s->pstride);
        PTR(k, r0, r0 + s->fetch + s->mod0);
    }
    for (int i = 0; i < s->nlist; i++) {
        const struct dpycop *o = &s->list[i];
        if (rec)
            s->colat[i][0] = s->colat[i][1] = 0;
        switch (o->op) {
        case CO_WAIT: {
            int L = o->a >> s->slace, hp = hpos(s, o->b);
            int j = i + 1;
            if (o->b == 0 && j < s->nlist && (s->list[j].op == CO_PLANE || s->list[j].op == CO_MOD)) {
                c = wait_to(c, DIW_V + L - 1, 0xD9);    /* the line above's fetch is over: pointers, modulos */
                for (; j < s->nlist && (s->list[j].op == CO_PLANE || s->list[j].op == CO_MOD); j++) {
                    const struct dpycop *q = &s->list[j];
                    if (rec)
                        s->colat[j][0] = s->colat[j][1] = 0;
                    if (q->op == CO_PLANE) {
                        s32 rel = (s32)((u32)q->b << 16 | q->c);
                        PTR(q->a, rel, rel + s->fetch + m[q->a & 1]);
                    } else {
                        m[0] = (s16)q->a;
                        m[1] = (s16)q->b;
                        MOVE(0x108, hwmod(s, m[0]));
                        MOVE(0x10A, hwmod(s, m[1]));
                    }
                }
            }
            c = wait_to(c, DIW_V + L, hp);
            i = j - 1;
            break;
        }
        case CO_PLANE:                  /* before the first WAIT: from line 0 */
        case CO_MOD:
            if (o->op == CO_PLANE) {
                s32 rel = (s32)((u32)o->b << 16 | o->c);
                PTR(o->a, rel, rel + s->fetch + m[o->a & 1]);
            } else {
                m[0] = (s16)o->a;
                m[1] = (s16)o->b;
                MOVE(0x108, hwmod(s, m[0]));
                MOVE(0x10A, hwmod(s, m[1]));
            }
            break;
        case CO_COLOUR: {
            u32 v = (u32)o->b << 16 | o->c;
            int r = 0x180 + 2 * (o->a & 31);
            if (aga) {
                MOVE(0x106, 0x0C00 | (o->a >> 5) << 13);
                if (rec) s->colat[i][0] = (u16)(c - c0 + 1);
                MOVE(r, rgb12(v));
                MOVE(0x106, 0x0E00 | (o->a >> 5) << 13);
                if (rec) s->colat[i][1] = (u16)(c - c0 + 1);
                MOVE(r, rgblo(v));
            } else {
                if (rec) s->colat[i][0] = (u16)(c - c0 + 1);
                MOVE(r, rgb12(v));
            }
            break;
        }
        case CO_SCROLL:
            MOVE(0x102, aga ? o->a : o->a & 0xFF);
            break;
        }
    }
    int H = s->h >> s->slace;
    if (H < base_h) {                   /* a short screen: the empty line below it (still a plane fetched) */
        c = wait_to(c, DIW_V + H, 0);   /* the next line's start: BPLCON0 changed after the last line is shown
                                         * (at its $D9 the line's last pixels would show one plane) */
        MOVE(0x100, 0x1200 | (s->hires ? 0x8000 : 0) | (s->slace ? 4 : 0));    /* one plane: 5 moves before the fetch */
        MOVE(0x0E0, z >> 16);
        MOVE(0x0E2, z);
        MOVE(0x108, -s->fetch);
        MOVE(0x10A, -s->fetch);
    }
    if (s->slace) {                     /* the desktop's field scheme: the long field's list next, the short one's at
                                         * the line only a long field has */
        MOVE(0x080, (u32)copl[0] >> 16);
        MOVE(0x082, (u32)copl[0]);
        c = wait_to(c, 256, 0);         /* (only the wrap, if not passed yet) */
        c -= 2;
        *c++ = (u16)((frame_l & 0xFF) << 8 | 0x01);
        *c++ = 0xFFFE;
        MOVE(0x080, (u32)copl[1] >> 16);
        MOVE(0x082, (u32)copl[1]);
    } else {
        MOVE(0x080, (u32)copl[0] >> 16);
        MOVE(0x082, (u32)copl[0]);
    }
    *c++ = 0xFFFF;
    *c++ = 0xFFFE;
#undef PTR
#undef MOVE
    if ((u32)(c - c0) > 2 * s->room)    /* never: check() counted the instructions */
        dprintf(2, "displayd: a screen's list overran its room (%u > %u)\n", (u32)(c - c0) / 2, s->room);
}
static void build_all(struct scrn *s, int b)
{
    build(s, b, 0, 1);
    if (s->slace)
        build(s, b, 1, 0);
    s->bgen[b] = s->gen;
}
/* a buffer's pointers for the offset off now (its shape the current one) */
static void points(struct scrn *s, int b)
{
    for (int f = 0; f <= s->slace; f++) {
        u16 *c = s->buf[b][f];
        for (int i = 0; i < s->nptr; i++) {
            u32 a = s->planes + s->off + (u32)(f ? s->ptrs[i].rel1 : s->ptrs[i].rel0);
            c[s->ptrs[i].at] = (u16)(a >> 16);
            c[s->ptrs[i].at + 2] = (u16)a;
        }
    }
}
static void show_buf(struct scrn *s, int b)
{
    cop_show(s->tramp[b][0], s->tramp[b][s->slace]);
}
/* the next picture: the buffer not in use made current, then shown (in front) */
static void flip(struct scrn *s)
{
    cop_settle();
    int b = s->cur ^ 1;
    if (s->bgen[b] != s->gen)
        build_all(s, b);
    else
        points(s, b);
    if (s->slot == front)
        show_buf(s, b);
    s->cur = b;
}
/* colours written in place into every buffer of the current shape */
static void poke(struct scrn *s, u16 at, u16 atlo, u32 v)
{
    for (int b = 0; b < 2; b++)
        if (s->bgen[b] == s->gen)
            for (int f = 0; f <= s->slace; f++) {
                if (at) s->buf[b][f][at] = rgb12(v);
                if (atlo) s->buf[b][f][atlo] = rgblo(v);
            }
}

/* ---- front and back ---- */
static void to_front(int i)
{
    if (front == i)
        return;
    if (front >= 0)
        tell(scrs[front], EV_FOCUS, 0, 0, 0, 0, 0, 0);
    front = i;
    if (i >= 0) {
        last = i;
        input_reset();
        show_buf(scrs[i], scrs[i]->cur);
        if (!nofocus)
            tell(scrs[i], EV_FOCUS, 0, 1, 0, 0, 0, 0);
    } else
        cop_show(0, 0);
}
void scr_focus(int in)                  /* the console shown (1) or switched away (0) */
{
    nofocus = !in;
    if (front >= 0)
        tell(scrs[front], EV_FOCUS, 0, in, 0, 0, 0, 0);
}

static void scr_free(struct scrn *s)
{
    int i = s->slot;
    if (front == i)
        to_front(-1);
    if (last == i)
        last = -1;
    cop_settle();                       /* the Copper is off its lists and planes from here */
    void *mem = s->chipmem;
    u32 planes = s->planes;
    chip_lists -= s->chip;
    scrs[i] = 0;
    sys_conhold(gfd, 0, planes);        /* the kernel's hold ends (a dead owner's block is freed now) */
    sys_memfree(mem);                   /* (the record too: nothing of s after this) */
}

/* the palette from the client's memory (0x00RRGGBB) into s->pal[first ..] */
static int pal_in(struct umsg *m, struct scrn *s, u32 first, u32 n, u32 addr)
{
    if (first + n > s->npal || first + n < first)
        return -EINVAL;
    if (n && ((addr & 1) || dpy_peer(m, PEER_RD, addr, n * 4) < 0))
        return -EFAULT;
    const u32 *v = (const u32 *)addr;
    for (u32 i = 0; i < n; i++)
        s->pal[first + i] = v[i] & 0xFFFFFF;
    return 0;
}

static int scr_make(struct umsg *m, struct scrn *s, const struct dpyscr *qp, int i, int cl, struct scrn **out);
/* DPY_SCREEN (section 3) */
static int scr_open(struct umsg *m, int cl)
{
    struct dpyscr *d = (struct dpyscr *)m->a[0];
    if (dpy_peer(m, PEER_RD, m->a[0], DPYSCR_FIRST) < 0 || d->size < DPYSCR_FIRST)
        return -EFAULT;
    if (rtg)
        return -ENODEV;                 /* the desktop is on a graphics card (section 9) */
    if (of(cl))
        return -EBUSY;
    if (dragging || shadowing)
        return -EBUSY;
    int i = 0;
    while (i < NSCR && scrs[i])
        i++;
    if (i == NSCR)
        return -ENOSPC;
    struct dpyscr q = *d;               /* read once: the client cannot change it under the checks */
    int hires = q.w == 640, slace = (q.flags & DPYSF_LACE) != 0, ham = (q.flags & DPYSF_HAM) != 0;
    int ehb = (q.flags & DPYSF_EHB) != 0;
    if ((q.w != 320 && q.w != 640) || !q.h || q.h > (u32)(base_h << slace) || (slace && (q.h & 1)) ||
        q.depth < 1 || q.depth > 8 || (q.flags & ~63u) || (ham && ehb) || (ham && q.depth != 6 && q.depth != 8) ||
        (ehb && q.depth != 6) || q.ncop > DPYSCR_COPMAX || q.ncol > 256)
        return -EINVAL;
    if (!aga && (q.ncol > 32 || q.depth > (hires ? 4 : 6) || (ham && q.depth == 8)))
        return -ENODEV;                 /* a mode the chips lack */
    if (!aga && q.depth == 6 && !ham && !ehb)
        return -EINVAL;                 /* OCS/ECS: 6 lores planes are HAM6 or EHB */
    u32 al = aga ? 7 : 1;
    if ((q.planes & al) || (q.bpr & al) || (q.pstride & al))
        return -EINVAL;
    if (q.bytes > (2u << 20) || (q.depth > 1 && q.pstride >= q.bytes))
        return -EINVAL;                 /* (bounded: the fetch's sums below stay far inside 32 bits) */
    static struct scrn tmp;             /* made here, then copied into the screen's own region */
    struct scrn *s;
    int r = scr_make(m, &tmp, &q, i, cl, &s);
    if (r < 0)
        return r;
    scrs[i] = s;
    s->used = 1;
    s->gen = 1;
    build_all(s, 0);
    s->cur = 0;
    if (!(q.flags & DPYSF_BACK))
        to_front(i);
    return when();
}
/* the screen's record and its lists (no change anywhere on failure) */
static int scr_make(struct umsg *m, struct scrn *s, const struct dpyscr *qp, int i, int cl, struct scrn **out)
{
    struct dpyscr q = *qp;
    int hires = q.w == 640, slace = (q.flags & DPYSF_LACE) != 0, ham = (q.flags & DPYSF_HAM) != 0;
    int ehb = (q.flags & DPYSF_EHB) != 0;
    memset(s, 0, sizeof *s);
    s->slot = i;
    s->cl = cl;
    s->w = q.w; s->h = q.h; s->depth = q.depth; s->flags = q.flags; s->bpr = q.bpr; s->ncop = q.ncop;
    s->hires = (u16)hires; s->slace = (u16)slace;
    s->planes = q.planes; s->bytes = q.bytes; s->pstride = q.pstride;
    s->fetch = (u16)(q.w / 8 + (q.flags & DPYSF_SCROLL ? (aga ? 8 : 2) : 0));
    s->mod0 = (s16)(q.bpr - s->fetch);
    s->npal = (u16)(ham ? (q.depth == 8 ? 64 : 16) : ehb ? 32 : 1 << q.depth);
    if (s->npal > (aga ? 256 : 32))
        s->npal = aga ? 256 : 32;
    if (q.bpr < s->fetch || !hold_ok(hwmod(s, s->mod0)))
        return -EINVAL;
    int r = pal_in(m, s, 0, q.ncol < s->npal ? q.ncol : s->npal, q.palette);
    if (r < 0)
        return r;
    int bad;
    if (check(s, 0, 0, &s->lo, &s->hi, &bad) < 0 || s->lo < 0 || (u32)s->hi > q.bytes)
        return -EINVAL;                 /* the planes' fetch outside the memory given */
    long held = sys_conhold(gfd, m->from, q.planes);
    if (held < 0)
        return held == -EBUSY ? -EBUSY : -EINVAL;   /* not a chip RAM region of the client's (or held already) */
    if ((u32)held < q.bytes) {
        sys_conhold(gfd, 0, q.planes);
        return -EINVAL;
    }
    /* the lists: the base, the most a list of ncop translates to, the end */
    u32 room = (u32)(base_moves(s) + 4 * q.ncop + tail_moves(s) + 4);
    if (room > MAXMOVES)
        room = MAXMOVES;
    u32 lists = 2 * (1 + (u32)slace) * room * 4, nl = q.ncop, rec = (sizeof *s + 7) & ~7u;
    u32 bytes = rec + lists + 2 * nl * (u32)sizeof(struct dpycop) + (q.depth + nl) * (u32)sizeof(struct ptr) +
                (nl + 1) * 4 + (((nl + 1) * 2 + 7) & ~7u);
    u8 *mem;
    if (chip_lists + bytes > SCR_BUDGET || !(mem = sys_memalloc_chip(bytes))) {
        sys_conhold(gfd, 0, q.planes);
        return -ENOMEM;
    }
    s->room = room;
    s->chip = bytes;
    s->chipmem = mem;
    chip_lists += bytes;
    u16 *c = (u16 *)(mem + rec);
    for (int b = 0; b < 2; b++)
        for (int f = 0; f < 2; f++) {
            s->buf[b][f] = f && !slace ? s->buf[b][0] : c;
            if (!f || slace)
                c += 2 * room;
            u16 *t = scr_tramp + 6 * (4 * i + 2 * b + f);  /* the screen slot's trampolines (displayd.c's block) */
            u32 a = (u32)s->buf[b][f];
            t[0] = 0x084; t[1] = (u16)(a >> 16);
            t[2] = 0x086; t[3] = (u16)a;
            t[4] = 0x08A; t[5] = 0;
            s->tramp[b][f] = t;
        }
    u8 *p = mem + rec + lists;
    s->list = (struct dpycop *)p; p += nl * sizeof(struct dpycop);
    s->spare = (struct dpycop *)p; p += nl * sizeof(struct dpycop);
    s->ptrs = (struct ptr *)p; p += (q.depth + nl) * sizeof(struct ptr);
    s->colat = (u16 (*)[2])p; p += (nl + 1) * 4;
    s->cidx = (u16 *)p;
    memcpy(mem, s, sizeof *s);          /* the record into its region */
    *out = (struct scrn *)mem;
    return 0;
}

static int scr_set(struct umsg *m, struct scrn *s)
{
    u32 *a = m->a;
    switch (a[0]) {
    case SS_PALETTE: {
        u32 first = a[1] & 0xFFFF, n = a[1] >> 16;
        int r = pal_in(m, s, first, n, a[2]);
        if (r < 0)
            return r;
        for (u32 i = first; i < first + n; i++)
            poke(s, s->palat[i][0], aga ? s->palat[i][1] : 0, s->pal[i]);
        return when();
    }
    case SS_COPPER: {
        u32 n = a[2];
        if (n > s->ncop)
            return -EINVAL;
        if (n && ((a[1] & 1) || dpy_peer(m, PEER_RD, a[1], n * sizeof(struct dpycop)) < 0))
            return -EFAULT;
        struct dpycop *l = s->spare;
        memcpy(l, (const void *)a[1], n * sizeof *l);  /* checked and kept in our memory: never read again there */
        s32 lo, hi;
        int bad, w = check(s, l, (int)n, &lo, &hi, &bad);
        if (w >= 0 && (s32)s->off + lo >= 0 && (u32)((s32)s->off + hi) <= s->bytes &&
            (u32)(w + base_moves(s) + tail_moves(s)) <= s->room) {
            s->spare = s->list;
            s->list = l;
            s->nlist = (int)n;
            s->ncidx = 0;
            for (u32 i = 0; i < n; i++)
                if (l[i].op == CO_COLOUR)
                    s->cidx[s->ncidx++] = (u16)i;
            s->lo = lo;
            s->hi = hi;
            s->gen++;
            flip(s);
            return when();
        }
        a[3] = (u32)bad;
        return -EINVAL;
    }
    case SS_PLANES: {
        u32 off = a[1];
        if ((off & align()) || off > s->bytes || (s32)off + s->lo < 0 || (u32)((s32)off + s->hi) > s->bytes)
            return -EINVAL;
        s->off = off;
        flip(s);
        return when();
    }
    case SS_FRONT:
        if (dragging || shadowing)
            return -EBUSY;
        if (a[1])
            to_front(s->slot);
        else if (front == s->slot)
            to_front(-1);
        return 0;
    case SS_COLOURS: {                  /* the next count CO_COLOURs from instruction first get the colours */
        u32 first = a[1] & 0xFFFF, n = a[1] >> 16;
        if (first > (u32)s->nlist)
            return -EINVAL;
        if (n && ((a[2] & 1) || dpy_peer(m, PEER_RD, a[2], n * 4) < 0))
            return -EFAULT;
        int k0 = 0;
        while (k0 < s->ncidx && s->cidx[k0] < first)
            k0++;
        u32 have = (u32)(s->ncidx - k0), set = n < have ? n : have;
        const u32 *v = (const u32 *)a[2];
        int live[2] = { s->bgen[0] == s->gen, s->bgen[1] == s->gen };
        for (u32 k = 0; k < set; k++) {
            u32 c = v[k] & 0xFFFFFF;
            struct dpycop *o = &s->list[s->cidx[k0 + (int)k]];
            if (((u32)o->b << 16 | o->c) == c)
                continue;               /* (a colour a line: most lines keep theirs) */
            o->b = (u16)(c >> 16);
            o->c = (u16)c;
            const u16 *at = s->colat[s->cidx[k0 + (int)k]];
            u16 hi = rgb12(c), lo = rgblo(c);
            for (int b = 0; b < 2; b++)
                if (live[b])
                    for (int f = 0; f <= s->slace; f++) {
                        s->buf[b][f][at[0]] = hi;
                        if (at[1]) s->buf[b][f][at[1]] = lo;
                    }
        }
        if (set < n) {                  /* fewer colours in the list than given: those found are set */
            a[3] = set;
            return -EINVAL;
        }
        return when();
    }
    }
    return -EINVAL;
}

int scr_req(struct umsg *m, int cl)
{
    if (m->type == DPY_SCREEN)
        return scr_open(m, cl);
    struct scrn *s = of(cl);
    if (!s)
        return -ENOENT;
    if (m->type == DPY_SCR_CLOSE) {
        scr_free(s);
        return 0;
    }
    return scr_set(m, s);
}
void scr_drop(int cl)
{
    struct scrn *s = of(cl);
    if (s)
        scr_free(s);
}
/* GEV_GONE: a held region's owner ended (the kernel handed the block to us): its screen goes now */
void scr_gone(u32 base)
{
    for (int i = 0; i < NSCR; i++)
        if (scrs[i] && scrs[i]->planes == base) {
            dprintf(1, "displayd: a screen's program ended: its screen taken off, its planes freed\n");
            scr_free(scrs[i]);
        }
}

/* ---- input while a screen is in front (and Left Amiga+N always) ---- */
#define RAW_N 0x36
#define RAW_M 0x37
int scr_key(const struct ugevent *e)
{
    int code = e->code & 0x7F, up = e->code & 0x80;
    int amiga = (e->qual & (GQ_LAMIGA | GQ_LSHIFT | GQ_RSHIFT | GQ_CTRL | GQ_LALT | GQ_RALT | GQ_RAMIGA)) == GQ_LAMIGA;
    if (amiga && (code == RAW_N || (code == RAW_M && front >= 0))) {
        if (!up && !dragging && !shadowing) {
            if (front >= 0)
                to_front(-1);
            else if (last >= 0 && scrs[last])
                to_front(last);
        }
        return 1;
    }
    if (front < 0)
        return 0;
    struct scrn *s = scrs[front];
    tell(s, EV_KEY, e->code, 0, s->hires ? e->x : e->x >> 1, e->y << s->slace, e->qual, e->ch);
    return 1;
}
int scr_pointer(const struct ugevent *e, u16 old, u16 now, int moved)
{
    if (front < 0)
        return 0;
    struct scrn *s = scrs[front];
    int x = s->hires ? e->x : e->x >> 1, y = e->y << s->slace;
    if (moved && (now || (s->flags & DPYSF_MOTION)))
        tell(s, EV_MOTION, 0, 0, x, y, e->qual, 0);
    static const u16 bit[3] = { GQ_LBUTTON, GQ_RBUTTON, GQ_MBUTTON };
    for (int i = 0; i < 3; i++)
        if ((old ^ now) & bit[i])
            tell(s, EV_BUTTON, (i + 1) | (now & bit[i] ? 0 : 0x80), 0, x, y, e->qual, 0);
    return 1;
}
