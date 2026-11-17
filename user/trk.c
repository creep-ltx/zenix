/* SPDX-License-Identifier: GPL-2.0-only */
/* The tracker player's replayers behind one face (DESIGN D56): the format's detection, the shared helpers, and the
 * ProTracker replayer (user/modrep.c, D40) wrapped for it, 4 to 32 channels. */
#include "trk.h"
#include "modrep.h"

#ifdef TRK_HOST
#include <stdlib.h>
void *trk_alloc(unsigned n) { return calloc(1, n ? n : 1); }
void trk_release(void *p) { free(p); }
#else
#include "libz.h"
void *trk_alloc(unsigned n)
{
    void *p = malloc(n ? n : 1);
    if (p) memset(p, 0, n);
    return p;
}
void trk_release(void *p) { free(p); }
#endif

const unsigned short trk_sine[32] = {
    0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24 };

void trk_say(char *why, const char *s)
{
    int i = 0;
    for (; s[i] && i < 63; i++) why[i] = s[i];
    why[i] = 0;
}

void trk_notename(int note, char out[4])
{
    static const char nn[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
    if (note < 0 || note > 119) {
        out[0] = out[1] = out[2] = note == -2 ? '=' : '-';
        out[3] = 0;
        return;
    }
    out[0] = nn[(note % 12) * 2];
    out[1] = nn[(note % 12) * 2 + 1];
    out[2] = (char)('0' + note / 12);
    out[3] = 0;
}

unsigned trk_ticklen(const struct trk *t, unsigned hz)
{
    /* hz x tnum / tden without overflow: hz (at most a few million) x tnum (at most 33) fits, tden is >= 1 */
    unsigned den = t->tden ? t->tden : 1;
    return (unsigned)(((unsigned long long)hz * t->tnum + den / 2) / den);
}

void trk_free(struct trk *t)
{
    if (t->ops && t->ops->done) t->ops->done(t);
    t->st = 0;
    t->ops = 0;
}

int trk_load(struct trk *t, unsigned char *d, unsigned size, char *why)
{
    unsigned char *z = (unsigned char *)t;
    for (unsigned i = 0; i < sizeof *t; i++) z[i] = 0;
    int r;
    if ((r = xm_trk(t, d, size, why)) != 0) return r > 0 ? 0 : -1;
    if ((r = s3m_trk(t, d, size, why)) != 0) return r > 0 ? 0 : -1;
    if ((r = med_trk(t, d, size, why)) != 0) return r > 0 ? 0 : -1;
    r = mod_trk(t, d, size, why);     /* last: the old 15-sample format has no tag, so it takes what is left */
    return r > 0 ? 0 : -1;
}

/* ---- ProTracker and FastTracker's MODs through modrep.c ---- */

static void mod_out(struct trk *t)
{
    struct mod *m = t->st;
    t->speed = m->speed;
    t->tempo = m->bpm;
    t->tnum = 5;
    t->tden = 2u * (unsigned)m->bpm;
    t->ended = m->ended;
    t->stop = m->stop;
}

static void tmod_tick(struct trk *t)
{
    struct mod *m = t->st;
    t->pos = m->pos;
    t->row = m->row;
    t->tick = m->tick;
    t->pat = m->order[m->pos];
    mod_tick(m);
    for (int c = 0; c < m->nch; c++) {
        const struct modout *o = &m->out[c];
        struct tvoice *v = &t->v[c];
        v->flags = (o->flags & MOD_TRIG ? TV_TRIG : 0) | (o->flags & MOD_LOOP ? TV_LOOP : 0);
        v->per = o->period;
        v->hz16 = 0;
        v->vol = o->vol;
        v->sp = t->bank;
        v->start = o->start;
        v->len = o->len;
        v->loop = o->loop;
        v->looplen = o->looplen;
    }
    mod_out(t);
}

static void tmod_go(struct trk *t, int pos) { mod_goto(t->st, pos); }

static int tmod_name(struct trk *t, int i, char *out, int n)
{
    const unsigned char *h = t->st ? ((struct mod *)t->st)->pat : 0;     /* the file: patterns at 1084 (600) */
    struct mod *m = t->st;
    h -= m->nsmp == 31 ? 1084 : 600;
    int k = 0;
    if (i >= 0 && i < m->nsmp)
        for (; k < 22 && k < n - 1 && h[20 + i * 30 + k]; k++) {
            unsigned char c = h[20 + i * 30 + k];
            out[k] = (char)(c >= 32 && c < 127 ? c : '.');
        }
    out[k] = 0;
    return k;
}

static int tmod_rows(struct trk *t, int pat) { (void)t; (void)pat; return 64; }

static void hex2(char *o, int v)
{
    static const char hx[] = "0123456789ABCDEF";
    o[0] = hx[(v >> 4) & 15];
    o[1] = hx[v & 15];
}

void trk_cellfmt(char out[16], const char *note, int ins, int vol, int eff, int prm)
{
    for (int i = 0; i < 3; i++) out[i] = note[i];
    out[3] = out[6] = out[9] = ' ';
    if (ins > 0) hex2(out + 4, ins); else out[4] = out[5] = '.';
    if (vol >= 0) { out[7] = (char)('0' + vol / 10 % 10); out[8] = (char)('0' + vol % 10); } else out[7] = out[8] = '.';
    if (eff > 0 || prm > 0) { out[10] = (char)eff; hex2(out + 11, prm); } else out[10] = out[11] = out[12] = '.';
    out[13] = 0;
}

static void tmod_cell(struct trk *t, int pat, int row, int ch, char out[16])
{
    struct mod *m = t->st;
    unsigned rowb = 4u * (unsigned)m->nch;
    const unsigned char *b = m->pat + (unsigned)pat * 64u * rowb + (unsigned)row * rowb + (unsigned)ch * 4u;
    int smp = (b[0] & 0xF0) | b[2] >> 4, per = (b[0] & 15) << 8 | b[1];
    char nn[4];
    mod_notename(per, nn);
    trk_cellfmt(out, nn, smp, -1, (b[2] & 15) || b[3] ? "0123456789ABCDEF"[b[2] & 15] : 0, b[3]);
}

static void tmod_done(struct trk *t) { trk_release(t->st); }

static const struct trkops modops = { tmod_tick, tmod_go, tmod_name, tmod_rows, tmod_cell, tmod_done };

int mod_trk(struct trk *t, unsigned char *d, unsigned size, char *why)
{
    struct mod *m = trk_alloc(sizeof *m);
    if (!m) { trk_say(why, "no memory"); return -1; }
    if (mod_loadn(m, d, size, why, MOD_MAXCH) < 0) { trk_release(m); return -1; }
    t->fmt = TF_MOD;
    t->st = m;
    t->ops = &modops;
    for (int i = 0; i < 20; i++) t->title[i] = m->title[i];
    const char *k = m->nsmp == 15 ? "Soundtracker (15)" : m->nch == 4 ? "ProTracker" : 0;
    if (k) trk_say(t->kind, k);
    else {
        char *o = t->kind;
        if (m->nch >= 10) *o++ = (char)('0' + m->nch / 10);
        *o++ = (char)('0' + m->nch % 10);
        trk_say(o, "-channel MOD");
    }
    t->nch = m->nch;
    t->songlen = m->songlen;
    t->ninst = m->nsmp;
    t->native = m->nch == 4;
    t->bank = (const signed char *)d + mod_bankoff(m);
    t->banksize = m->banksize;
    for (int c = 0; c < m->nch; c++)
        t->v[c].pan = (c & 3) == 0 || (c & 3) == 3 ? 0 : 255;     /* Paula's sides: left, right, right, left */
    t->rows = 64;
    t->pat = m->order[0];
    mod_out(t);
    return 1;
}
