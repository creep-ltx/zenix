/* SPDX-License-Identifier: GPL-2.0-only */
/* modplay's replayer (DESIGN D40 section 10), written from the ProTracker module format's documentation and its
 * documented effect behaviour; no player's source was read. The file: a 20-byte title, 31 (or 15, the old format)
 * sample headers of 30 bytes (a 22-byte name, the length in words, the finetune's low nibble as a signed number, the
 * volume 0-64, the loop's start and length in words), the song's length, a byte trackers ignore, 128 order entries,
 * then for 31 samples the 4-byte tag at 1080 ("M.K." and kin); the patterns (64 rows x 4 channels x 4 bytes: the
 * sample's high nibble and the period's 12 bits, the sample's low nibble and the effect, the parameter), then the
 * samples' data. A tick is one call of mod_tick; a row lasts `speed` ticks, a tick E-clock x 2.5 / BPM. */
#include "modrep.h"

/* ProTracker's period table: 16 finetunes (0..7, then -8..-1, the nibble's order) x 36 notes (C-1 .. B-3) */
const short mod_periods[16][36] = {
    {856,808,762,720,678,640,604,570,538,508,480,453,428,404,381,360,339,320,302,285,269,254,240,226,214,202,190,180,170,160,151,143,135,127,120,113},
    {850,802,757,715,674,637,601,567,535,505,477,450,425,401,379,357,337,318,300,284,268,253,239,225,213,201,189,179,169,159,150,142,134,126,119,113},
    {844,796,752,709,670,632,597,563,532,502,474,447,422,398,376,355,335,316,298,282,266,251,237,224,211,199,188,177,167,158,149,141,133,125,118,112},
    {838,791,746,704,665,628,592,559,528,498,470,444,419,395,373,352,332,314,296,280,264,249,235,222,209,198,187,176,166,157,148,140,132,125,118,111},
    {832,785,741,699,660,623,588,555,524,495,467,441,416,392,370,350,330,312,294,278,262,247,233,220,208,196,185,175,165,156,147,139,131,124,117,110},
    {826,779,736,694,655,619,584,551,520,491,463,437,413,390,368,347,328,309,292,276,260,245,232,219,206,195,184,174,164,155,146,138,130,123,116,109},
    {820,774,730,689,651,614,580,547,516,487,460,434,410,387,365,345,325,307,290,274,258,244,230,217,205,193,183,172,163,154,145,137,129,122,115,109},
    {814,768,725,684,646,610,575,543,513,484,457,431,407,384,363,342,323,305,288,272,256,242,228,216,204,192,181,171,161,152,144,136,128,121,114,108},
    {907,856,808,762,720,678,640,604,570,538,508,480,453,428,404,381,360,340,320,302,285,269,254,240,226,214,202,190,180,170,160,151,143,135,127,120},
    {900,850,802,757,715,675,636,601,567,535,505,477,450,425,401,379,357,337,318,300,284,268,253,238,225,212,200,189,179,169,159,150,142,134,126,119},
    {894,844,796,752,709,670,632,597,563,532,502,474,447,422,398,376,355,335,316,298,282,266,251,237,223,211,199,188,177,167,158,149,141,133,125,118},
    {887,838,791,746,704,665,628,592,559,528,498,470,444,419,395,373,352,332,314,296,280,264,249,235,222,209,198,187,176,166,157,148,140,132,125,118},
    {881,832,785,741,699,660,623,588,555,524,494,467,441,416,392,370,350,330,312,294,278,262,247,233,220,208,196,185,175,165,156,147,139,131,123,117},
    {875,826,779,736,694,655,619,584,551,520,491,463,437,413,390,368,347,328,309,292,276,260,245,232,219,206,195,184,174,164,155,146,138,130,123,116},
    {868,820,774,730,689,651,614,580,547,516,487,460,434,410,387,365,345,325,307,290,274,258,244,230,217,205,193,183,172,163,154,145,137,129,122,115},
    {862,814,768,725,684,646,610,575,543,513,484,457,431,407,384,363,342,323,305,288,272,256,242,228,216,203,192,181,171,161,152,144,136,128,121,114},
};

/* the vibrato's and tremolo's sine: the first half of a wave, 0..255 */
static const unsigned char sine[32] = {
    0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24 };

#define PER_MIN 113             /* the portamento's limits (B-3 and C-1) */
#define PER_MAX 856

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }

static void say(char *why, const char *s)
{
    int i = 0;
    for (; s[i] && i < 63; i++) why[i] = s[i];
    why[i] = 0;
}

static int tag4(const unsigned char *p, const char *t)
{
    return p[0] == (unsigned char)t[0] && p[1] == (unsigned char)t[1] && p[2] == (unsigned char)t[2] &&
           p[3] == (unsigned char)t[3];
}

static unsigned bankoff_of(int nsmp, int npat) { return (nsmp == 31 ? 1084u : 600u) + (unsigned)npat * 1024u; }

unsigned mod_bankoff(const struct mod *m) { return bankoff_of(m->nsmp, m->npat); }

int mod_load(struct mod *m, const unsigned char *d, unsigned size, char *why)
{
    unsigned char *z = (unsigned char *)m;
    for (unsigned i = 0; i < sizeof *m; i++) z[i] = 0;
    if (size >= 1084 && (tag4(d + 1080, "M.K.") || tag4(d + 1080, "M!K!") || tag4(d + 1080, "4CHN") ||
                         tag4(d + 1080, "FLT4")))
        m->nsmp = 31;
    else if (size >= 1084 && (tag4(d + 1080, "6CHN") || tag4(d + 1080, "8CHN") || (d[1082] == 'C' && d[1083] == 'H') || tag4(d + 1080, "FLT8") ||
                              tag4(d + 1080, "OCTA") || tag4(d + 1080, "CD81"))) {
        say(why, "not a 4-channel module");
        return -1;
    } else
        m->nsmp = 15;                           /* the old Ultimate Soundtracker format: no tag */
    unsigned head = m->nsmp == 31 ? 1084 : 600, ord = m->nsmp == 31 ? 950 : 470;
    if (size < head) {
        say(why, "too short for a module");
        return -1;
    }
    for (int i = 0; i < 20; i++) m->title[i] = (char)(d[i] >= 32 && d[i] < 127 ? d[i] : d[i] ? '?' : 0);
    m->songlen = d[ord];
    if (m->songlen < 1 || m->songlen > 128) {
        say(why, "bad song length");
        return -1;
    }
    for (int i = 0; i < 128; i++) {
        m->order[i] = d[ord + 2 + i];
        if (m->order[i] >= 128) {
            say(why, "bad order list");
            return -1;
        }
        if (m->order[i] + 1 > m->npat) m->npat = m->order[i] + 1;
    }
    if (bankoff_of(m->nsmp, m->npat) > size) {
        say(why, "the patterns run past the file's end");
        return -1;
    }
    m->pat = d + head;
    unsigned off = 0;
    m->banksize = size - bankoff_of(m->nsmp, m->npat);
    for (int i = 1; i <= m->nsmp; i++) {
        const unsigned char *h = d + 20 + (i - 1) * 30;
        struct modsmp *s = &m->s[i];
        unsigned len = be16(h + 22) * 2u, ls = be16(h + 26) * 2u, ll = be16(h + 28) * 2u;
        s->finetune = h[24] & 15;
        s->vol = h[25] > 64 ? 64 : h[25];
        if (m->nsmp == 15)
            ls /= 2;                            /* the old format's loop start counts bytes, not words */
        s->off = off < m->banksize ? off : 0;   /* a cut file: the sample is what is there, or nothing */
        if (off >= m->banksize) len = 0;
        else if (len > m->banksize - off) len = (m->banksize - off) & ~1u;
        s->len = len;
        if (ll > 2 && ls < len) {
            if (ls + ll > len) ll = len - ls;
            s->loop = off + ls;
            s->looplen = ll & ~1u;
        }
        if (s->looplen < 2) { s->loop = 0; s->looplen = 0; }    /* no loop: the server's silent word */
        off += be16(h + 22) * 2u;
    }
    m->speed = 6;
    m->bpm = 125;
    m->jump = -1;
    return 0;
}

unsigned mod_ticklen(const struct mod *m, unsigned eclock)
{
    return (eclock * 5u) / (2u * (unsigned)m->bpm);
}

void mod_goto(struct mod *m, int pos)
{
    m->pos = pos < 0 ? 0 : pos >= m->songlen ? m->songlen - 1 : pos;
    m->row = 0;
    m->tick = 0;
    m->brk = 0;
    m->jump = -1;
    m->pdelay = m->pdelayn = 0;
    m->ploop = 0;
}

static int note_index(int ft, int period)
{
    const short *t = mod_periods[ft & 15];
    for (int i = 0; i < 36; i++)
        if (t[i] <= period)
            return i;
    return 35;
}

void mod_notename(int period, char out[4])
{
    static const char nn[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
    if (period <= 0) {
        out[0] = out[1] = out[2] = '-';
        out[3] = 0;
        return;
    }
    int i = note_index(0, period);
    if (i > 0 && mod_periods[0][i - 1] - period < period - mod_periods[0][i]) i--;
    out[0] = nn[(i % 12) * 2];
    out[1] = nn[(i % 12) * 2 + 1];
    out[2] = (char)('1' + i / 12);
    out[3] = 0;
}

/* a note's period in the channel's finetune: the pattern holds finetune 0's periods */
static int tune(int period, int ft)
{
    return mod_periods[ft & 15][note_index(0, period)];
}

static void trigger(struct modch *c, int period)
{
    c->period = period;
    c->trig = MOD_TRIG;
    if (!(c->vwave & 4)) c->vpos = 0;
    if (!(c->twave & 4)) c->tpos = 0;
}

static void vslide(struct modch *c, int prm)
{
    if (prm >> 4) {
        c->vol += prm >> 4;
        if (c->vol > 64) c->vol = 64;
    } else {
        c->vol -= prm & 15;
        if (c->vol < 0) c->vol = 0;
    }
}

static void porta(struct modch *c)
{
    if (!c->ptarget)
        return;
    if (c->period < c->ptarget) {
        c->period += c->pspeed;
        if (c->period >= c->ptarget) { c->period = c->ptarget; c->ptarget = 0; }
    } else if (c->period > c->ptarget) {
        c->period -= c->pspeed;
        if (c->period <= c->ptarget) { c->period = c->ptarget; c->ptarget = 0; }
    } else
        c->ptarget = 0;
    c->operiod = c->period;
    if (c->gliss)                               /* E31: the slide in semitones of the channel's finetune */
        c->operiod = mod_periods[c->finetune & 15][note_index(c->finetune, c->period)];
}

static int wave(int kind, int pos, int signpos)
{
    if ((kind & 3) == 0) return sine[pos & 31];
    if ((kind & 3) == 1) { int v = (pos & 31) * 8; return signpos >= 32 ? 255 - v : v; }
    return 255;
}

static void vibrato(struct modch *c)
{
    int d = wave(c->vwave, c->vpos, c->vpos) * c->vdepth >> 7;
    c->operiod = c->vpos < 32 ? c->period + d : c->period - d;
    c->vpos = (c->vpos + c->vspeed) & 63;
}

static void tremolo(struct modch *c)
{
    /* ProTracker's ramp tests the vibrato's position for its direction (documented): kept */
    int d = wave(c->twave, c->tpos, c->vpos) * c->tdepth >> 6;
    int v = c->tpos < 32 ? c->vol + d : c->vol - d;
    c->ovol = v < 0 ? 0 : v > 64 ? 64 : v;
    c->tpos = (c->tpos + c->tspeed) & 63;
}

/* the effects of ticks after the first (and of every tick of a row repeated by EE) */
static void running(struct mod *m, struct modch *c)
{
    int x = c->prm >> 4, y = c->prm & 15;
    switch (c->eff) {
    case 0:
        if (c->prm && m->tick % 3) {
            int ft = c->finetune & 15, i = note_index(ft, c->period) + (m->tick % 3 == 1 ? x : y);
            c->operiod = mod_periods[ft][i > 35 ? 35 : i];      /* past the table: its last note (clamped) */
        }
        break;
    case 1:
        c->period -= c->prm;
        if (c->period < PER_MIN) c->period = PER_MIN;
        c->operiod = c->period;
        break;
    case 2:
        c->period += c->prm;
        if (c->period > PER_MAX) c->period = PER_MAX;
        c->operiod = c->period;
        break;
    case 3: porta(c); break;
    case 4: vibrato(c); break;
    case 5: porta(c); vslide(c, c->prm); c->ovol = c->vol; break;
    case 6: vibrato(c); vslide(c, c->prm); c->ovol = c->vol; break;
    case 7: tremolo(c); break;
    case 0xA: vslide(c, c->prm); c->ovol = c->vol; break;
    case 0xE:
        if (x == 9 && y && m->tick % y == 0)
            c->trig = MOD_TRIG;
        else if (x == 0xC && m->tick == y)
            c->vol = c->ovol = 0;
        else if (x == 0xD && m->tick == y && c->dper) {
            trigger(c, c->dper);
            c->dper = 0;
            c->operiod = c->period;
        }
        break;
    }
}

/* the first tick of a row: its note, sample and the effects of that tick */
static void row_start(struct mod *m, int ch)
{
    struct modch *c = &m->c[ch];
    const unsigned char *b = m->pat + (unsigned)m->order[m->pos] * 1024u + (unsigned)m->row * 16u + (unsigned)ch * 4u;
    int smp = (b[0] & 0xF0) | b[2] >> 4, per = (b[0] & 15) << 8 | b[1];
    c->eff = b[2] & 15;
    c->prm = b[3];
    int x = c->prm >> 4, y = c->prm & 15;
    c->dper = 0;
    if (smp && smp <= m->nsmp) {
        const struct modsmp *s = &m->s[smp];
        c->smp = smp;
        c->vol = s->vol;
        c->finetune = s->finetune;
        c->start = s->off;
        c->len = s->len;
        c->loop = s->loop;
        c->looplen = s->looplen;
        if (!per)
            c->trig = MOD_LOOP;                 /* ProTracker: the next loop is the new sample's */
    }
    if (c->eff == 0xE && x == 5)
        c->finetune = y;
    if (per) {
        int p = tune(per, c->finetune);
        if (c->eff == 3 || c->eff == 5)
            c->ptarget = p == c->period ? 0 : p;
        else if (c->eff == 0xE && x == 0xD && y)
            c->dper = p;
        else
            trigger(c, p);
    }
    c->operiod = c->period;
    switch (c->eff) {
    case 3: if (c->prm) c->pspeed = c->prm; break;
    case 4: if (x) c->vspeed = x; if (y) c->vdepth = y; break;
    case 7: if (x) c->tspeed = x; if (y) c->tdepth = y; break;
    case 9: {
        unsigned o = c->prm ? (unsigned)c->prm << 8 : (unsigned)c->offmem;
        c->offmem = (int)o;
        if (c->trig & MOD_TRIG) {
            if (o < c->len) { c->start += o; c->len -= o; }
            else { c->start = c->loop; c->len = c->looplen; }   /* past the end: the loop (or silence) */
        }
        break;
    }
    case 0xB: m->brk = 1; m->jump = c->prm; break;
    case 0xC: c->vol = c->prm > 64 ? 64 : c->prm; break;
    case 0xD: { int r = x * 10 + y; m->brk = 1; m->brkrow = r > 63 ? 0 : r; break; }
    case 0xE:
        switch (x) {
        case 1: c->period -= y; if (c->period < PER_MIN) c->period = PER_MIN; c->operiod = c->period; break;
        case 2: c->period += y; if (c->period > PER_MAX) c->period = PER_MAX; c->operiod = c->period; break;
        case 3: c->gliss = y; break;
        case 4: c->vwave = y; break;
        case 6:
            if (!y) c->loopr = m->row;
            else if (!c->loopn) { c->loopn = y; m->ploop = 1; m->plrow = c->loopr; }
            else if (--c->loopn) { m->ploop = 1; m->plrow = c->loopr; }
            break;
        case 7: c->twave = y; break;
        case 9: if (y && !per) c->trig = MOD_TRIG; break;
        case 0xA: c->vol += y; if (c->vol > 64) c->vol = 64; break;
        case 0xB: c->vol -= y; if (c->vol < 0) c->vol = 0; break;
        case 0xC: if (!y) c->vol = 0; break;
        case 0xE: if (!m->pdelayn) m->pdelay = y; break;
        }
        break;
    case 0xF:
        if (!c->prm) m->stop = m->ended = 1;
        else if (c->prm < 32) m->speed = c->prm;
        else m->bpm = c->prm;
        break;
    }
    c->ovol = c->vol;
}

static void next_row(struct mod *m)
{
    if (m->pdelay) {                            /* EEx: the row x more times */
        m->pdelayn = m->pdelay;
        m->pdelay = 0;
    }
    if (m->pdelayn) {
        m->pdelayn--;
        m->rep = 1;
        return;
    }
    m->rep = 0;
    if (m->ploop) {                             /* E6x: back to the loop's row, same position */
        m->ploop = 0;
        m->row = m->plrow;
        m->brk = 0;
        m->jump = -1;
        return;
    }
    if (m->brk) {
        int np = m->jump >= 0 ? m->jump : m->pos + 1;
        if (m->jump >= 0 && m->jump <= m->pos)
            m->ended = 1;                       /* a jump back: the song has looped */
        m->pos = np;
        m->row = m->brkrow;
        m->brk = 0;
        m->brkrow = 0;
        m->jump = -1;
    } else if (++m->row >= 64) {
        m->row = 0;
        m->pos++;
    }
    if (m->pos >= m->songlen) {
        m->pos = 0;
        m->ended = 1;
    }
}

void mod_tick(struct mod *m)
{
    for (int ch = 0; ch < 4; ch++) {
        struct modch *c = &m->c[ch];
        c->trig = 0;
        if (m->tick == 0 && !m->rep)
            row_start(m, ch);
        else {
            c->operiod = c->period;
            c->ovol = c->vol;
            running(m, c);
        }
        struct modout *o = &m->out[ch];
        o->flags = c->trig;
        o->period = c->operiod;
        o->vol = c->ovol;
        o->start = c->start;
        o->len = c->len;
        o->loop = c->loop;
        o->looplen = c->looplen;
    }
    if (++m->tick >= m->speed) {
        m->tick = 0;
        next_row(m);
    }
}
