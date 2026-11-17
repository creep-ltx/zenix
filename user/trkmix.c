/* SPDX-License-Identifier: GPL-2.0-only */
/* The tracker player's mixer (DESIGN D56 section 5, user/trkmix.h). */
#include "trkmix.h"

int mix_init(struct mix *m, unsigned rate, unsigned clock, int nch)
{
    unsigned char *z = (unsigned char *)m;
    for (unsigned i = 0; i < sizeof *m; i++) z[i] = 0;
    m->rate = rate;
    m->clock = clock;
    m->kper = (unsigned)(((unsigned long long)clock << 16) / rate);
    m->nch = nch;
    m->acc = trk_alloc(3 * MIX_CHUNK * sizeof(short));
    m->vt = trk_alloc(65 * 256 * sizeof(short));
    m->clip = trk_alloc(2 * MIX_CLIP);
    if (!m->acc || !m->vt || !m->clip) { mix_free(m); return -1; }
    /* the gain: half the channels on one side at full volume reach full scale, then twice that (a module seldom
     * peaks on every channel at once; past full scale it clips). It is in the table: a channel adds sample x volume x
     * gain / 256, and the sums are the output's bytes, clipped by a table */
    int half = nch / 2 > 1 ? nch / 2 : 1;
    m->amp = 2 * 127 * 256 / (half * 2032);
    if (m->amp < 1) m->amp = 1;
    for (int v = 0; v <= 64; v++)
        for (int b = 0; b < 256; b++)
            m->vt[v][b] = (short)((signed char)b * v * m->amp / 256);
    for (int i = 0; i < 2 * MIX_CLIP; i++) {
        int x = i - MIX_CLIP;
        m->clip[i] = (signed char)(x > 127 ? 127 : x < -128 ? -128 : x);
    }
    return 0;
}

void mix_free(struct mix *m)
{
    trk_release(m->acc);
    trk_release(m->vt);
    trk_release(m->clip);
    m->acc = 0;
    m->vt = 0;
    m->clip = 0;
}

void mix_set(struct mix *m, const struct tvoice *v)
{
    for (int i = 0; i < m->nch; i++, v++) {
        struct mixch *c = &m->c[i];
        if (v->flags & TV_CUT) c->p = 0;
        if (v->flags & TV_TRIG) {
            c->p = v->sp + v->start;
            c->n = v->len;
            c->x = 0;
            if (!c->n) {                        /* nothing before the loop: the loop at once */
                c->p = v->looplen ? v->sp + v->loop : 0;
                c->n = v->looplen;
            }
        }
        if (v->flags & (TV_TRIG | TV_LOOP)) {
            c->lp = v->sp + v->loop;
            c->ln = v->looplen;
        }
        if (v->per > 0) c->step = m->kper / (unsigned)v->per;
        else c->step = (v->hz16 / m->rate << 12) + ((v->hz16 % m->rate) << 12) / m->rate;   /* hz16 x 4096 / rate */
        if (c->step > 64u << 16) c->step = 64u << 16;     /* 64 bytes a frame at most (a megahertz): k x step fits */
        c->vol = v->vol < 0 ? 0 : v->vol > 64 ? 64 : v->vol;
        c->side = v->pan < 64 ? 0 : v->pan > 192 ? 1 : 2;
    }
}

/* cnt frames of one channel's bytes from p at x (16.16, x >> 16 < 32768 + cnt x step), each times its volume, added
 * into acc */
#if defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || defined(__mc68060__) || defined(mc68020)
static void run(short *acc, const signed char *p, unsigned x, unsigned step, int cnt, const short *vt)
{
    /* the integer place in d1, the fraction in the high word of d0: add.l then addx.l steps both. The byte goes into
     * d2's low byte (its upper bits stay 0), the table indexed by it as a word. Four frames a pass, the rest one by one */
    unsigned frac = x << 16, ip = x >> 16, sf = step << 16, si = step >> 16, b = 0, w, n4 = (unsigned)cnt >> 2,
             n1 = (unsigned)cnt & 3;
#define ONE "move.b (%[p],%[ip].l),%[b]\n move.w (%[vt],%[b].l*2),%[w]\n add.w %[w],%[acc]@+\n" \
            " add.l %[sf],%[fr]\n addx.l %[si],%[ip]\n"
    __asm__ volatile(
        "   bra.s   2f\n"
        "1: " ONE
        "2: dbra    %[n1],1b\n"
        "   bra.s   4f\n"
        "3: " ONE ONE ONE ONE
        "4: dbra    %[n4],3b\n"
        : [acc] "+a"(acc), [ip] "+d"(ip), [fr] "+d"(frac), [b] "+d"(b), [n1] "+d"(n1), [n4] "+d"(n4), [w] "=&d"(w)
        : [p] "a"(p), [vt] "a"(vt), [sf] "d"(sf), [si] "d"(si)
        : "cc", "memory");
#undef ONE
}
#else
static void run(short *acc, const signed char *p, unsigned x, unsigned step, int cnt, const short *vt)
{
    for (int i = 0; i < cnt; i++, x += step) acc[i] += vt[(unsigned char)p[x >> 16]];
}
#endif

static void chan(struct mix *m, struct mixch *c, short *acc, int n)
{
    const short *vt = m->vt[c->vol];
    while (n > 0 && c->p) {
        if ((c->x >> 16) >= c->n) {             /* the segment's end: its loop, or silence */
            c->x -= c->n << 16;
            c->p = c->lp;
            c->n = c->ln;
            if (!c->p || c->n < 1) { c->p = 0; break; }
            if ((c->x >> 16) >= c->n) c->x = 0;     /* a step longer than the loop: from its start */
            continue;
        }
        if (!c->step) break;
        unsigned rem = c->n - (c->x >> 16);
        if (rem > 0x7FFF) rem = 0x7FFF;
        unsigned need = ((rem << 16) - (c->x & 0xFFFF) + c->step - 1) / c->step;     /* frames inside rem */
        int cnt = need < (unsigned)n ? (int)need : n;
        if (cnt < 1) cnt = 1;
        run(acc, c->p + (c->x >> 16), c->x & 0xFFFF, c->step, cnt, vt);
        unsigned adv = (c->x & 0xFFFF) + (unsigned)cnt * c->step, k = (c->x >> 16) + (adv >> 16);
        if (k <= c->n) {                        /* still inside: from here on */
            c->p += k;
            c->n -= k;
            c->x = adv & 0xFFFF;
        } else {                                /* past the end: the overshoot kept for the loop */
            c->p += c->n;
            c->x = (k - c->n) << 16 | (adv & 0xFFFF);
            c->n = 0;
        }
        acc += cnt;
        n -= cnt;
    }
}

static void skip(struct mixch *c, int k)
{
    unsigned adv = c->x + (unsigned)k * c->step;      /* x < 2^17 and k x step <= 2^31 */
    if (c->p && (adv >> 16) >= c->n) {                  /* past the segment: into the loop, by a remainder */
        unsigned rest = (adv >> 16) - c->n;
        c->p = c->lp;
        c->n = c->ln;
        if (!c->p || !c->n) { c->p = 0; return; }
        adv = (rest % c->n) << 16 | (adv & 0xFFFF);
    }
    if (!c->p) return;
    c->p += adv >> 16;
    c->n -= adv >> 16;
    c->x = adv & 0xFFFF;
}

void mix_render(struct mix *m, signed char *out, int n)
{
    const signed char *cl = m->clip + MIX_CLIP;
    while (n > 0) {
        int k = n > MIX_CHUNK ? MIX_CHUNK : n, used = 0;
        short *a = m->acc;
        for (int i = 0; i < m->nch; i++)        /* the sums some channel adds into this pass */
            if (m->c[i].p && m->c[i].vol) used |= 1 << m->c[i].side;
        for (int sd = 0; sd < 3; sd++)          /* (k of each side, packed: left, right, middle) */
            if (used & 1 << sd) {
                short *z = a + sd * k;
                for (int i = 0; i < k; i++) z[i] = 0;
            }
        for (int i = 0; i < m->nch; i++) {
            struct mixch *c = &m->c[i];
            if (!c->p) continue;
            if (!c->vol) { skip(c, k); continue; }    /* silent, but its place moves on */
            chan(m, c, a + c->side * k, k);
        }
        const short *l = a, *r = a + k, *mid = a + 2 * k;
        if (used == 4)                          /* the middle only (an XM's usual panning) */
            for (int i = 0; i < k; i++, out += 2) out[0] = out[1] = cl[mid[i]];
        else if (!(used & 4)) {                 /* left and right, no middle (a MOD) */
            if (!(used & 1)) l = mid;           /* (an unused side reads zeros: the middle's, cleared... */
            if (!(used & 2)) r = mid;
            if (used != 3) { short *z = a + 2 * k; for (int i = 0; i < k; i++) z[i] = 0; }  /* ...here) */
            for (int i = 0; i < k; i++, out += 2) { out[0] = cl[l[i]]; out[1] = cl[r[i]]; }
        } else {
            if (!(used & 1)) { short *z = a; for (int i = 0; i < k; i++) z[i] = 0; }
            if (!(used & 2)) { short *z = a + k; for (int i = 0; i < k; i++) z[i] = 0; }
            for (int i = 0; i < k; i++, out += 2) {
                int c = mid[i];
                out[0] = cl[l[i] + c];
                out[1] = cl[r[i] + c];
            }
        }
        n -= k;
    }
}
