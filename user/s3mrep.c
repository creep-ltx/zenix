/* SPDX-License-Identifier: GPL-2.0-only */
/* Scream Tracker 3 modules (.s3m) for the tracker player (DESIGN D56 section 3), written from Scream Tracker 3.01's
 * own format description (TECH.DOC, "Scream Tracker 3.01 BETA File Formats And Mixing Info") and its user's manual's
 * effect list (Kalle Kaivola, 1994); no player's source was read. The file: a 96-byte header ("SCRM" at 44), the
 * order list, parapointers (offset / 16) to the instruments (80 bytes each, "SCRS") and to the patterns (a length
 * word, then packed rows: a byte of channel and what follows, 0 ending a row, 64 rows). Periods are ST3's (4 x the
 * Amiga's): period = 8363 x 16 x (the note's period >> octave) / C4SPD, the rate 14,317,056 / period. Adlib
 * instruments are silent. */
#include "trk.h"

#define S3M_CH 32
#define PMIN 64                 /* the periods' range (ST3's units): about 224 kHz .. 520 Hz */
#define PMAX 32767

struct s3msmp { const signed char *d; unsigned len, loop, looplen, c4spd; int vol; char name[28]; };
struct s3mch {
    int note, smp, per, vol, pan;       /* note 0..119 (-1 none) */
    unsigned c4;
    int eff, prm, last;                 /* this row's effect (1 = A) and value; the shared memory of D E F I J K L Q */
    int ptarget, pspeed;                /* G */
    int vpos, vspeed, vdepth, vwave;    /* H, U */
    int tpos, tspeed, tdepth, twave;    /* R */
    int tremor;                         /* I: ticks into the on+off cycle */
    int retrig;                         /* Q: ticks since the last start */
    int dnote, dsmp, dvol;              /* SD: the delayed note */
    int loopr, loopn;                   /* SB */
    int oper, ovol, trig, cut, offset;  /* the output this tick */
};
struct s3m {
    const unsigned char *d;
    unsigned size;
    int ordnum, insnum, patnum, gvol, flags, sign, ffv, speed, tempo, tick, row, pos;
    const unsigned char *ord;
    unsigned short *rowoff;             /* patnum x 64: each row's offset from its pattern's start (0: empty) */
    unsigned *patoff;                   /* each pattern's place in the file */
    signed char chmap[S3M_CH];          /* a pattern's channel -> our channel (-1 none) */
    struct s3msmp *smp;
    struct s3mch c[S3M_CH];
    int nch;
    int brk, brkrow, jump, ploop, plrow, pdelay, pdelayn, rep, ended, stop;
};

static const unsigned short ntab[12] = { 1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 907 };
static const unsigned short ftc4[16] = { 7895, 7941, 7985, 8046, 8107, 8169, 8232, 8280, 8363, 8413, 8463, 8529, 8581,
                                         8651, 8723, 8757 };

static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

static int notepr(int note, unsigned c4)
{
    if (note < 0 || !c4) return 0;
    unsigned p = (8363u * 16u * ntab[note % 12] >> (note / 12)) / c4;   /* the shift first in ST3; after: in tune */
    return p < PMIN ? PMIN : p > PMAX ? PMAX : (int)p;
}

static void say(char *why, const char *s) { trk_say(why, s); }

static int vibwave(int kind, int pos)
{
    pos &= 63;
    switch (kind & 3) {
    case 0: return pos < 32 ? trk_sine[pos] : -(int)trk_sine[pos - 32];
    case 1: return 255 - pos * 8;           /* ramp down */
    case 2: return pos < 32 ? 255 : -255;
    default: return (int)((pos * 73 + 41) & 511) - 256;     /* "random": a fixed scramble */
    }
}

/* the row's cells: for each channel present, the five bytes (note 255 none, ins, vol 255 none, cmd, info) */
static const unsigned char *rowstart(struct s3m *s, int pat, int row)
{
    if (pat < 0 || pat >= s->patnum || !s->rowoff[pat * 64]) return 0;
    return s->d + s->patoff[pat] + s->rowoff[pat * 64 + row];
}

/* one entry at p (bounds checked at load): -> the next, the cell into c[5] */
static const unsigned char *entry(const unsigned char *p, int *ch, unsigned char c[5])
{
    int w = *p++;
    *ch = w & 31;
    c[0] = 255; c[1] = 0; c[2] = 255; c[3] = 0; c[4] = 0;
    if (w & 32) { c[0] = p[0]; c[1] = p[1]; p += 2; }
    if (w & 64) { c[2] = p[0]; p++; }
    if (w & 128) { c[3] = p[0]; c[4] = p[1]; p += 2; }
    return p;
}

static void vslide(struct s3mch *c, int x, int y, int tick)
{
    /* D: x0 up, 0y down, on ticks after the first; xF and Fy (y, x not 0) fine, on the first only */
    if (y == 15 && x) { if (!tick) c->vol += x; }
    else if (x == 15 && y) { if (!tick) c->vol -= y; }
    else if (x) { if (tick) c->vol += x; }
    else if (tick) c->vol -= y;
    if (c->vol > 64) c->vol = 64;
    if (c->vol < 0) c->vol = 0;
}

static void pslide(struct s3m *s, struct s3mch *c, int d)
{
    c->per += d;
    int lo = s->flags & 16 ? 452 : PMIN, hi = s->flags & 16 ? 3424 : PMAX;
    if (c->per < lo) c->per = lo;
    if (c->per > hi) c->per = hi;
}

static void porta(struct s3mch *c)
{
    if (!c->ptarget || !c->per) return;
    if (c->per < c->ptarget) { c->per += c->pspeed * 4; if (c->per >= c->ptarget) c->per = c->ptarget; }
    else { c->per -= c->pspeed * 4; if (c->per <= c->ptarget) c->per = c->ptarget; }
}

static void vibrato(struct s3mch *c, int shift)
{
    c->oper = c->per + (vibwave(c->vwave, c->vpos) * c->vdepth >> shift);
    c->vpos = (c->vpos + c->vspeed) & 63;
}

static void start(struct s3m *s, struct s3mch *c, int note, int smp, int keep_vol)
{
    (void)keep_vol;
    if (smp > 0 && smp <= s->insnum) c->c4 = s->smp[smp - 1].c4spd;
    c->note = note;
    c->per = notepr(note, c->c4);
    c->trig = 1;
    c->offset = 0;
    c->retrig = 0;
    if (!(c->vwave & 4)) c->vpos = 0;
    if (!(c->twave & 4)) c->tpos = 0;
}

static void row_start(struct s3m *s)
{
    for (int i = 0; i < s->nch; i++) {
        s->c[i].eff = 0;
        s->c[i].prm = 0;
    }
    int pat = s->ord[s->pos];
    const unsigned char *p = rowstart(s, pat, s->row);
    if (!p) return;
    for (;;) {
        int chn;
        unsigned char x[5];
        if (!*p) break;
        p = entry(p, &chn, x);
        int k = s->chmap[chn];
        if (k < 0) continue;
        struct s3mch *c = &s->c[k];
        int note = x[0], ins = x[1], vol = x[2];
        c->eff = x[3];
        c->prm = x[4];
        int fx = c->eff, v = c->prm;
        if ((fx >= 4 && fx <= 6) || (fx >= 9 && fx <= 12) || fx == 17) {   /* D E F I J K L Q: 00 is the last */
            if (v) c->last = v; else v = c->prm = c->last;
        }
        if (fx == 19 && (v >> 4) == 0xD && (v & 15)) {          /* SDx: the note later */
            c->dnote = note; c->dsmp = ins; c->dvol = vol;
            continue;
        }
        if (ins > 0 && ins <= s->insnum) {
            c->smp = ins;
            c->vol = s->smp[ins - 1].vol;
            c->c4 = s->smp[ins - 1].c4spd;
        }
        if (note == 254) c->cut = 1;                            /* key off: the sample stops */
        else if (note < 255 && (note & 15) < 12 && c->smp) {
            int n = (note >> 4) * 12 + (note & 15);
            if (fx == 7 || fx == 12) c->ptarget = notepr(n, c->c4), c->note = n;   /* G, L: slide to it */
            else start(s, c, n, 0, 0);
        }
        if (vol <= 64) c->vol = vol;
        /* the first tick's effects */
        int hx = v >> 4, lx = v & 15;
        switch (fx) {
        case 1: if (v) s->speed = v; break;                                     /* A */
        case 2: s->brk = 1; s->jump = v; break;                                  /* B */
        case 3: s->brk = 1; s->brkrow = hx * 10 + lx > 63 ? 0 : hx * 10 + lx; break;   /* C (decimal) */
        case 4: vslide(c, hx, lx, 0); break;                                    /* D */
        case 5: if (hx == 15) pslide(s, c, lx * 4); else if (hx == 14) pslide(s, c, lx); break;    /* E fine */
        case 6: if (hx == 15) pslide(s, c, -lx * 4); else if (hx == 14) pslide(s, c, -lx); break;  /* F fine */
        case 7: if (v) c->pspeed = v; break;                                     /* G */
        case 8: case 21: if (hx) c->vspeed = hx; if (lx) c->vdepth = lx; break;  /* H, U */
        case 9: c->tremor = 0; break;                                            /* I */
        case 11: vslide(c, hx, lx, 0); break;                                   /* K: D on the first tick (fine) */
        case 12: vslide(c, hx, lx, 0); break;                                   /* L */
        case 15: if (c->trig) {                                                  /* O */
                struct s3msmp *sm = c->smp ? &s->smp[c->smp - 1] : 0;
                unsigned o = (unsigned)v << 8;
                if (sm && o >= sm->len && sm->looplen)                          /* past the end: as if looped */
                    o = sm->loop + (o - sm->loop) % sm->looplen;
                c->offset = (int)o;
            }
            break;
        case 18: if (hx) c->tspeed = hx; if (lx) c->tdepth = lx; break;          /* R */
        case 19:                                                                 /* S */
            switch (hx) {
            case 2: c->c4 = ftc4[lx]; if (c->note >= 0) c->per = notepr(c->note, c->c4); break;
            case 3: c->vwave = lx; break;
            case 4: c->twave = lx; break;
            case 8: c->pan = lx * 17; break;
            case 0xB:
                if (!lx) c->loopr = s->row;
                else if (!c->loopn) { c->loopn = lx; s->ploop = 1; s->plrow = c->loopr; }
                else if (--c->loopn) { s->ploop = 1; s->plrow = c->loopr; }
                break;
            case 0xC: if (!lx) c->cut = 1; break;
            case 0xE: if (!s->pdelayn) s->pdelay = lx; break;
            }
            break;
        case 20: if (v >= 0x20) s->tempo = v; break;                             /* T */
        case 22: s->gvol = v > 64 ? 64 : v; break;                               /* V */
        }
    }
}

/* the effects of a later tick */
static void running(struct s3m *s, struct s3mch *c)
{
    int v = c->prm, hx = v >> 4, lx = v & 15;
    switch (c->eff) {
    case 4: vslide(c, hx, lx, s->tick); break;
    case 5: if (v < 0xE0) pslide(s, c, v * 4); break;
    case 6: if (v < 0xE0) pslide(s, c, -v * 4); break;
    case 7: porta(c); break;
    case 8: vibrato(c, 5); break;
    case 21: vibrato(c, 7); break;
    case 9: {
        int on = hx + 1, off = lx + 1;
        c->tremor = (c->tremor + 1) % (on + off);
        if (c->tremor >= on) c->ovol = 0;
        break;
    }
    case 10: {
        int k = s->tick % 3;
        if (k && c->note >= 0) { int n = c->note + (k == 1 ? hx : lx); c->oper = notepr(n > 119 ? 119 : n, c->c4); }
        break;
    }
    case 11: vibrato(c, 5); vslide(c, hx, lx, s->tick); break;
    case 12: porta(c); vslide(c, hx, lx, s->tick); break;
    case 17:
        if (lx && ++c->retrig >= lx) {
            static const signed char add[16] = { 0, -1, -2, -4, -8, -16, 0, 0, 0, 1, 2, 4, 8, 16, 0, 0 };
            c->retrig = 0;
            c->trig = 1;
            if (hx == 6) c->vol = c->vol * 2 / 3;
            else if (hx == 7) c->vol /= 2;
            else if (hx == 14) c->vol = c->vol * 3 / 2;
            else if (hx == 15) c->vol *= 2;
            else c->vol += add[hx];
            if (c->vol < 0) c->vol = 0;
            if (c->vol > 64) c->vol = 64;
        }
        break;
    case 19:
        if (hx == 0xC && s->tick == lx) c->cut = 1;
        else if (hx == 0xD && s->tick == lx) {
            int ins = c->dsmp, note = c->dnote;
            if (ins > 0 && ins <= s->insnum) { c->smp = ins; c->vol = s->smp[ins - 1].vol; c->c4 = s->smp[ins - 1].c4spd; }
            if (note == 254) c->cut = 1;
            else if (note < 255 && (note & 15) < 12 && c->smp) start(s, c, (note >> 4) * 12 + (note & 15), 0, 0);
            if (c->dvol <= 64) c->vol = c->dvol;
        }
        break;
    }
    if (c->eff == 18) {                                                          /* R tremolo */
        int v2 = c->vol + (vibwave(c->twave, c->tpos) * c->tdepth >> 6);
        c->ovol = v2 < 0 ? 0 : v2 > 64 ? 64 : v2;
        c->tpos = (c->tpos + c->tspeed) & 63;
    }
}

static int nextpos(struct s3m *s, int p)
{
    for (int n = 0; n < s->ordnum; n++, p++) {
        if (p >= s->ordnum || s->ord[p] == 255) return -1;
        if (s->ord[p] != 254 && s->ord[p] < s->patnum) return p;
    }
    return -1;
}

static void next_row(struct s3m *s)
{
    if (s->pdelay) { s->pdelayn = s->pdelay; s->pdelay = 0; }
    if (s->pdelayn) { s->pdelayn--; s->rep = 1; return; }
    s->rep = 0;
    if (s->ploop) { s->ploop = 0; s->row = s->plrow; s->brk = 0; s->jump = -1; return; }
    int np = -2;
    if (s->brk) {
        if (s->jump >= 0) { if (s->jump <= s->pos) s->ended = 1; np = nextpos(s, s->jump); }
        else np = nextpos(s, s->pos + 1);
        s->row = s->brkrow;
        s->brk = 0;
        s->brkrow = 0;
        s->jump = -1;
    } else if (++s->row >= 64) {
        s->row = 0;
        np = nextpos(s, s->pos + 1);
    }
    if (np == -1) { s->ended = 1; np = nextpos(s, 0); s->row = 0; }
    if (np >= 0) s->pos = np;
}

static void s3m_tick(struct trk *t)
{
    struct s3m *s = t->st;
    t->pos = s->pos;
    t->row = s->row;
    t->tick = s->tick;
    t->pat = s->ord[s->pos];
    for (int i = 0; i < s->nch; i++) { s->c[i].trig = 0; s->c[i].cut = 0; }
    if (s->tick == 0 && !s->rep) row_start(s);
    for (int i = 0; i < s->nch; i++) {
        struct s3mch *c = &s->c[i];
        c->oper = c->ovol = -1;                 /* -1: the effects left them: the period and volume as they are now */
        if (s->tick || s->rep) running(s, c);
        if (c->oper < 0) c->oper = c->per;
        if (c->ovol < 0) c->ovol = c->vol;
        struct tvoice *v = &t->v[i];
        v->flags = (c->trig ? TV_TRIG : 0) | (c->cut ? TV_CUT : 0);
        if (c->cut && c->eff == 19) { v->flags &= ~TV_CUT; c->vol = 0; c->ovol = 0; }   /* SC: volume 0 */
        v->per = 0;
        v->hz16 = c->oper > 0 ? 229072896u / (unsigned)c->oper : 0;     /* 14,317,056 x 16 / period */
        v->vol = c->ovol * s->gvol / 64;
        v->pan = c->pan;
        if (c->trig && c->smp) {
            const struct s3msmp *sm = &s->smp[c->smp - 1];
            unsigned o = (unsigned)c->offset;
            v->sp = sm->d;
            if (sm->looplen) {
                unsigned end = sm->loop + sm->looplen;
                if (o >= end) o = sm->loop;
                v->start = o;
                v->len = end - o;
                v->loop = sm->loop;
                v->looplen = sm->looplen;
            } else {
                if (o > sm->len) o = sm->len;
                v->start = o;
                v->len = sm->len - o;
                v->loop = v->looplen = 0;
            }
        }
    }
    t->speed = s->speed;
    t->tempo = s->tempo;
    if (++s->tick >= s->speed) { s->tick = 0; next_row(s); }
    t->tnum = 5;
    t->tden = 2u * (unsigned)s->tempo;
    t->ended = s->ended;
    t->stop = s->stop;
}

static void s3m_go(struct trk *t, int pos)
{
    struct s3m *s = t->st;
    int p = nextpos(s, pos < 0 ? 0 : pos >= s->ordnum ? s->ordnum - 1 : pos);
    if (p < 0) p = nextpos(s, 0);
    if (p < 0) return;
    s->pos = p;
    s->row = s->tick = 0;
    s->brk = s->ploop = s->pdelay = s->pdelayn = s->rep = 0;
    s->jump = -1;
}

static int s3m_name(struct trk *t, int i, char *out, int n)
{
    struct s3m *s = t->st;
    int k = 0;
    if (i >= 0 && i < s->insnum)
        for (; k < 27 && k < n - 1 && s->smp[i].name[k]; k++) out[k] = s->smp[i].name[k];
    out[k] = 0;
    return k;
}

static int s3m_rows(struct trk *t, int pat) { (void)t; (void)pat; return 64; }

static void s3m_cell(struct trk *t, int pat, int row, int ch, char out[16])
{
    struct s3m *s = t->st;
    char nn[4] = "---";
    int ins = 0, vol = -1, fx = 0, prm = 0;
    const unsigned char *p = rowstart(s, pat, row);
    while (p && *p) {
        int chn;
        unsigned char x[5];
        p = entry(p, &chn, x);
        if (s->chmap[chn] != ch) continue;
        if (x[0] == 254) trk_notename(-2, nn);
        else if (x[0] < 255) trk_notename((x[0] >> 4) * 12 + (x[0] & 15) % 12, nn);
        ins = x[1];
        vol = x[2] <= 64 ? x[2] : -1;
        if (x[3] && x[3] <= 26) { fx = 'A' - 1 + x[3]; prm = x[4]; }
        break;
    }
    trk_cellfmt(out, nn, ins, vol, fx, prm);
}

static void s3m_done(struct trk *t)
{
    struct s3m *s = t->st;
    if (!s) return;
    trk_release(s->rowoff);
    trk_release(s->patoff);
    trk_release(s->smp);
    trk_release(s);
}

static const struct trkops s3mops = { s3m_tick, s3m_go, s3m_name, s3m_rows, s3m_cell, s3m_done };

int s3m_trk(struct trk *t, unsigned char *d, unsigned size, char *why)
{
    if (size < 0x60 || d[44] != 'S' || d[45] != 'C' || d[46] != 'R' || d[47] != 'M') return 0;
    if (d[29] != 16) { say(why, "an S3M of an unknown type"); return -1; }
    struct s3m *s = trk_alloc(sizeof *s);
    if (!s) { say(why, "no memory"); return -1; }
    t->st = s;
    t->ops = &s3mops;
    s->d = d;
    s->size = size;
    s->ordnum = (int)le16(d + 32);
    s->insnum = (int)le16(d + 34);
    s->patnum = (int)le16(d + 36);
    s->flags = (int)le16(d + 38);
    s->ffv = (int)le16(d + 42);
    s->gvol = d[48] > 64 ? 64 : d[48];
    s->speed = d[49] ? d[49] : 6;
    s->tempo = d[50] >= 32 ? d[50] : 125;
    int stereo = d[51] & 128;
    unsigned need = 0x60u + (unsigned)s->ordnum + 2u * (unsigned)(s->insnum + s->patnum);
    if (s->ordnum < 1 || s->ordnum > 256 || s->insnum > 99 || s->patnum > 256 || need > size) {
        say(why, "a damaged S3M (its counts)");
        goto bad;
    }
    s->ord = d + 0x60;
    for (int i = 0; i < S3M_CH; i++) {
        int cs = d[64 + i];
        s->chmap[i] = -1;
        if (cs < 16) {                                           /* 0-7 left, 8-15 right; adlib and off: none */
            s->chmap[i] = (signed char)s->nch;
            s->c[s->nch].pan = !stereo ? 128 : cs < 8 ? 48 : 208;
            s->c[s->nch].note = -1;
            s->nch++;
        }
    }
    if (!s->nch) { say(why, "an S3M without sample channels"); goto bad; }
    const unsigned char *pp = d + 0x60 + s->ordnum;
    s->smp = trk_alloc((unsigned)(s->insnum ? s->insnum : 1) * sizeof *s->smp);
    s->rowoff = trk_alloc((unsigned)(s->patnum ? s->patnum : 1) * 64u * sizeof *s->rowoff);
    s->patoff = trk_alloc((unsigned)(s->patnum ? s->patnum : 1) * sizeof *s->patoff);
    if (!s->smp || !s->rowoff || !s->patoff) { say(why, "no memory"); goto bad; }
    for (int i = 0; i < s->insnum; i++) {
        unsigned o = le16(pp + 2 * i) * 16u;
        struct s3msmp *m = &s->smp[i];
        if (!o) continue;
        if (o + 80 > size) { say(why, "a damaged S3M (an instrument past the end)"); goto bad; }
        const unsigned char *h = d + o;
        for (int k = 0; k < 27; k++) m->name[k] = (char)(h[48 + k] >= 32 && h[48 + k] < 127 ? h[48 + k] : h[48 + k] ? '.' : 0);
        if (h[0] != 1) continue;                                 /* adlib, or empty: silent */
        unsigned at = ((unsigned)h[13] << 16 | le16(h + 14)) * 16u, len = le32(h + 16);
        unsigned lb = le32(h + 20), le = le32(h + 24), fl = h[31];
        m->vol = h[28] > 64 ? 64 : h[28];
        m->c4spd = le16(h + 32);
        if (!m->c4spd) m->c4spd = 8363;
        if (h[30]) continue;                                     /* packed (DP30ADPCM): not played */
        int bytes = fl & 4 ? 2 : 1;
        if (at >= size) continue;
        if (len > (size - at) / (unsigned)bytes) len = (size - at) / (unsigned)bytes;   /* a cut file: what is there */
        unsigned char *sd = d + at;
        /* to 8-bit signed in place: unsigned (format 2) flipped, 16-bit taken by its high byte */
        if (bytes == 2)
            for (unsigned k = 0; k < len; k++) sd[k] = (unsigned char)(sd[2 * k + 1] ^ (s->ffv == 1 ? 0 : 0x80));
        else if (s->ffv != 1)
            for (unsigned k = 0; k < len; k++) sd[k] ^= 0x80;
        m->d = (const signed char *)sd;
        m->len = len;
        if ((fl & 1) && le > lb && lb < len) {
            if (le > len) le = len;
            m->loop = lb;
            m->looplen = le - lb;
            if (m->looplen < 2) m->looplen = 0;
        }
        if (m->looplen) m->len = m->loop + m->looplen;
    }
    pp += 2 * s->insnum;
    for (int i = 0; i < s->patnum; i++) {
        unsigned o = le16(pp + 2 * i) * 16u;
        if (!o) continue;
        if (o + 2 > size) { say(why, "a damaged S3M (a pattern past the end)"); goto bad; }
        unsigned plen = le16(d + o), q = o + 2, end = o + 2 + plen;
        if (end > size) end = size;
        for (int r = 0; r < 64; r++) {
            if (q - o > 0xFFFF) { say(why, "a damaged S3M (a pattern)"); goto bad; }
            s->rowoff[i * 64 + r] = (unsigned short)(q - o);
            for (;;) {
                if (q >= end) { say(why, "a damaged S3M (a pattern runs past its end)"); goto bad; }
                int w = d[q++];
                if (!w) break;
                q += (w & 32 ? 2u : 0u) + (w & 64 ? 1u : 0u) + (w & 128 ? 2u : 0u);
            }
        }
        s->patoff[i] = o;
    }
    t->fmt = TF_S3M;
    trk_say(t->kind, "Scream Tracker 3");
    for (int i = 0; i < 28 && d[i]; i++) t->title[i] = (char)(d[i] >= 32 && d[i] < 127 ? d[i] : '.');
    t->nch = s->nch;
    t->songlen = s->ordnum;
    t->ninst = s->insnum;
    t->rows = 64;
    s->jump = -1;
    s->pos = nextpos(s, 0);
    if (s->pos < 0) { say(why, "an S3M with nothing to play"); goto bad; }
    t->pat = s->ord[s->pos];
    t->speed = s->speed;
    t->tempo = s->tempo;
    t->tnum = 5;
    t->tden = 2u * (unsigned)s->tempo;
    for (int i = 0; i < s->nch; i++) t->v[i].pan = s->c[i].pan;
    return 1;
bad:
    s3m_done(t);
    t->st = 0;
    t->ops = 0;
    return -1;
}
