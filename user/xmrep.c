/* SPDX-License-Identifier: GPL-2.0-only */
/* FastTracker 2 modules (.xm) for the tracker player (DESIGN D56 section 3), written from the format's descriptions:
 * Triton's XM description as carried in Matti Hamalainen's "The 'Complete' XM module format specification" v0.81
 * (2001: the header, patterns, instruments, envelopes, the period and frequency formulas, the effects and their
 * memories, the volume column), and ProTracker's documented effects for what it says works "as in PT MOD"; no
 * player's source was read. Samples become 8-bit in place (16-bit by their high byte); a ping-pong loop is laid out
 * forward and backward once at load, so the mixer only ever loops forward. */
#include "trk.h"

#define XM_CH 32
#define XM_INS 128

struct xmsmp {
    const signed char *d;
    unsigned len, loop, looplen;        /* 8-bit sample bytes */
    int vol, fine, pan, rel;
    char name[23];
};
struct xmenv { unsigned short x[12], y[12]; int n, sus, ls, le, on; };
struct xmins {
    unsigned char map[96];
    int nsmp, first;                    /* its samples: smp[first .. first + nsmp - 1] */
    struct xmenv ve, pe;
    int vtype, vsweep, vdepth, vrate, fade;
    char name[23];
};
struct xmch {
    int note, ins, smp, per, vol, pan, fine;        /* note 0-based (C-0 = 0), real (relative note added) */
    int eff, prm, vcol;
    int m1, m2, m3, m4, m5, m9, mA, mE1, mE2, mEA, mEB, mH, mP, mR, mT, mX1, mX2;   /* the memories */
    int ptarget, gliss;
    int vpos, vwave, tpos, twave, tremor, retrig;
    int keyon, fadev, venv, penv, avpos, avsweep;   /* fadev 0..65536; the envelopes' frames */
    int loopr, loopn, dnote, dins, dvol;
    int oper, ovol, trig, cut, offset;
};
struct xm {
    const unsigned char *d;
    unsigned size;
    int songlen, restart, nch, npat, nins, linear, speed, tempo, gvol;
    unsigned char ord[256];
    unsigned *patoff;                   /* each pattern's packed data in the file (0: an empty one) */
    unsigned short *rowoff;             /* its rows' offsets from there, 256 a pattern */
    unsigned short *nrows;
    struct xmins *ins;
    struct xmsmp *smp;
    int nsmp;
    void **extra;                       /* the ping-pong copies */
    int nextra;
    struct xmch c[XM_CH];
    int tick, row, pos, brk, brkrow, jump, ploop, plrow, pdelay, pdelayn, rep, ended;
};

static unsigned short pow2[768];        /* 2^(i / 768) x 32768 */
static const unsigned short ptab[96] = {
    907, 900, 894, 887, 881, 875, 868, 862, 856, 850, 844, 838, 832, 826, 820, 814,
    808, 802, 796, 791, 785, 779, 774, 768, 762, 757, 752, 746, 741, 736, 730, 725,
    720, 715, 709, 704, 699, 694, 689, 684, 678, 675, 670, 665, 660, 655, 651, 646,
    640, 636, 632, 628, 623, 619, 614, 610, 604, 601, 597, 592, 588, 584, 580, 575,
    570, 567, 563, 559, 555, 551, 547, 543, 538, 535, 532, 528, 524, 520, 516, 513,
    508, 505, 502, 498, 494, 491, 487, 484, 480, 477, 474, 470, 467, 463, 460, 457 };

static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

/* the period of a 0-based note with a finetune -128..127: linear 7680 - note x 64 - fine / 2; Amiga: the table's
 * entry for the note and finetune (8 a semitone, interpolated by the finetune's low 4 bits) x 32 >> octave, so C-4
 * at finetune 0 is 1712, the rate 8363 Hz */
static int period(const struct xm *x, int note, int fine)
{
    if (note < 0) note = 0;
    if (note > 119) note = 119;
    if (x->linear) return 7680 - note * 64 - fine / 2;
    int f = fine + 128, i = 8 + (note % 12) * 8 + (f >> 4) - 8, oct = note / 12;
    if (i >= 96) { i -= 96; oct++; }
    int a = ptab[i], b = i + 1 < 96 ? ptab[i + 1] : ptab[0] / 2;
    return ((a * (16 - (f & 15)) + b * (f & 15)) * 2) >> oct;
}

/* the rate of a period in 1/16 Hz */
static unsigned rate16(const struct xm *x, int p)
{
    if (x->linear) {
        int e = 4608 - p, oct = e >= 0 ? e / 768 : -((-e + 767) / 768), fr = e - oct * 768;
        unsigned h = pow2[fr] * 33452u;         /* 8363 x 16 / 4 x 2^(fr / 768) x 32768: below 2^32 */
        oct -= 13;
        if (oct > 0) return oct > 2 || h > 0xFFFFFFFFu >> oct ? 0xFFFFFFFFu : h << oct;
        return -oct > 31 ? 0 : h >> -oct;
    }
    return p > 0 ? 229079296u / (unsigned)p : 0;       /* 8363 x 1712 x 16 / period */
}

static int wave(int kind, int pos)
{
    pos &= 63;
    switch (kind & 3) {
    case 0: return pos < 32 ? trk_sine[pos] : -(int)trk_sine[pos - 32];
    case 1: return pos < 32 ? 255 - pos * 8 : -(pos - 32) * 8;     /* ramp down (ProTracker's: from the top) */
    default: return pos < 32 ? 255 : -255;
    }
}

static int envval(const struct xmenv *e, int frame)
{
    if (e->n < 1) return 64;
    int i = 0;
    while (i + 1 < e->n && e->x[i + 1] <= frame) i++;
    if (i + 1 >= e->n || frame <= e->x[i]) return e->y[i];
    int dx = e->x[i + 1] - e->x[i], dy = e->y[i + 1] - e->y[i];
    return e->y[i] + (dx ? dy * (frame - e->x[i]) / dx : 0);
}

/* one tick of an envelope's frame: held at the sustain point while the key is down, looped */
static int envstep(const struct xmenv *e, int frame, int keyon)
{
    if (!e->on || e->n < 1) return frame;
    if (keyon && (e->on & 2) && e->sus < e->n && frame == e->x[e->sus]) return frame;
    frame++;
    if ((e->on & 4) && e->le < e->n && e->ls <= e->le && frame >= e->x[e->le]) frame = e->x[e->ls];
    if (frame > e->x[e->n - 1]) frame = e->x[e->n - 1];
    return frame;
}

/* the packed row's cells: p at a row; c[5] note, ins, vol, eff, prm */
static const unsigned char *cell(const unsigned char *p, unsigned char c[5])
{
    int b = *p++;
    c[0] = c[1] = c[2] = c[3] = c[4] = 0;
    if (b & 0x80) {
        for (int k = 0; k < 5; k++)
            if (b & (1 << k)) c[k] = *p++;
    } else {
        c[0] = (unsigned char)b;
        for (int k = 1; k < 5; k++) c[k] = *p++;
    }
    return p;
}

static const unsigned char *rowat(const struct xm *x, int pat, int row)
{
    if (pat >= x->npat || !x->patoff[pat] || row >= x->nrows[pat]) return 0;
    return x->d + x->patoff[pat] + x->rowoff[pat * 256 + row];
}

static void vclamp(struct xmch *c) { c->vol = c->vol < 0 ? 0 : c->vol > 64 ? 64 : c->vol; }

static void vslide(struct xmch *c, int prm)
{
    if (prm >> 4) c->vol += prm >> 4; else c->vol -= prm & 15;
    vclamp(c);
}

static void pslide(struct xmch *c, int d)
{
    c->per += d;
    if (c->per < 1) c->per = 1;
    if (c->per > 32000) c->per = 32000;
}

static void porta(struct xm *x, struct xmch *c)
{
    if (!c->ptarget) return;
    if (c->per < c->ptarget) { c->per += c->m3 * 4; if (c->per >= c->ptarget) c->per = c->ptarget; }
    else { c->per -= c->m3 * 4; if (c->per <= c->ptarget) c->per = c->ptarget; }
    c->oper = c->per;
    if (c->gliss && c->ins) {                    /* E31: the slide in semitones */
        int best = c->per, bd = 1 << 30;
        for (int n = 0; n < 120; n++) {
            int p = period(x, n, c->fine), dd = p > c->per ? p - c->per : c->per - p;
            if (dd < bd) { bd = dd; best = p; }
        }
        c->oper = best;
    }
}

static void vibrato(struct xmch *c)
{
    c->oper = c->per + (wave(c->vwave, c->vpos) * (c->m4 & 15) >> 5);
    c->vpos = (c->vpos + (c->m4 >> 4)) & 63;
}

static void trigger(struct xm *x, struct xmch *c, int note)
{
    c->note = note;
    c->per = period(x, note, c->fine);
    c->trig = 1;
    c->offset = 0;
    c->retrig = 0;
    if (!(c->vwave & 4)) c->vpos = 0;
    if (!(c->twave & 4)) c->tpos = 0;
}

static void envreset(struct xmch *c)
{
    c->keyon = 1;
    c->fadev = 65536;
    c->venv = c->penv = 0;
    c->avpos = c->avsweep = 0;
}

/* a cell's note and instrument (now, or delayed by EDx) */
static void noteins(struct xm *x, struct xmch *c, int note, int ins, int porta3)
{
    if (ins && ins <= x->nins) {
        c->ins = ins;
        const struct xmins *in = &x->ins[ins - 1];
        if (note >= 1 && note <= 96 && in->map[note - 1] < in->nsmp) c->smp = in->first + in->map[note - 1] + 1;
        if (c->smp && c->smp <= x->nsmp) {             /* "OldVol": the sample's volume and panning */
            c->vol = x->smp[c->smp - 1].vol;
            c->pan = x->smp[c->smp - 1].pan;
        }
        envreset(c);
    } else if (ins) {
        c->cut = 1;                                    /* an instrument that does not exist: silence */
        return;
    }
    if (note == 97) { c->keyon = 0; return; }
    if (note < 1 || note > 96 || !c->ins) return;
    const struct xmins *in = &x->ins[c->ins - 1];
    int s = in->nsmp ? in->map[note - 1] : 255;
    if (s >= in->nsmp) { c->cut = 1; return; }
    c->smp = in->first + s + 1;
    const struct xmsmp *sm = &x->smp[c->smp - 1];
    int real = note - 1 + sm->rel;
    if (real < 0 || real > 119) { c->cut = 1; return; }
    c->fine = sm->fine;
    if (porta3) { c->ptarget = period(x, real, c->fine); c->note = real; return; }
    if (!ins) envreset(c);
    trigger(x, c, real);
}

static void row_start(struct xm *x)
{
    int pat = x->ord[x->pos];
    const unsigned char *p = rowat(x, pat, x->row);
    for (int i = 0; i < x->nch; i++) {
        struct xmch *c = &x->c[i];
        unsigned char k[5] = { 0, 0, 0, 0, 0 };
        if (p) p = cell(p, k);
        int note = k[0], ins = k[1], vc = k[2], fx = k[3], v = k[4], hx = v >> 4, lx = v & 15;
        c->eff = fx;
        c->prm = v;
        c->vcol = vc;
        int p3 = fx == 3 || fx == 5 || (vc >= 0xF0);
        if (fx == 0xE && hx == 0xD && lx) { c->dnote = note; c->dins = ins; c->dvol = vc; }
        else noteins(x, c, note, ins, p3);
        if (fx == 9 && c->trig) {                        /* 9xx: with a note only; it remembers */
            if (v) c->m9 = v;
            c->offset = c->m9 << 8;
        }
        /* the volume column (before the effect) */
        if (!(fx == 0xE && hx == 0xD && lx)) {
            if (vc >= 0x10 && vc <= 0x50) c->vol = vc - 0x10;
            else if (vc >= 0x80 && vc < 0x90) { c->vol -= vc & 15; vclamp(c); }
            else if (vc >= 0x90 && vc < 0xA0) { c->vol += vc & 15; vclamp(c); }
            else if (vc >= 0xA0 && vc < 0xB0) c->m4 = (c->m4 & 15) | (vc & 15) << 4;
            else if (vc >= 0xB0 && vc < 0xC0) { if (vc & 15) c->m4 = (c->m4 & 0xF0) | (vc & 15); }
            else if (vc >= 0xC0 && vc < 0xD0) c->pan = (vc & 15) * 17;
            else if (vc >= 0xF0 && (vc & 15)) c->m3 = (vc & 15) << 4;
        }
        switch (fx) {
        case 1: if (v) c->m1 = v; break;
        case 2: if (v) c->m2 = v; break;
        case 3: if (v) c->m3 = v; break;
        case 4: if (hx) c->m4 = (c->m4 & 15) | hx << 4; if (lx) c->m4 = (c->m4 & 0xF0) | lx; break;
        case 5: case 6: case 0xA: if (v) c->mA = v; break;
        case 7: if (v) c->m5 = v; break;
        case 8: c->pan = v; break;
        case 0xB: x->brk = 1; x->jump = v; break;
        case 0xC: c->vol = v > 64 ? 64 : v; break;
        case 0xD: { int r = hx * 10 + lx; x->brk = 1; x->brkrow = r; break; }
        case 0xE:
            switch (hx) {
            case 1: if (lx) c->mE1 = lx; pslide(c, -c->mE1 * 4); break;
            case 2: if (lx) c->mE2 = lx; pslide(c, c->mE2 * 4); break;
            case 3: c->gliss = lx; break;
            case 4: c->vwave = lx; break;
            case 5:                                         /* FastTracker's: 8 is none (D56 review); the row's note too */
                c->fine = (lx - 8) * 16;
                if (c->trig) c->per = period(x, c->note, c->fine);
                break;
            case 6:
                if (!lx) c->loopr = x->row;
                else if (!c->loopn) { c->loopn = lx; x->ploop = 1; x->plrow = c->loopr; }
                else if (--c->loopn) { x->ploop = 1; x->plrow = c->loopr; }
                break;
            case 7: c->twave = lx; break;
            case 0xA: if (lx) c->mEA = lx; c->vol += c->mEA; vclamp(c); break;
            case 0xB: if (lx) c->mEB = lx; c->vol -= c->mEB; vclamp(c); break;
            case 0xC: if (!lx) c->vol = 0; break;
            case 0xE: if (!x->pdelayn) x->pdelay = lx; break;
            }
            break;
        case 0xF: if (v && v < 32) x->speed = v; else if (v >= 32) x->tempo = v; break;
        case 16: x->gvol = v > 64 ? 64 : v; break;                      /* G */
        case 17: if (v) c->mH = v; break;                               /* H */
        case 20: if (!v) c->keyon = 0; break;                           /* K00: now */
        case 21: c->venv = c->penv = v; break;                          /* L */
        case 25: if (v) c->mP = v; break;                               /* P */
        case 27: if (v) c->mR = v; break;                               /* R */
        case 29: if (v) c->mT = v; c->tremor = 0; break;                /* T */
        case 33:                                                        /* X */
            if (hx == 1) { if (lx) c->mX1 = lx; pslide(c, -c->mX1); }
            else if (hx == 2) { if (lx) c->mX2 = lx; pslide(c, c->mX2); }
            break;
        }
    }
}

static void running(struct xm *x, struct xmch *c)
{
    int v = c->prm, hx = v >> 4, lx = v & 15, vc = c->vcol;
    if (vc >= 0x60 && vc < 0x70) { c->vol -= vc & 15; vclamp(c); }
    else if (vc >= 0x70 && vc < 0x80) { c->vol += vc & 15; vclamp(c); }
    else if (vc >= 0xB0 && vc < 0xC0) vibrato(c);
    else if (vc >= 0xD0 && vc < 0xE0) { c->pan -= vc & 15; if (c->pan < 0) c->pan = 0; }
    else if (vc >= 0xE0 && vc < 0xF0) { c->pan += vc & 15; if (c->pan > 255) c->pan = 255; }
    else if (vc >= 0xF0) porta(x, c);
    switch (c->eff) {
    case 0:
        if (v && x->tick % 3) {
            int n = c->note + (x->tick % 3 == 1 ? hx : lx);
            c->oper = period(x, n > 119 ? 119 : n, c->fine);
        }
        break;
    case 1: pslide(c, -c->m1 * 4); c->oper = c->per; break;
    case 2: pslide(c, c->m2 * 4); c->oper = c->per; break;
    case 3: porta(x, c); break;
    case 4: vibrato(c); break;
    case 5: porta(x, c); vslide(c, c->mA); break;
    case 6: vibrato(c); vslide(c, c->mA); break;
    case 7: {
        int d = wave(c->twave, c->tpos) * (c->m5 & 15) >> 6, nv = c->vol + d;
        c->ovol = nv < 0 ? 0 : nv > 64 ? 64 : nv;
        c->tpos = (c->tpos + (c->m5 >> 4)) & 63;
        break;
    }
    case 0xA: vslide(c, c->mA); break;
    case 0xE:
        if (hx == 9 && lx && x->tick % lx == 0) { c->trig = 1; c->offset = 0; envreset(c); }
        else if (hx == 0xC && x->tick == lx) c->vol = 0;
        else if (hx == 0xD && x->tick == lx) {
            int p3 = c->dvol >= 0xF0;
            noteins(x, c, c->dnote, c->dins, p3);
            if (c->dvol >= 0x10 && c->dvol <= 0x50) c->vol = c->dvol - 0x10;
        }
        break;
    case 17:                                                            /* H: global volume slide */
        if (c->mH >> 4) x->gvol += c->mH >> 4; else x->gvol -= c->mH & 15;
        x->gvol = x->gvol < 0 ? 0 : x->gvol > 64 ? 64 : x->gvol;
        break;
    case 20: if (x->tick == v) c->keyon = 0; break;
    case 25:
        if (c->mP >> 4) c->pan += c->mP >> 4; else c->pan -= c->mP & 15;
        c->pan = c->pan < 0 ? 0 : c->pan > 255 ? 255 : c->pan;
        break;
    case 27: {                                                          /* R: multi retrig, the S3M's table */
        int y = c->mR & 15, k = c->mR >> 4;
        if (y && ++c->retrig >= y) {
            static const signed char add[16] = { 0, -1, -2, -4, -8, -16, 0, 0, 0, 1, 2, 4, 8, 16, 0, 0 };
            c->retrig = 0;
            c->trig = 1;
            c->offset = 0;
            if (k == 6) c->vol = c->vol * 2 / 3;
            else if (k == 7) c->vol /= 2;
            else if (k == 14) c->vol = c->vol * 3 / 2;
            else if (k == 15) c->vol *= 2;
            else c->vol += add[k];
            vclamp(c);
        }
        break;
    }
    case 29: {
        int on = (c->mT >> 4) + 1, off = (c->mT & 15) + 1;
        c->tremor = (c->tremor + 1) % (on + off);
        if (c->tremor >= on) c->ovol = 0;
        break;
    }
    }
}

static void next_row(struct xm *x)
{
    if (x->pdelay) { x->pdelayn = x->pdelay; x->pdelay = 0; }
    if (x->pdelayn) { x->pdelayn--; x->rep = 1; return; }
    x->rep = 0;
    if (x->ploop) { x->ploop = 0; x->row = x->plrow; x->brk = 0; x->jump = -1; return; }
    int rows = x->ord[x->pos] < x->npat ? x->nrows[x->ord[x->pos]] : 64;
    if (x->brk) {
        int np = x->jump >= 0 ? x->jump : x->pos + 1;
        if (x->jump >= 0 && x->jump <= x->pos) x->ended = 1;
        x->pos = np;
        x->row = x->brkrow;
        x->brk = 0;
        x->brkrow = 0;
        x->jump = -1;
    } else if (++x->row >= rows) {
        x->row = 0;
        x->pos++;
    }
    if (x->pos >= x->songlen) { x->pos = x->restart < x->songlen ? x->restart : 0; x->ended = 1; }
    int r2 = x->ord[x->pos] < x->npat ? x->nrows[x->ord[x->pos]] : 64;
    if (x->row >= r2) x->row = 0;
}

static void xm_tick(struct trk *t)
{
    struct xm *x = t->st;
    t->pos = x->pos;
    t->row = x->row;
    t->tick = x->tick;
    t->pat = x->ord[x->pos];
    t->rows = x->ord[x->pos] < x->npat ? x->nrows[x->ord[x->pos]] : 64;
    for (int i = 0; i < x->nch; i++) { x->c[i].trig = 0; x->c[i].cut = 0; }
    if (x->tick == 0 && !x->rep) row_start(x);
    for (int i = 0; i < x->nch; i++) {
        struct xmch *c = &x->c[i];
        c->oper = c->ovol = -1;                 /* -1: the effects left them: the period and volume as they are now */
        if (x->tick || x->rep) running(x, c);
        if (c->oper < 0) c->oper = c->per;
        if (c->ovol < 0) c->ovol = c->vol;
        struct tvoice *v = &t->v[i];
        v->flags = (c->trig ? TV_TRIG : 0) | (c->cut ? TV_CUT : 0);
        int vol = c->ovol, pan = c->pan, per = c->oper;
        if (c->ins && c->ins <= x->nins) {
            const struct xmins *in = &x->ins[c->ins - 1];
            if (in->ve.on & 1) {
                vol = vol * envval(&in->ve, c->venv) / 64;
                c->venv = envstep(&in->ve, c->venv, c->keyon);
                if (!c->keyon) { c->fadev -= in->fade; if (c->fadev < 0) c->fadev = 0; }
                vol = vol * c->fadev >> 16;             /* 64 x 65,536 fits */
            } else if (!c->keyon)
                vol = 0;                                /* a key off without an envelope: the note ends */
            if (in->pe.on & 1) {
                int e = envval(&in->pe, c->penv), a = pan - 128;
                pan += (e - 32) * (128 - (a < 0 ? -a : a)) / 32;
                c->penv = envstep(&in->pe, c->penv, c->keyon);
            }
            if (in->vdepth && in->vrate) {              /* the instrument's vibrato, swept in */
                int ramp = in->vsweep ? (c->avsweep < in->vsweep ? c->avsweep * 256 / in->vsweep : 256) : 256;
                if (c->keyon && c->avsweep < in->vsweep) c->avsweep++;
                int w = in->vtype == 1 ? (c->avpos & 128 ? -255 : 255) : in->vtype == 2 ? 255 - (c->avpos & 255) * 2 :
                        in->vtype == 3 ? (c->avpos & 255) * 2 - 255 : wave(0, c->avpos >> 2);
                per += w * in->vdepth * ramp >> 14;
                c->avpos = (c->avpos + in->vrate) & 255;
            }
        }
        v->per = 0;
        v->hz16 = per > 0 || x->linear ? rate16(x, per) : 0;
        v->vol = vol * x->gvol / 64;
        v->pan = pan < 0 ? 0 : pan > 255 ? 255 : pan;
        if (c->trig && c->smp && c->smp <= x->nsmp) {
            const struct xmsmp *sm = &x->smp[c->smp - 1];
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
                if (o >= sm->len) v->flags = TV_CUT;    /* an offset past the end: nothing */
                else { v->start = o; v->len = sm->len - o; v->loop = v->looplen = 0; }
            }
            if (!sm->d) v->flags = TV_CUT;
        }
    }
    t->speed = x->speed;
    t->tempo = x->tempo;
    if (++x->tick >= x->speed) { x->tick = 0; next_row(x); }
    t->tnum = 5;
    t->tden = 2u * (unsigned)x->tempo;
    t->ended = x->ended;
}

static void xm_go(struct trk *t, int pos)
{
    struct xm *x = t->st;
    x->pos = pos < 0 ? 0 : pos >= x->songlen ? x->songlen - 1 : pos;
    x->row = x->tick = 0;
    x->brk = x->ploop = x->pdelay = x->pdelayn = x->rep = 0;
    x->jump = -1;
}

static int xm_name(struct trk *t, int i, char *out, int n)
{
    struct xm *x = t->st;
    int k = 0;
    if (i >= 0 && i < x->nins)
        for (; k < 22 && k < n - 1 && x->ins[i].name[k]; k++) out[k] = x->ins[i].name[k];
    out[k] = 0;
    return k;
}

static int xm_rows(struct trk *t, int pat)
{
    struct xm *x = t->st;
    return pat >= 0 && pat < x->npat ? x->nrows[pat] : 64;
}

static void xm_cell(struct trk *t, int pat, int row, int ch, char out[16])
{
    struct xm *x = t->st;
    const unsigned char *p = rowat(x, pat, row);
    unsigned char k[5] = { 0, 0, 0, 0, 0 };
    for (int i = 0; p && i <= ch; i++) p = cell(p, k);
    char nn[4];
    trk_notename(k[0] >= 1 && k[0] <= 96 ? k[0] - 1 : k[0] == 97 ? -2 : -1, nn);
    int fx = k[3] || k[4] ? (k[3] < 10 ? '0' + k[3] : k[3] < 36 ? 'A' + k[3] - 10 : '?') : 0;
    trk_cellfmt(out, nn, k[1], k[2] >= 0x10 && k[2] <= 0x50 ? k[2] - 0x10 : -1, fx, k[4]);
}

static void xm_done(struct trk *t)
{
    struct xm *x = t->st;
    if (!x) return;
    for (int i = 0; i < x->nextra; i++) trk_release(x->extra[i]);
    trk_release(x->extra);
    trk_release(x->patoff);
    trk_release(x->rowoff);
    trk_release(x->nrows);
    trk_release(x->ins);
    trk_release(x->smp);
    trk_release(x);
}

static const struct trkops xmops = { xm_tick, xm_go, xm_name, xm_rows, xm_cell, xm_done };

static void name22(char *o, const unsigned char *s)
{
    int k = 0;
    for (; k < 22 && s[k]; k++) o[k] = (char)(s[k] >= 32 && s[k] < 127 ? s[k] : '.');
    o[k] = 0;
}

static void envload(struct xmenv *e, const unsigned char *pts, int n, int sus, int ls, int le, int type)
{
    e->n = n > 12 ? 12 : n;
    for (int i = 0; i < e->n; i++) {
        e->x[i] = (unsigned short)le16(pts + 4 * i);
        e->y[i] = (unsigned short)(le16(pts + 4 * i + 2) > 64 ? 64 : le16(pts + 4 * i + 2));
        if (i && e->x[i] < e->x[i - 1]) e->x[i] = e->x[i - 1];      /* points out of order: held */
    }
    e->sus = sus;
    e->ls = ls;
    e->le = le;
    e->on = e->n >= 1 ? type & 7 : 0;
}

int xm_trk(struct trk *t, unsigned char *d, unsigned size, char *why)
{
    static const char id[] = "Extended Module: ";
    if (size < 80) return 0;
    for (int i = 0; i < 17; i++) if (d[i] != (unsigned char)id[i]) return 0;
    if (!pow2[0]) {
        unsigned long long r = 1ull << 30;
        for (int i = 0; i < 768; i++) {
            pow2[i] = (unsigned short)((r + (1 << 14)) >> 15 > 65535 ? 65535 : (r + (1 << 14)) >> 15);
            r = (r * 1074711351ull) >> 30;
        }
    }
    if (le16(d + 58) < 0x104) { trk_say(why, "an XM older than version 1.04"); return -1; }
    struct xm *x = trk_alloc(sizeof *x);
    if (!x) { trk_say(why, "no memory"); return -1; }
    t->st = x;
    t->ops = &xmops;
    x->d = d;
    x->size = size;
    unsigned hs = le32(d + 60), o;
    x->songlen = (int)le16(d + 64);
    x->restart = (int)le16(d + 66);
    x->nch = (int)le16(d + 68);
    x->npat = (int)le16(d + 70);
    x->nins = (int)le16(d + 72);
    x->linear = le16(d + 74) & 1;
    x->speed = le16(d + 76) ? (int)le16(d + 76) : 6;
    x->tempo = le16(d + 78) >= 32 ? (int)le16(d + 78) : 125;
    if (x->speed > 31) x->speed = 31;
    if (x->tempo > 255) x->tempo = 255;
    x->gvol = 64;
    if (hs < 20 || hs > size - 60 || x->songlen < 1 || x->songlen > 256 || x->nch < 1 || x->nch > XM_CH ||
        x->npat > 256 || x->nins > XM_INS || 80u + 256u > size) {
        trk_say(why, x->nch > XM_CH ? "an XM of more than 32 channels" : "a damaged XM (its header)");
        goto bad;
    }
    for (int i = 0; i < 256; i++) x->ord[i] = d[80 + i];
    x->patoff = trk_alloc((unsigned)(x->npat + 1) * sizeof *x->patoff);
    x->rowoff = trk_alloc((unsigned)(x->npat + 1) * 256u * sizeof *x->rowoff);
    x->nrows = trk_alloc((unsigned)(x->npat + 1) * sizeof *x->nrows);
    if (!x->patoff || !x->rowoff || !x->nrows) { trk_say(why, "no memory"); goto bad; }
    o = 60 + hs;
    for (int i = 0; i < x->npat; i++) {
        if (o + 9 > size) { trk_say(why, "a damaged XM (a pattern past the end)"); goto bad; }
        unsigned phl = le32(d + o), rows = le16(d + o + 5), psz = le16(d + o + 7);
        if (phl < 9 || phl > size - o || rows < 1 || rows > 256) { trk_say(why, "a damaged XM (a pattern's header)"); goto bad; }
        o += phl;
        x->nrows[i] = (unsigned short)rows;
        if (!psz) continue;                             /* an empty pattern */
        if (psz > size - o) { trk_say(why, "a damaged XM (a pattern past the end)"); goto bad; }
        x->patoff[i] = o;
        unsigned q = 0;
        for (unsigned r = 0; r < rows; r++) {
            x->rowoff[i * 256 + r] = (unsigned short)q;
            for (int c = 0; c < x->nch; c++) {
                if (q >= psz) { trk_say(why, "a damaged XM (a pattern runs past its end)"); goto bad; }
                int b = d[o + q];
                unsigned n = b & 0x80 ? 1u + (unsigned)((b & 1) + (b >> 1 & 1) + (b >> 2 & 1) + (b >> 3 & 1) + (b >> 4 & 1)) : 5u;
                if (q + n > psz) { trk_say(why, "a damaged XM (a pattern runs past its end)"); goto bad; }
                q += n;
            }
        }
        o += psz;
    }
    /* an order naming a pattern not saved (FT2 leaves out empty ones at the end) plays 64 empty rows */
    x->ins = trk_alloc((unsigned)(x->nins ? x->nins : 1) * sizeof *x->ins);
    if (!x->ins) { trk_say(why, "no memory"); goto bad; }
    /* two passes: count the samples, then read them */
    for (int pass = 0; pass < 2; pass++) {
        unsigned io = o;
        int ns = 0;
        for (int i = 0; i < x->nins; i++) {
            if (io + 29 > size) {
                if (pass) break;
                trk_say(why, "a damaged XM (an instrument past the end)");
                goto bad;
            }
            unsigned ihs = le32(d + io), n = le16(d + io + 27);
            struct xmins *in = &x->ins[i];
            if (ihs < 29 || ihs > size - io || n > 16 || (n && ihs < 243)) {
                trk_say(why, "a damaged XM (an instrument's header)");
                goto bad;
            }
            if (pass) {
                name22(in->name, d + io + 4);
                in->nsmp = (int)n;
                in->first = ns;
            }
            if (!n) { io += ihs; continue; }
            const unsigned char *h = d + io;
            unsigned shs = le32(h + 29);
            if (shs < 40 || shs > 256) { trk_say(why, "a damaged XM (a sample's header size)"); goto bad; }
            if (pass) {
                for (int k = 0; k < 96; k++) in->map[k] = h[33 + k] < 16 ? h[33 + k] : 255;
                envload(&in->ve, h + 129, h[225], h[227], h[228], h[229], h[233]);
                envload(&in->pe, h + 177, h[226], h[230], h[231], h[232], h[234]);
                in->vtype = h[235]; in->vsweep = h[236]; in->vdepth = h[237] & 15; in->vrate = h[238] & 63;
                in->fade = (int)(le16(h + 239) & 0xFFF);
            }
            unsigned so = io + ihs, dat = so + n * shs;
            if (dat > size) { trk_say(why, "a damaged XM (samples past the end)"); goto bad; }
            for (unsigned k = 0; k < n; k++) {
                const unsigned char *s = d + so + k * shs;
                unsigned len = le32(s), ls = le32(s + 4), ll = le32(s + 8), ty = s[14];
                unsigned avail = size - dat;
                if (len > avail) len = avail;           /* a cut file: what is there */
                if (pass) {
                    struct xmsmp *m = &x->smp[ns];
                    unsigned char *sd = d + dat;
                    int b16 = ty & 16;
                    if (b16) {                          /* delta-coded 16-bit words -> their high bytes */
                        int old = 0;
                        unsigned nn = len / 2;
                        for (unsigned j = 0; j < nn; j++) {
                            old = (short)(old + (short)le16(sd + 2 * j));
                            sd[j] = (unsigned char)((unsigned)old >> 8);
                        }
                        len = nn; ls /= 2; ll /= 2;
                    } else {
                        unsigned char old = 0;
                        for (unsigned j = 0; j < len; j++) { old = (unsigned char)(old + sd[j]); sd[j] = old; }
                    }
                    m->d = (const signed char *)sd;
                    m->len = len;
                    m->vol = s[12] > 64 ? 64 : s[12];
                    m->fine = (signed char)s[13];
                    m->pan = s[15];
                    m->rel = (signed char)s[16];
                    name22(m->name, s + 18);
                    if ((ty & 3) && ll >= 2 && ls < len) {
                        if (ll > len - ls) ll = len - ls;
                        m->loop = ls;
                        m->looplen = ll;
                        m->len = ls + ll;
                        if ((ty & 3) == 2 && ll >= 2) {     /* ping-pong: forward then backward, once, in a copy */
                            signed char *pp = trk_alloc(ls + 2 * ll);
                            void **ne = trk_alloc((unsigned)(x->nextra + 1) * sizeof *ne);
                            if (!pp || !ne) { trk_release(pp); trk_release(ne); trk_say(why, "no memory"); goto bad; }
                            for (int j = 0; j < x->nextra; j++) ne[j] = x->extra[j];
                            trk_release(x->extra);
                            x->extra = ne;
                            x->extra[x->nextra++] = pp;
                            for (unsigned j = 0; j < ls + ll; j++) pp[j] = m->d[j];
                            for (unsigned j = 0; j < ll; j++) pp[ls + ll + j] = m->d[ls + ll - 1 - j];
                            m->d = pp;
                            m->looplen = 2 * ll;
                            m->len = ls + 2 * ll;
                        }
                    }
                }
                ns++;
                dat += le32(s) > avail ? avail : le32(s);
            }
            io = dat;
        }
        if (!pass) {
            x->nsmp = ns;
            x->smp = trk_alloc((unsigned)(ns ? ns : 1) * sizeof *x->smp);
            if (!x->smp) { trk_say(why, "no memory"); goto bad; }
        }
    }
    t->fmt = TF_XM;
    trk_say(t->kind, x->linear ? "FastTracker 2" : "FastTracker 2 (Amiga)");
    name22(t->title, d + 17);
    t->nch = x->nch;
    t->songlen = x->songlen;
    t->ninst = x->nins;
    t->rows = x->ord[0] < x->npat ? x->nrows[x->ord[0]] : 64;
    t->pat = x->ord[0];
    t->speed = x->speed;
    t->tempo = x->tempo;
    t->tnum = 5;
    t->tden = 2u * (unsigned)x->tempo;
    x->jump = -1;
    for (int i = 0; i < x->nch; i++) { x->c[i].pan = 128; x->c[i].note = -1; t->v[i].pan = 128; }
    return 1;
bad:
    xm_done(t);
    t->st = 0;
    t->ops = 0;
    return -1;
}
