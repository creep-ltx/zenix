/* SPDX-License-Identifier: GPL-2.0-only */
/* MED and OctaMED modules (MMD0, MMD1, MMD2, MMD3) for the tracker player (DESIGN D56 section 3), written from Teijo
 * Kinnunen's "MED/OctaMED MMD0 and MMD1 file formats" (revision 1, 1992: the header, the song, the blocks, the
 * instruments, the expansion), MED 3.21's manual (the player commands and the two tempos), OctaMED SoundStudio's
 * manual (appendix A: the commands 11-1F, 2E; the BPM mode) and the published field list of MMD2's song (its play
 * sequences and sections); no player's source was read. Sample instruments only (8- and 16-bit); synthetic, hybrid
 * and multi-octave instruments are silent, MIDI and the mix mode's echo are ignored, hold and decay too. */
#include "trk.h"
#ifdef TRK_HOST
#include <string.h>
#else
#include "libz.h"
#endif

#define MED_CH 16
#define MED_ORD 1024            /* the flattened play sequence's longest */

struct medsmp { const signed char *d; unsigned len, loop, looplen; int vol, trans, fine; char name[41]; };
struct medch {
    int note, smp, per, vol, fine;      /* note: 0 = C-1 (-1 none) */
    int cmd, dat;
    int ptarget, pspeed, vspeed, vdepth, vpos, vshift, tspeed, tdepth, tpos;
    int loopr, loopn, dnote, dsmp, retr, cutat, pan;
    int oper, ovol, trig, cut, offset;
};
struct med {
    const unsigned char *d;
    unsigned size;
    int ver, nblk, nsmp, ntrk, flags, flags2, tempo, tempo2, transp, master;
    unsigned blkarr;
    unsigned short ord[MED_ORD];        /* the blocks in play order */
    unsigned short sect[MED_ORD];       /* the order index where each entry's play sequence starts */
    int norder;
    unsigned char tvol[MED_CH];
    signed char tpan[MED_CH];
    struct medsmp smp[64];
    struct medch c[MED_CH];
    int tick, row, pos, brk, brkrow, jump, ploop, plrow, pdelay, pdelayn, rep, ended, stop;
};

static const short pt[36] = { 856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320,
                              302, 285, 269, 254, 240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113 };

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned be32(const unsigned char *p) { return be16(p) << 16 | be16(p + 2); }

/* a pointer field: an offset in the file that leaves room for n bytes, or 0 */
static unsigned ptr(const struct med *m, unsigned at, unsigned n)
{
    if (at + 4 > m->size) return 0;
    unsigned o = be32(m->d + at);
    return o && o < m->size && n <= m->size - o ? o : 0;
}

/* a note's Paula period: C-1 (0) .. B-3 (35) as ProTracker's (finetune by 1/8 semitones as its table is made);
 * higher notes an octave up for each 12, lower ones held at C-1 */
static int period(int n, int fine)
{
    if (n < 0) n = 0;
    int oct = 0;
    while (n > 35) { n -= 12; oct++; }
    int p = pt[n];
    if (fine) {                                 /* between this note and the next by fine / 8 (-8 .. 7) */
        int q = fine > 0 ? (n < 35 ? pt[n + 1] : pt[n] * 17 / 18) : (n > 0 ? pt[n - 1] : pt[0] * 18 / 17);
        p += (q - p) * (fine < 0 ? -fine : fine) / 8;
    }
    return p >> oct;
}

/* a block's place, its tracks and lines */
static const unsigned char *block(const struct med *m, int b, int *tracks, int *lines)
{
    if (b < 0 || b >= m->nblk) return 0;
    unsigned o = ptr(m, m->blkarr + 4u * (unsigned)b, m->ver ? 8 : 2);
    if (!o) return 0;
    const unsigned char *p = m->d + o;
    unsigned nt = m->ver ? be16(p) : p[0], nl = (m->ver ? be16(p + 2) : p[1]) + 1u, hb = m->ver ? 8u : 2u;
    unsigned cs = m->ver ? 4u : 3u;
    if (!nt || nt > 64 || nl > 3200 || hb + nt * nl * cs > m->size - o) return 0;
    *tracks = (int)nt;
    *lines = (int)nl;
    return p + hb;
}

/* the cell at line l, track t: note (0 none), instrument, command, its data */
static int cellat(const struct med *m, int b, int l, int t, int *note, int *ins, int *cmd, int *dat)
{
    int nt, nl;
    const unsigned char *p = block(m, b, &nt, &nl);
    *note = *ins = *cmd = *dat = 0;
    if (!p || l >= nl || t >= nt) return 0;
    if (m->ver) {
        p += ((unsigned)l * (unsigned)nt + (unsigned)t) * 4u;
        *note = p[0] & 0x7F;
        *ins = p[1] & 0x3F;
        *cmd = p[2];
        *dat = p[3];
    } else {
        p += ((unsigned)l * (unsigned)nt + (unsigned)t) * 3u;
        *note = p[0] & 0x3F;
        *ins = (p[1] >> 4) | (p[0] & 0x80 ? 16 : 0) | (p[0] & 0x40 ? 32 : 0);
        *cmd = p[1] & 15;
        *dat = p[2];
    }
    return 1;
}

static void vslide(struct medch *c, int d)
{
    if (d >> 4) c->vol += d >> 4; else c->vol -= d & 15;
    c->vol = c->vol < 0 ? 0 : c->vol > 64 ? 64 : c->vol;
}

static void tempo_set(struct med *m, struct trk *t)
{
    if (m->flags2 & 0x20) {                     /* BPM: a tick is 1 / (BPM x lines a beat / 10) s */
        int lpb = (m->flags2 & 0x1F) + 1;
        t->tnum = 10;
        t->tden = (unsigned)(m->tempo > 0 ? m->tempo : 125) * (unsigned)lpb;
        if (t->tden < 40) t->tden = 40;         /* a tick of 0.25 s at most (the player's buffer; D56 review) */
        t->tempo = m->tempo;
    } else if (m->tempo <= 10) {                /* 1-10: the trackers' speed, ticks at 50 Hz */
        t->tnum = 1;
        t->tden = 50;
        t->tempo = 125;
    } else {                                    /* 33 is ProTracker's 125 BPM: a tick 0.66 / tempo s */
        t->tnum = 33;
        t->tden = 50u * (unsigned)m->tempo;
        t->tempo = m->tempo * 125 / 33;
    }
}

static void trigger(struct medch *c, int n)
{
    c->note = n;
    c->per = period(n, c->fine);
    c->trig = 1;
    c->offset = 0;
    c->vpos = c->tpos = 0;
}

static void noteins(struct med *m, struct medch *c, int note, int ins, int porta)
{
    if (ins && ins <= m->nsmp) {
        c->smp = ins;
        c->vol = m->smp[ins - 1].vol;
        c->fine = m->smp[ins - 1].fine;
    }
    if (!note || !c->smp) return;
    int n = note - 1 + m->transp + m->smp[c->smp - 1].trans;
    if (n < 0) n = 0;
    if (n > 71) n = 71;
    if (porta) { c->ptarget = period(n, c->fine); c->note = n; return; }
    trigger(c, n);
}

static void row_start(struct med *m, struct trk *t)
{
    int b = m->ord[m->pos];
    for (int i = 0; i < m->ntrk; i++) {
        struct medch *c = &m->c[i];
        int note, ins, cmd, dat;
        cellat(m, b, m->row, i, &note, &ins, &cmd, &dat);
        if (!m->ver && cmd > 15) cmd = 0;
        c->cmd = cmd;
        c->dat = dat;
        c->retr = 0;
        c->cutat = -1;
        int hx = dat >> 4, lx = dat & 15;
        int delay = cmd == 0x1F && hx ? hx : cmd == 0x0F && dat == 0xF2 ? m->tempo2 / 2 :
                    cmd == 0x0F && dat == 0xF4 ? m->tempo2 / 3 : cmd == 0x0F && dat == 0xF5 ? m->tempo2 * 2 / 3 : 0;
        if (delay) { c->dnote = note; c->dsmp = ins; }
        else if (cmd == 0x0F && dat == 0xFD && note && c->smp) {     /* FFD: the pitch only */
            int n = note - 1 + m->transp + m->smp[c->smp - 1].trans;
            c->note = n < 0 ? 0 : n > 71 ? 71 : n;
            c->per = period(c->note, c->fine);
        } else
            noteins(m, c, note, ins, cmd == 3 || cmd == 5);
        if (delay) c->retr = -delay;            /* the delayed start: a negative count */
        switch (cmd) {
        case 3: if (dat) c->pspeed = dat; break;
        case 4: case 0x14: if (hx) c->vspeed = hx; if (lx) c->vdepth = lx; c->vshift = cmd == 4 ? 6 : 7; break;
        case 7: if (hx) c->tspeed = hx; if (lx) c->tdepth = lx; break;
        case 9: if (dat >= 1 && dat <= 0x20) m->tempo2 = dat; break;
        case 0xB: m->brk = 1; m->jump = m->sect[m->pos] + dat; break;
        case 0xC: {
            int v = dat;
            if (dat >= 0x80 && dat <= 0xC0) v = dat - 0x80;        /* the instrument's default volume */
            else if (!(m->flags & 0x10)) v = hx * 10 + lx;           /* decimal unless FLAG_VOLHEX */
            c->vol = v > 64 ? 64 : v;
            if (dat >= 0x80 && dat <= 0xC0 && c->smp) m->smp[c->smp - 1].vol = c->vol;
            break;
        }
        case 0xF:
            if (dat == 0) { m->brk = 1; m->brkrow = 0; }
            else if (dat <= 0xF0) { m->tempo = dat; tempo_set(m, t); }
            else if (dat == 0xF1) c->retr = (m->tempo2 + 1) / 2;
            else if (dat == 0xF3) c->retr = m->tempo2 / 3 ? m->tempo2 / 3 : 1;
            else if (dat == 0xFE) m->stop = m->ended = 1;
            else if (dat == 0xFF) c->cut = 1;
            break;
        case 0x11: c->per -= dat; if (c->per < 28) c->per = 28; break;
        case 0x12: c->per += dat; if (c->per > 856) c->per = 856; break;
        case 0x15: c->fine = (signed char)dat; c->fine = c->fine < -8 ? -8 : c->fine > 7 ? 7 : c->fine; break;
        case 0x16:
            if (!dat) c->loopr = m->row;
            else if (!c->loopn) { c->loopn = dat; m->ploop = 1; m->plrow = c->loopr; }
            else if (--c->loopn) { m->ploop = 1; m->plrow = c->loopr; }
            break;
        case 0x18: c->cutat = dat; if (!dat) c->vol = 0; break;
        case 0x19: if (c->trig) c->offset = dat << 8; break;
        case 0x1A: c->vol += dat; if (c->vol > 64) c->vol = 64; break;
        case 0x1B: c->vol -= dat; if (c->vol < 0) c->vol = 0; break;
        case 0x1D: m->brk = 1; m->brkrow = dat; break;
        case 0x1E: if (!m->pdelayn) m->pdelay = dat; break;
        case 0x1F: if (lx) c->retr = lx; break;
        case 0x2E: { int p = (signed char)dat; if (p >= -16 && p <= 16) c->pan = 128 + p * 8 > 255 ? 255 : 128 + p * 8; break; }
        }
    }
}

/* the effects of every tick (MED slides on the first tick too unless FLAG_STSLIDE) */
static void effects(struct med *m, struct medch *c, int first)
{
    int hx = c->dat >> 4, lx = c->dat & 15, slide = !first || !(m->flags & 0x20);
    switch (c->cmd) {
    case 0:
        if (c->dat && m->tick % 3) {
            int n = c->note + (m->tick % 3 == 1 ? hx : lx);
            c->oper = period(n, c->fine);
        }
        break;
    case 1: if (slide) { c->per -= c->dat; if (c->per < 28) c->per = 28; c->oper = c->per; } break;
    case 2: if (slide) { c->per += c->dat; if (c->per > 856) c->per = 856; c->oper = c->per; } break;
    case 3: case 5:
        if (slide && c->ptarget) {
            if (c->per < c->ptarget) { c->per += c->pspeed; if (c->per >= c->ptarget) c->per = c->ptarget, c->ptarget = 0; }
            else { c->per -= c->pspeed; if (c->per <= c->ptarget) c->per = c->ptarget, c->ptarget = 0; }
            c->oper = c->per;
        }
        if (c->cmd == 5 && slide) { vslide(c, c->dat); c->ovol = c->vol; }
        break;
    case 4: case 6: case 0x14:
        if (!first) {
            int w = c->vpos < 32 ? trk_sine[c->vpos & 31] : -(int)trk_sine[c->vpos & 31];
            c->oper = c->per + (w * c->vdepth >> (c->cmd == 0x14 ? 7 : 6));
            c->vpos = (c->vpos + c->vspeed) & 63;
        }
        if (c->cmd == 6 && slide) { vslide(c, c->dat); c->ovol = c->vol; }
        break;
    case 7:
        if (!first) {
            int w = c->tpos < 32 ? trk_sine[c->tpos & 31] : -(int)trk_sine[c->tpos & 31], v = c->vol + (w * c->tdepth >> 6);
            c->ovol = v < 0 ? 0 : v > 64 ? 64 : v;
            c->tpos = (c->tpos + c->tspeed) & 63;
        }
        break;
    case 0xA: case 0xD: if (slide) { vslide(c, c->dat); c->ovol = c->vol; } break;
    }
    if (c->cutat > 0 && m->tick == c->cutat) c->vol = c->ovol = 0;
    if (!first && c->retr < 0 && m->tick == -c->retr) {
        c->retr = 0;
        noteins(m, c, c->dnote, c->dsmp, 0);
        c->oper = c->per;
        c->ovol = c->vol;
    } else if (!first && c->retr > 0 && m->tick % c->retr == 0) { c->trig = 1; c->offset = 0; }
}

static void next_row(struct med *m)
{
    if (m->pdelay) { m->pdelayn = m->pdelay; m->pdelay = 0; }
    if (m->pdelayn) { m->pdelayn--; m->rep = 1; return; }
    m->rep = 0;
    if (m->ploop) { m->ploop = 0; m->row = m->plrow; m->brk = 0; m->jump = -1; return; }
    int nt, nl = 64;
    if (!block(m, m->ord[m->pos], &nt, &nl)) nl = 64;
    if (m->brk) {
        int np = m->jump >= 0 ? m->jump : m->pos + 1;
        if (m->jump >= 0 && m->jump <= m->pos) m->ended = 1;
        m->pos = np;
        m->row = m->brkrow;
        m->brk = 0;
        m->brkrow = 0;
        m->jump = -1;
    } else if (++m->row >= nl) {
        m->row = 0;
        m->pos++;
    }
    if (m->pos >= m->norder) { m->pos = 0; m->ended = 1; }
    if (!block(m, m->ord[m->pos], &nt, &nl)) nl = 64;
    if (m->row >= nl) m->row = 0;
}

static void med_tick(struct trk *t)
{
    struct med *m = t->st;
    int nt, nl = 64;
    t->pos = m->pos;
    t->row = m->row;
    t->tick = m->tick;
    t->pat = m->ord[m->pos];
    if (!block(m, t->pat, &nt, &nl)) nl = 64;
    t->rows = nl;
    for (int i = 0; i < m->ntrk; i++) { m->c[i].trig = 0; m->c[i].cut = 0; }
    int first = m->tick == 0 && !m->rep;
    if (first) row_start(m, t);
    for (int i = 0; i < m->ntrk; i++) {
        struct medch *c = &m->c[i];
        c->oper = c->per;
        c->ovol = c->vol;
        effects(m, c, first);
        struct tvoice *v = &t->v[i];
        v->flags = (c->trig ? TV_TRIG : 0) | (c->cut ? TV_CUT : 0);
        v->per = c->oper;
        v->hz16 = 0;
        int tv = m->tvol[i] ? m->tvol[i] : 64;
        v->vol = c->ovol * tv / 64 * m->master / 64;
        v->pan = c->pan;
        if (c->trig && c->smp) {
            const struct medsmp *s = &m->smp[c->smp - 1];
            unsigned o = (unsigned)c->offset;
            v->sp = t->bank;
            unsigned base = s->d ? (unsigned)(s->d - t->bank) : 0;
            if (!s->d || !s->len) v->flags = TV_CUT;
            else if (s->looplen) {
                unsigned end = s->loop + s->looplen;
                if (o >= end) o = s->loop;
                v->start = base + o;
                v->len = end - o;
                v->loop = base + s->loop;
                v->looplen = s->looplen;
            } else {
                if (o >= s->len) v->flags = TV_CUT;
                v->start = base + o;
                v->len = o < s->len ? s->len - o : 0;
                v->loop = v->looplen = 0;
            }
        }
    }
    t->speed = m->tempo2;
    if (++m->tick >= (m->tempo <= 10 && !(m->flags2 & 0x20) ? m->tempo : m->tempo2)) { m->tick = 0; next_row(m); }
    t->speed = m->tempo <= 10 && !(m->flags2 & 0x20) ? m->tempo : m->tempo2;
    tempo_set(m, t);
    t->ended = m->ended;
    t->stop = m->stop;
}

static void med_go(struct trk *t, int pos)
{
    struct med *m = t->st;
    m->pos = pos < 0 ? 0 : pos >= m->norder ? m->norder - 1 : pos;
    m->row = m->tick = 0;
    m->brk = m->ploop = m->pdelay = m->pdelayn = m->rep = 0;
    m->jump = -1;
}

static int med_name(struct trk *t, int i, char *out, int n)
{
    struct med *m = t->st;
    int k = 0;
    if (i >= 0 && i < m->nsmp)
        for (; k < 40 && k < n - 1 && m->smp[i].name[k]; k++) out[k] = m->smp[i].name[k];
    out[k] = 0;
    return k;
}

static int med_rows(struct trk *t, int pat)
{
    int nt, nl;
    return block(t->st, pat, &nt, &nl) ? nl : 64;
}

static void med_cell(struct trk *t, int pat, int row, int ch, char out[16])
{
    int note, ins, cmd, dat;
    char nn[4];
    cellat(t->st, pat, row, ch, &note, &ins, &cmd, &dat);
    trk_notename(note ? note - 1 + 12 : -1, nn);
    int fx = cmd || dat ? (cmd < 16 ? "0123456789ABCDEF"[cmd] : '+') : 0;
    trk_cellfmt(out, nn, ins, -1, fx, dat);
}

static void med_done(struct trk *t) { trk_release(t->st); }

static const struct trkops medops = { med_tick, med_go, med_name, med_rows, med_cell, med_done };

int med_trk(struct trk *t, unsigned char *d, unsigned size, char *why)
{
    if (size < 52 || d[0] != 'M' || d[1] != 'M' || d[2] != 'D' || d[3] < '0' || d[3] > '3') return 0;
    struct med *m = trk_alloc(sizeof *m);
    if (!m) { trk_say(why, "no memory"); return -1; }
    t->st = m;
    t->ops = &medops;
    m->d = d;
    m->size = size;
    m->ver = d[3] - '0';
    unsigned song = ptr(m, 8, 788);
    if (!song) { trk_say(why, "a damaged MED module (no song)"); goto bad; }
    const unsigned char *s = d + song;
    m->nblk = (int)be16(s + 504);
    m->blkarr = ptr(m, 16, 4u * (unsigned)m->nblk);
    m->tempo = (int)be16(s + 764);
    m->transp = (signed char)s[766];
    m->flags = s[767];
    m->flags2 = s[768];
    m->tempo2 = s[769] >= 1 && s[769] <= 32 ? s[769] : 6;
    if (m->tempo < 1 || m->tempo > 255) m->tempo = m->flags2 & 0x20 ? 125 : 33;
    m->master = s[786] >= 1 && s[786] <= 64 ? s[786] : 64;
    m->nsmp = s[787] > 63 ? 63 : s[787];
    if (!m->blkarr || !m->nblk) { trk_say(why, "a damaged MED module (no blocks)"); goto bad; }
    unsigned songlen = be16(s + 506);
    if (m->ver < 2) {
        if (!songlen || songlen > 256) { trk_say(why, "a damaged MED module (its play sequence)"); goto bad; }
        for (unsigned i = 0; i < songlen; i++) { m->ord[i] = s[508 + i]; m->sect[i] = 0; }
        m->norder = (int)songlen;
        for (int i = 0; i < 16; i++) m->tvol[i] = s[770 + i] <= 64 ? s[770 + i] : 64;
    } else {
        unsigned pst = ptr(m, song + 508, 4), sct = ptr(m, song + 512, 2u * songlen);
        unsigned npseq = be16(s + 522), tv = ptr(m, song + 516, 16), tp = ptr(m, song + 524, 16);
        if (!pst || !sct || !songlen) { trk_say(why, "a damaged MED module (its sections)"); goto bad; }
        for (unsigned i = 0; i < songlen && m->norder < MED_ORD; i++) {
            unsigned ps = be16(d + sct + 2 * i);
            if (ps >= npseq) continue;
            unsigned po = ptr(m, pst + 4 * ps, 42);
            if (!po) continue;
            unsigned len = be16(d + po + 40), start = (unsigned)m->norder;
            if (42 + 2 * len > size - po) len = (size - po - 42) / 2;
            for (unsigned k = 0; k < len && m->norder < MED_ORD; k++) {
                unsigned b = be16(d + po + 42 + 2 * k);
                if (b >= 0x8000) continue;          /* a command in the sequence: not played */
                m->ord[m->norder] = (unsigned short)b;
                m->sect[m->norder++] = (unsigned short)start;
            }
        }
        for (int i = 0; i < 16; i++) {
            m->tvol[i] = tv ? (d[tv + i] <= 64 ? d[tv + i] : 64) : 64;
            m->tpan[i] = tp ? (signed char)d[tp + i] : 0;
        }
        if (!m->norder) { trk_say(why, "a MED module with nothing to play"); goto bad; }
    }
    for (int i = 0; i < m->norder; i++) {
        int nt, nl;
        if (block(m, m->ord[i], &nt, &nl) && nt > m->ntrk) m->ntrk = nt;
    }
    if (!m->ntrk) { trk_say(why, "a damaged MED module (its blocks)"); goto bad; }
    if (m->ntrk > MED_CH) { trk_say(why, "a MED module of more than 16 tracks"); goto bad; }
    /* the instruments: names from the expansion, finetunes too */
    unsigned exp = ptr(m, 32, 52), iinfo = 0, ient = 0, isz = 0, xsmp = 0, xent = 0, xsz = 0;
    if (exp) {
        xsmp = ptr(m, exp + 4, 1); xent = be16(d + exp + 8); xsz = be16(d + exp + 10);
        iinfo = ptr(m, exp + 20, 1); ient = be16(d + exp + 24); isz = be16(d + exp + 26);
        unsigned sn = ptr(m, exp + 44, 1), sl = be32(d + exp + 48);
        for (unsigned i = 0; sn && i < sl && i < 32 && sn + i < size && d[sn + i]; i++)
            t->title[i] = (char)(d[sn + i] >= 32 && d[sn + i] < 127 ? d[sn + i] : '.');
    }
    unsigned sarr = ptr(m, 24, 4u * (unsigned)m->nsmp), lo = size, hi = 0;
    int odd = 0;
    for (int i = 0; i < m->nsmp; i++) {
        struct medsmp *x = &m->smp[i];
        const unsigned char *ss = s + 8 * i;
        x->vol = ss[6] > 64 ? 64 : ss[6];
        x->trans = (signed char)ss[7];
        if (xsmp && (unsigned)i < xent && xsz >= 4 && xsmp + (unsigned)i * xsz + 4 <= size) {
            int f = (signed char)d[xsmp + (unsigned)i * xsz + 3];
            x->fine = f < -8 ? -8 : f > 7 ? 7 : f;
        }
        if (iinfo && (unsigned)i < ient && isz >= 40 && iinfo + (unsigned)i * isz + 40 <= size)
            for (int k = 0; k < 40 && d[iinfo + (unsigned)i * isz + k]; k++) {
                unsigned char c = d[iinfo + (unsigned)i * isz + k];
                x->name[k] = (char)(c >= 32 && c < 127 ? c : '.');
            }
        unsigned io = sarr ? ptr(m, sarr + 4u * (unsigned)i, 6) : 0;
        if (!io) continue;
        unsigned len = be32(d + io);
        int type = (short)be16(d + io + 4);
        if (type < 0 || (type & 0x0F)) { odd++; continue; }         /* synthetic, hybrid, multi-octave: silent */
        unsigned at = io + 6;
        if (len > size - at) len = size - at;
        if (at & 1) {                                                /* Paula's places are even: the data one byte
                                                                      * down, over the header's last byte (read) */
            memmove(d + at - 1, d + at, len);
            at--;
        }
        unsigned char *sd = d + at;
        if (type & 0x20) len /= 2;                                   /* stereo: the left half */
        if (type & 0x10) {                                           /* 16-bit (big-endian): the high bytes */
            len /= 2;
            for (unsigned k = 0; k < len; k++) sd[k] = sd[2 * k];
        }
        x->d = (const signed char *)sd;
        x->len = len & ~1u;
        unsigned rep = be16(ss) * 2u, rl = be16(ss + 2) * 2u;
        if (rl > 2 && rep < x->len) {
            if (rep + rl > x->len) rl = x->len - rep;
            x->loop = rep;
            x->looplen = rl & ~1u;
            x->len = rep + x->looplen;
        }
        if (at < lo) lo = at;
        if (at + x->len > hi) hi = at + x->len;
    }
    t->fmt = TF_MED;
    trk_say(t->kind, m->ver == 0 ? "MED (MMD0)" : m->ver == 1 ? "OctaMED (MMD1)" : m->ver == 2 ? "OctaMED (MMD2)" : "OctaMED SS (MMD3)");
    t->nch = m->ntrk;
    t->songlen = m->norder;
    t->ninst = m->nsmp;
    if (hi <= lo) lo = hi = 0;
    t->bank = (const signed char *)d + lo;
    t->banksize = hi - lo;
    t->native = m->ntrk <= 4 && !(m->flags & 0x40);
    for (int i = 0; i < m->ntrk; i++) {
        m->c[i].note = -1;
        m->c[i].pan = m->tpan[i] ? 128 + m->tpan[i] * 8 : (i & 3) == 0 || (i & 3) == 3 ? 0 : 255;
        if (m->c[i].pan > 255) m->c[i].pan = 255;
        if (m->c[i].pan < 0) m->c[i].pan = 0;
        t->v[i].pan = m->c[i].pan;
    }
    (void)odd;
    m->jump = -1;
    t->pat = m->ord[0];
    t->rows = med_rows(t, t->pat);
    tempo_set(m, t);
    t->speed = m->tempo <= 10 && !(m->flags2 & 0x20) ? m->tempo : m->tempo2;
    return 1;
bad:
    med_done(t);
    t->st = 0;
    t->ops = 0;
    return -1;
}
