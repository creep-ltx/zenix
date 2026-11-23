/* SPDX-License-Identifier: GPL-2.0-only */
/* sndd, the sound server (DESIGN D40, docs/d40-sound.md): port "audio", a D11 service started by the first call
 * (/etc/svctab). It owns Paula's four channels (the kernel's DRV_PAULA claim) and plays two kinds of stream:
 *   - PCM (SND_OPEN without SNDF_FRAMES): 8-bit signed bytes on a pair of channels (0 left + 1 right, or 3 left + 2
 *     right), double-buffered in chip RAM: Paula plays one half while the other is filled (section 6);
 *   - frames (SNDF_FRAMES, modplay): all four channels, a sample bank in chip RAM, one frame of register values a
 *     tracker tick, applied at its time (section 8).
 * No interrupts: Paula sets a channel's request bit in INTREQ when it has read LC/LEN (HRM, "Joining tones"), enabled
 * or not, and sndd reads INTREQR at times worked out from the E-clock, which is exactly Paula's clock / 5. It sleeps in
 * sys_evwait on its port (a request, or POLLPRI: a waiting client was killed) with a timeout of whole 100 Hz ticks.
 * The registers: without full memory protection the chips' own (user mode reaches them); with it (DESIGN D60 6.2) the
 * kernel writes them for sndd, a batch at a time (DRV_AUDIO), and checks that every channel's DMA reads only sndd's
 * own chip RAM: the chips' page is never sndd's. Every wait on the beam has a limit. */
#include "libz.h"
#include "sndproto.h"
#include "../kernel/netproto.h"

#define CUST(o) (*(volatile unsigned short *)(0xDFF000 + (o)))
#define DMACONR 0x002
#define VPOSR   0x004
#define INTREQR 0x01E
#define DMACON  0x096
#define ADKCON  0x09E
#define INTREQ  0x09C
#define AUDLC(c)  (*(volatile unsigned *)(0xDFF0A0 + 16 * (c)))
#define AUDLEN(c) CUST(0x0A4 + 16 * (c))
#define AUDPER(c) CUST(0x0A6 + 16 * (c))
#define AUDVOL(c) CUST(0x0A8 + 16 * (c))
#define INTF_AUD(c) (0x0080u << (c))
#define DMAF_SET 0x8000u

#define HALF_MS 60              /* a PCM half: 60 ms of sound (section 6) */
#define HALF_MIN 256
#define HALF_MAX 32768          /* a second at Paula's fastest rate */
#define IDLE_HALVES 4           /* halves of silence after which an underrun stream stops its DMA */

struct stream {
    int used, pid, flags, vol, paused;
    int ch[2];                  /* PCM: its pair (left, right); frames: unused */
    unsigned period, rate;
    /* PCM */
    unsigned half;              /* bytes a half, a channel */
    signed char *mem;           /* chip RAM: the rings (one a channel: two halves each) */
    signed char *ring[2];
    int playing, cur;           /* DMA on; the half Paula plays */
    int fillh;                  /* the half being filled (-1: none free) */
    unsigned fill;              /* its bytes so far */
    unsigned tstart, hticks;    /* when the playing half started; a half's length (E-clock ticks) */
    int last;                   /* a drain: the half holding the last bytes (-1: none) */
    int quiet;                  /* halves of silence in a row */
    struct umsg *wmsg;          /* a held SND_WRITE, wdone of its bytes copied */
    unsigned wdone;
    struct umsg *dmsg;          /* a held SND_DRAIN */
    /* frames */
    unsigned char *bank;
    unsigned banklen;
    int qh, qn;                 /* the queue (fq: only one frames stream can exist, it has all four channels) */
    unsigned played, due;
    int timing;                 /* due is valid */
    struct umsg *fmsg;          /* a held SND_FRAMES, fdone of its frames queued */
    unsigned fdone;
};

static struct stream st[SND_MAXSTREAM + 1];     /* 1..SND_MAXSTREAM */
static struct sndframe fq[SND_QFRAMES];
static int chown_[4];                           /* the stream on each channel, 0 free */
static unsigned late_n, empty_n;
static unsigned clock_hz, eclock_hz, ntsc, underruns, frames_applied, waits_hit, irqs[4];
static unsigned short lastper[4], lastvol[4];
static unsigned lastlc[4], lastlen[4];
static signed char *silent;                     /* 4 zero bytes in chip RAM */
static int port, gen;                           /* gen: this sndd's start count, in every stream handle */

static int claim, kpaula;                       /* kpaula: Paula through the kernel (DRV_AUDIO, full protection) */
static struct udrvaudio q;                      /* the batch being made (kpaula) */
static unsigned refused;

static unsigned now(void) { return sys_hrtime(); }

/* ---- Paula's registers: written at once, or batched for the kernel (kpaula) ---- */

/* the batch to the kernel, then (wait: INTREQ's bits) its bounded wait -> the audio bits of DMACONR and INTREQR and
 * VPOSR as the kernel read them (DRV_AUDIO's answer); 0 when refused (said once: a bug here, never the client's) */
static unsigned kflush(unsigned wait, unsigned limit)
{
    q.wait = wait;
    q.limit = limit;
    int r = sys_drv(DRV_AUDIO, (unsigned)claim, (unsigned)&q);
    q.n = 0;
    if (r < 0) {
        if (!refused++) dprintf(1, "sndd: the kernel refused a batch of Paula's writes (%d)\n", r);
        return 0;
    }
    return (unsigned)r;
}
static void flush(void) { if (kpaula && q.n) kflush(0, 0); }
static void put(unsigned reg, unsigned v)
{
    if (!kpaula) { CUST(reg) = (unsigned short)v; return; }
    if (q.n >= DRV_AUDIO_MAX) kflush(0, 0);
    q.w[q.n].reg = (unsigned short)reg;
    q.w[q.n++].val = (unsigned short)v;
}
static unsigned get(unsigned reg)               /* INTREQR, DMACONR or VPOSR */
{
    if (!kpaula) return CUST(reg);
    unsigned r = kflush(0, 0);
    return reg == VPOSR ? r >> 16 : reg == DMACONR ? r & 15 : r & 0x0780;
}

/* E-clock ticks from now to t (negative: past) */
static int until(unsigned t) { return (int)(t - now()); }

static void say(const char *s) { dprintf(1, "sndd: %s\n", s); }

/* ---- Paula ---- */

static void set_lc(int c, const void *p, unsigned bytes)
{
    if (!kpaula) {
        AUDLC(c) = (unsigned)p;
        AUDLEN(c) = (unsigned short)(bytes / 2);
    } else {                    /* the pointer and its length in one batch: the kernel checks the pair as it leaves it */
        if (q.n + 3 > DRV_AUDIO_MAX) kflush(0, 0);
        put(0x0A0 + 16 * c, (unsigned)p >> 16);
        put(0x0A2 + 16 * c, (unsigned)p & 0xFFFF);
        put(0x0A4 + 16 * c, bytes / 2);
    }
    lastlc[c] = (unsigned)p;
    lastlen[c] = bytes;
}
static void set_per(int c, unsigned p) { put(0x0A6 + 16 * c, p); lastper[c] = (unsigned short)p; }
static void set_vol(int c, unsigned v) { put(0x0A8 + 16 * c, v); lastvol[c] = (unsigned short)v; }

/* the request bit of channel c was set since it was last cleared: cleared now -> 1 */
static int took(int c)
{
    if (kpaula) {               /* one call: the kernel looks and clears the bit when it is set */
        if (!(kflush(INTF_AUD(c), 0) & INTF_AUD(c)))
            return 0;
    } else {
        if (!(CUST(INTREQR) & INTF_AUD(c)))
            return 0;
        CUST(INTREQ) = (unsigned short)INTF_AUD(c);
    }
    irqs[c]++;
    return 1;
}

/* the longest period on the channels of mask, Paula's last (what it still counts down) or a new one */
static unsigned maxper(unsigned mask, unsigned p)
{
    for (int c = 0; c < 4; c++) if ((mask & (1u << c)) && lastper[c] > p) p = lastper[c];
    return p;
}

/* a channel whose DMA was switched off stops only after two sample periods (HRM, "Stopping the audio DMA"): wait that
 * long for period per, on the E-clock (a period is per colour clocks, an E-clock tick five) */
static void off_wait(unsigned per)
{
    unsigned t0 = now(), n = 2 * per / 5 + 2;
    while (now() - t0 < n)
        ;                       /* each look is a system call reading the CIA: a wait on hardware, bounded by n */
}

/* wait until every request bit of mask is set (Paula read LC/LEN within a period of per), then clear them */
static void wait_bits(unsigned mask, unsigned per)
{
    unsigned want = mask << 7, t0 = now(), lim = 2 * per / 5 + eclock_hz / 1000u;  /* a period and a millisecond */
    if (kpaula) {               /* the batch so far, then the kernel's wait (the bits cleared after it) */
        if ((kflush(want, lim) & want) != want && !waits_hit++)
            dprintf(1, "sndd: Paula did not take the places of channels %x in time\n", mask);
    } else {
        while ((CUST(INTREQR) & want) != want)
            if (now() - t0 > lim) { /* said once (CLAUDE.md: a wait on hardware says what it waited for; audit A6-6) */
                if (!waits_hit++) dprintf(1, "sndd: Paula did not take the places of channels %x in time\n", mask);
                break;
            }
        CUST(INTREQ) = (unsigned short)want;
    }
    for (int c = 0; c < 4; c++) if (mask & (1u << c)) irqs[c]++;
}

static void chan_off(unsigned mask)
{
    for (int c = 0; c < 4; c++) if (mask & (1u << c)) set_vol(c, 0);
    put(DMACON, mask);
    put(INTREQ, mask << 7);
    flush();
}

/* ---- streams ---- */

static void reply(struct umsg **mp, int r)
{
    if (*mp) { sys_reply(*mp, r); *mp = 0; }
}

static unsigned mask_of(struct stream *s)
{
    unsigned m = 0;
    for (int c = 0; c < 4; c++) if (chown_[c] == s - st) m |= 1u << c;
    return m;
}

static void close_stream(struct stream *s, int why)
{
    unsigned m = mask_of(s);
    chan_off(m);
    off_wait(maxper(m, 0));     /* the channels have let go before their memory goes */
    for (int c = 0; c < 4; c++) if (m & (1u << c)) chown_[c] = 0;
    reply(&s->wmsg, why);
    reply(&s->dmsg, why);
    reply(&s->fmsg, why);
    if (s->mem) sys_memfree(s->mem);
    if (s->bank) sys_memfree(s->bank);
    memset(s, 0, sizeof *s);
}

static int peer_ok(struct umsg *m, int op, unsigned addr, unsigned len)
{
    struct upeermem pm = { addr, len };
    return len == 0 || sys_peer(m, op, &pm) == 0;
}

static unsigned period_of(unsigned rate)
{
    unsigned p = rate ? (clock_hz + rate / 2) / rate : 0;
    return p < SND_MINPER ? SND_MINPER : p > 65535 ? 65535 : p;
}

static int do_open(struct umsg *m)
{
    unsigned rate = m->a[0], fl = m->a[1], vol = m->a[2], ms = m->a[3] ? m->a[3] : HALF_MS;
    if ((fl & ~3u) || vol > 64 || ms < 20 || ms > 1000 || (!(fl & SNDF_FRAMES) && (rate < 1000 || rate > 100000)))
        return -EINVAL;
    int id = 0;
    for (int i = 1; i <= SND_MAXSTREAM && !id; i++) if (!st[i].used) id = i;
    if (!id)
        return -EBUSY;
    struct stream *s = &st[id];
    if (fl & SNDF_FRAMES) {
        if (chown_[0] || chown_[1] || chown_[2] || chown_[3])
            return -EBUSY;
    } else {
        int a = !chown_[0] && !chown_[1], b = !chown_[3] && !chown_[2];
        if (!a && !b)
            return -EBUSY;
        s->ch[0] = a ? 0 : 3;
        s->ch[1] = a ? 1 : 2;
        s->period = period_of(rate);
        s->rate = clock_hz / s->period;
        unsigned h = (s->rate * ms / 1000 + 1) & ~1u;
        s->half = h < HALF_MIN ? HALF_MIN : h > HALF_MAX ? HALF_MAX : h;
        int n = fl & SNDF_STEREO ? 2 : 1;
        s->mem = sys_memalloc_chip(2 * s->half * (unsigned)n);
        if (!s->mem)
            return -ENOMEM;
        memset(s->mem, 0, 2 * s->half * (unsigned)n);
        s->ring[0] = s->mem;
        s->ring[1] = n == 2 ? s->mem + 2 * s->half : s->mem;
        s->hticks = s->half * s->period / 5u;
        s->fillh = 0;
        s->last = -1;
    }
    s->used = 1;
    s->pid = m->from;
    s->flags = (int)fl;
    s->vol = (int)vol;
    if (fl & SNDF_FRAMES)
        for (int c = 0; c < 4; c++) chown_[c] = id;
    else
        chown_[s->ch[0]] = chown_[s->ch[1]] = id;
    return gen << 8 | id;
}

static struct stream *mine(struct umsg *m, int *err)
{
    unsigned id = m->a[0] & 0xFF;
    if (m->a[0] >> 8 != (unsigned)gen) { *err = -ECONNRESET; return 0; }   /* a stream of an earlier sndd */
    if (id < 1 || id > SND_MAXSTREAM || !st[id].used) { *err = -EINVAL; return 0; }
    if (st[id].pid != m->from && m->uid != 0) { *err = -EPERM; return 0; }
    return &st[id];
}

/* PCM (section 6). The halves: Paula plays `cur`; the other is either queued behind it (fillh -1) or being filled
 * (fillh, fill bytes so far). Not playing: half 0 is filled first, and the stream starts when it is sealed. A drain
 * marks the half with the last bytes (`last`); when the half after it starts, the sound has ended. */
static void pcm_stop(struct stream *s)
{
    chan_off(mask_of(s));
    s->playing = 0;
    s->fillh = 0;
    s->fill = 0;
    s->last = -1;
    s->quiet = 0;
}

static void pcm_start(struct stream *s)
{
    unsigned m = mask_of(s);
    chan_off(m);
    off_wait(maxper(m, s->period));
    for (int k = 0; k < 2; k++) {
        int c = s->ch[k];
        set_per(c, s->period);
        set_vol(c, (unsigned)s->vol);
        set_lc(c, s->ring[k], s->half);
    }
    put(INTREQ, m << 7);
    put(DMACON, DMAF_SET | (kpaula ? 0 : 0x0200) | m);     /* the master bit (set already: the kernel's) with the pair's */
    wait_bits(m, s->period);                                     /* Paula has read half 0's LC/LEN */
    for (int k = 0; k < 2; k++)
        set_lc(s->ch[k], s->ring[k] + s->half, s->half);         /* half 1 behind it */
    s->playing = 1;
    s->cur = 0;
    s->tstart = now();
    s->fillh = 1;
    s->fill = 0;
}

static int nch(const struct stream *s) { return s->flags & SNDF_STEREO ? 2 : 1; }

/* the half being filled is done: the rest silent; not playing, half 0 starts the stream */
static void pcm_seal(struct stream *s)
{
    for (int k = 0; k < nch(s); k++)
        memset(s->ring[k] + s->fillh * (int)s->half + s->fill, 0, s->half - s->fill);
    int h = s->fillh;
    s->fillh = -1;
    s->fill = 0;
    if (!s->playing && h == 0) {
        pcm_start(s);
        if (s->last == 0) {     /* all there is was in half 0: half 1 silent */
            pcm_seal(s);
        }
    }
}

/* copy what the held write still has into the half being filled */
static void pcm_take(struct stream *s)
{
    struct umsg *m = s->wmsg;
    while (m && s->fillh >= 0 && s->last < 0) {
        int two = s->flags & SNDF_STEREO;
        const signed char *src = (const signed char *)m->a[1] + s->wdone;
        unsigned left = m->a[2] - s->wdone;
        unsigned n = two ? left / 2 : left, room = s->half - s->fill;
        if (n > room) n = room;
        signed char *d0 = s->ring[0] + s->fillh * (int)s->half + s->fill;
        if (!two)
            memcpy(d0, src, n);
        else {
            signed char *d1 = s->ring[1] + s->fillh * (int)s->half + s->fill;
            for (unsigned i = 0; i < n; i++) { d0[i] = src[2 * i]; d1[i] = src[2 * i + 1]; }
        }
        s->fill += n;
        s->wdone += two ? 2 * n : n;
        if (n) s->quiet = 0;
        if (m->a[2] - s->wdone < (two ? 2u : 1u)) {             /* all taken (an odd last byte of a stereo pair dropped) */
            reply(&s->wmsg, (int)m->a[2]);
            m = 0;
        }
        if (s->fill == s->half)
            pcm_seal(s);
        else
            break;
    }
}

/* a drain with nothing more to come: which half holds the last bytes */
static void pcm_drain(struct stream *s)
{
    if (!s->dmsg || s->wmsg || s->last >= 0)
        return;
    if (!s->playing) {
        if (!s->fill) { reply(&s->dmsg, 0); return; }
        s->last = 0;
        pcm_seal(s);
        return;
    }
    if (s->fillh >= 0) {
        s->last = s->fill ? s->fillh : s->cur;
        pcm_seal(s);
    } else
        s->last = s->cur ^ 1;
}

/* PCM: what happened since the last look -> the E-clock time to look again (0: nothing to wait for) */
static unsigned pcm_service(struct stream *s)
{
    pcm_take(s);
    pcm_drain(s);
    if (!s->playing)
        return 0;
    if (took(s->ch[0])) {       /* the queued half started: the one it follows is free */
        unsigned exp = s->tstart + s->hticks;
        int late = (int)(now() - exp);
        s->tstart = late >= 0 && late < (int)s->hticks ? exp : now();
        int old = s->cur;
        if (s->fillh >= 0) {    /* it started before it was sealed: it plays what it had */
            underruns++;
            late_n++;
            pcm_seal(s);
        }
        s->cur ^= 1;
        if (s->last == old) {   /* the last bytes have played */
            pcm_stop(s);
            reply(&s->dmsg, 0);
            return 0;
        }
        for (int k = 0; k < 2; k++)
            set_lc(s->ch[k], s->ring[k] + old * (int)s->half, s->half);
        s->fillh = old;
        s->fill = 0;
        if (s->last >= 0)
            pcm_seal(s);        /* the end is playing: the half after it silent */
        else {
            pcm_take(s);
            pcm_drain(s);
        }
    }
    if (s->fillh >= 0 && until(s->tstart + s->hticks) < (int)(eclock_hz / 66)) {     /* 15 ms left: seal it */
        if (!s->fill && !s->wmsg)
            s->quiet++;         /* nothing at all: silence (four in a row and the stream rests) */
        if (!s->dmsg && (s->fill || s->wmsg || s->quiet == 1)) {
            underruns++;        /* the client's bytes came too late: a gap */
            empty_n++;
        }
        pcm_seal(s);
    }
    if (s->quiet >= IDLE_HALVES && !s->wmsg && !s->dmsg) {
        pcm_stop(s);            /* nothing comes: Paula stops until something does */
        return 0;
    }
    if (s->fillh >= 0)
        return s->tstart + s->hticks - eclock_hz / 50;          /* 20 ms before the half's end: the seal at 15 */
    return s->tstart + s->hticks + eclock_hz / 400;            /* just after the queued half starts */
}

/* ---- frames ---- */

/* a place in the bank: SND_SILENT, or even, inside it, a length 2..131,070 (AUDxLEN counts words) */
static int place_ok(const struct stream *s, unsigned off, unsigned len)
{
    if (off == SND_SILENT)
        return 1;
    return !((off | len) & 1) && len >= 2 && len <= 131070 && off <= s->banklen && len <= s->banklen - off;
}

static int frame_ok(const struct stream *s, const struct sndframe *f)
{
    for (int c = 0; c < 4; c++) {
        const struct sndch *x = &f->ch[c];
        if (x->flags & ~31u) return 0;
        if ((x->flags & SNDC_PER) && (x->period < SND_FMINPER || x->period > SND_FMAXPER)) return 0;
        if ((x->flags & SNDC_TRIG) && !place_ok(s, x->start, x->len)) return 0;
        if ((x->flags & (SNDC_TRIG | SNDC_LOOP)) && !place_ok(s, x->loop, x->looplen)) return 0;
    }
    return 1;
}

static const void *place(struct stream *s, unsigned off, unsigned *len)
{
    if (off == SND_SILENT || *len < 2) { *len = 2; return silent; }
    return s->bank + off;
}

static void apply(struct stream *s, const struct sndframe *f)
{
    unsigned trig = 0, stop = 0;
    for (int c = 0; c < 4; c++) {
        if (f->ch[c].flags & SNDC_TRIG) trig |= 1u << c;
        if (f->ch[c].flags & SNDC_STOP) stop |= 1u << c;
    }
    unsigned old = maxper(trig, 0);             /* the periods Paula still counts down on the channels to start */
    if (trig | stop)
        put(DMACON, trig | stop);
    for (int c = 0; c < 4; c++) {
        const struct sndch *x = &f->ch[c];
        if (x->flags & SNDC_PER) set_per(c, x->period);
        if (x->flags & SNDC_VOL) set_vol(c, (x->volume > 64 ? 64u : x->volume) * (unsigned)s->vol / 64u);
        if (trig & (1u << c)) {
            unsigned len = x->len;
            const void *p = place(s, x->start, &len);
            set_lc(c, p, len);
        }
    }
    if (trig) {
        flush();
        off_wait(old);                          /* two of the old periods: the channels have let go */
        put(INTREQ, trig << 7);
        put(DMACON, DMAF_SET | (kpaula ? 0 : 0x0200) | trig);
        wait_bits(trig, maxper(trig, 0));       /* Paula has read the start's LC/LEN */
    }
    for (int c = 0; c < 4; c++) {
        const struct sndch *x = &f->ch[c];
        if (x->flags & (SNDC_TRIG | SNDC_LOOP)) {
            unsigned len = x->looplen;
            const void *p = place(s, x->loop, &len);
            set_lc(c, p, len);
        }
    }
    flush();
    frames_applied++;
}

/* copy what the held SND_FRAMES still has into the queue */
static void frames_take(struct stream *s)
{
    struct umsg *m = s->fmsg;
    if (!m)
        return;
    const struct sndframe *f = (const struct sndframe *)m->a[1];
    while (s->fdone < m->a[2] && s->qn < SND_QFRAMES) {
        if (!frame_ok(s, &f[s->fdone])) { reply(&s->fmsg, -EINVAL); return; }
        memcpy(&fq[(s->qh + s->qn) % SND_QFRAMES], &f[s->fdone], sizeof *f);
        s->qn++;
        s->fdone++;
    }
    if (s->fdone >= m->a[2])
        reply(&s->fmsg, (int)s->played);
}

static unsigned frames_service(struct stream *s)
{
    frames_take(s);
    if (s->paused)
        return 0;
    int halftick = (int)(eclock_hz / 200);
    while (s->qn) {
        if (!s->timing) { s->due = now(); s->timing = 1; }
        if (until(s->due) > halftick)
            break;
        struct sndframe *f = &fq[s->qh];
        apply(s, f);
        s->due += f->ticks ? f->ticks : 1;
        if (until(s->due) < -(int)(eclock_hz / 10)) s->due = now();     /* far behind (sndd was held): no rush */
        s->qh = (s->qh + 1) % SND_QFRAMES;
        s->qn--;
        s->played++;
        frames_take(s);
    }
    if (!s->qn) {
        if (s->timing && until(s->due) <= halftick) {
            if (s->dmsg) { reply(&s->dmsg, 0); s->timing = 0; return 0; }
            underruns++;
            s->timing = 0;                  /* starved: the next frame starts the clock again */
        }
        return s->timing ? s->due : 0;
    }
    return s->due;
}

/* ---- requests ---- */

static void info(struct sndinfo *x, unsigned h)
{
    memset(x, 0, sizeof *x);
    x->size = SNDINFO_FIRST;
    x->clock = clock_hz;
    x->eclock = eclock_hz;
    x->flags = ntsc ? SNDI_NTSC : 0;
    unsigned id = h & 0xFF;
    if (h >> 8 == (unsigned)gen && id >= 1 && id <= SND_MAXSTREAM && st[id].used) {
        x->period = st[id].period;
        x->rate = st[id].rate;
    }
    x->underruns = underruns;
    for (int c = 0; c < 4; c++) if (!chown_[c]) x->freech |= 1u << c;
}

static void request(struct umsg *m)
{
    int e = 0;
    struct stream *s;
    switch (m->type) {
    case SND_OPEN:
        sys_reply(m, do_open(m));
        return;
    case SND_INFO: {
        if (m->a[1] < SNDINFO_FIRST) { sys_reply(m, -EINVAL); return; }
        if (!peer_ok(m, PEER_WR, m->a[0], SNDINFO_FIRST)) { sys_reply(m, -EFAULT); return; }
        struct sndinfo x;
        info(&x, m->a[2]);
        memcpy((void *)m->a[0], &x, SNDINFO_FIRST);
        sys_reply(m, SNDINFO_FIRST);
        return;
    }
    case SND_STATE: {
        unsigned n = m->a[1] > SNDSTATE_SIZE ? SNDSTATE_SIZE : m->a[1];
        if (!peer_ok(m, PEER_WR, m->a[0], n)) { sys_reply(m, -EFAULT); return; }
        struct sndstate x;
        info(&x.info, 0);
        x.dmacon = get(DMACONR);    /* (through the kernel: its audio bits) */
        x.streams = 0;
        for (int i = 1; i <= SND_MAXSTREAM; i++) x.streams += st[i].used ? 1 : 0;
        x.frames = frames_applied;
        x.waits = waits_hit;
        x.late = late_n;
        x.empty = empty_n;
        for (int c = 0; c < 4; c++) {
            x.ch[c].per = lastper[c]; x.ch[c].vol = lastvol[c]; x.ch[c].lc = lastlc[c]; x.ch[c].len = lastlen[c];
            x.ch[c].irqs = irqs[c]; x.ch[c].owner = (unsigned)chown_[c];
        }
        memcpy((void *)m->a[0], &x, n);
        sys_reply(m, (int)n);
        return;
    }
    }
    if (!(s = mine(m, &e))) { sys_reply(m, e); return; }
    switch (m->type) {
    case SND_WRITE:
        if (s->flags & SNDF_FRAMES) { sys_reply(m, -EINVAL); return; }
        if (s->wmsg || s->dmsg) { sys_reply(m, -EBUSY); return; }
        if (!peer_ok(m, PEER_RD, m->a[1], m->a[2])) { sys_reply(m, -EFAULT); return; }
        if (!m->a[2]) { sys_reply(m, 0); return; }
        s->wmsg = m;
        s->wdone = 0;
        return;
    case SND_DRAIN:
        if (s->wmsg || s->dmsg) { sys_reply(m, -EBUSY); return; }
        s->dmsg = m;
        return;
    case SND_CLOSE:
        close_stream(s, -EINTR);
        sys_reply(m, 0);
        return;
    case SND_VOLUME: {
        if (m->a[1] > 64) { sys_reply(m, -EINVAL); return; }
        int old = s->vol;
        s->vol = (int)m->a[1];
        if (!(s->flags & SNDF_FRAMES))
            for (int k = 0; k < 2; k++) set_vol(s->ch[k], (unsigned)s->vol);
        sys_reply(m, old);
        return;
    }
    case SND_BANK:
        if (!(s->flags & SNDF_FRAMES) || m->a[2] > SND_BANKMAX) { sys_reply(m, -EINVAL); return; }
        if (!peer_ok(m, PEER_RD, m->a[1], m->a[2])) { sys_reply(m, -EFAULT); return; }
        chan_off(0xF);
        off_wait(maxper(0xF, 0));
        if (s->bank) { sys_memfree(s->bank); s->bank = 0; s->banklen = 0; }
        if (m->a[2]) {
            s->bank = sys_memalloc_chip((m->a[2] + 1) & ~1u);
            if (!s->bank) { sys_reply(m, -ENOMEM); return; }
            memcpy(s->bank, (const void *)m->a[1], m->a[2]);
            s->banklen = m->a[2] & ~1u;
        }
        s->qn = 0;
        s->timing = 0;
        sys_reply(m, 0);
        return;
    case SND_FRAMES:
        if (!(s->flags & SNDF_FRAMES) || s->fmsg || s->dmsg || !m->a[2] || m->a[2] > 4096 || m->a[3] != SNDFRAME_SIZE) {
            sys_reply(m, -EINVAL);
            return;
        }
        if (!peer_ok(m, PEER_RD, m->a[1], m->a[2] * sizeof(struct sndframe))) { sys_reply(m, -EFAULT); return; }
        s->fmsg = m;
        s->fdone = 0;
        frames_take(s);
        return;
    case SND_PAUSE:
        if (!(s->flags & SNDF_FRAMES)) { sys_reply(m, -EINVAL); return; }
        s->paused = m->a[1] != 0;
        if (s->paused) { for (int c = 0; c < 4; c++) set_vol(c, 0); }
        else s->timing = 0;
        sys_reply(m, 0);
        return;
    }
    sys_reply(m, -EINVAL);
}

/* a waiting client was killed (D23's UMSG_CANCEL): its stream ends at once */
static void cancels(void)
{
    for (int i = 1; i <= SND_MAXSTREAM; i++) {
        struct stream *s = &st[i];
        if (!s->used) continue;
        if ((s->wmsg && (s->wmsg->kflags & UMSG_CANCEL)) || (s->dmsg && (s->dmsg->kflags & UMSG_CANCEL)) ||
            (s->fmsg && (s->fmsg->kflags & UMSG_CANCEL)))
            close_stream(s, -EINTR);
    }
}

/* streams whose process has ended (-ESRCH) are closed */
static void reap(void)
{
    for (int i = 1; i <= SND_MAXSTREAM; i++)
        if (st[i].used && !st[i].wmsg && !st[i].dmsg && !st[i].fmsg && sys_getpriority(PRIO_PROCESS, st[i].pid) == -ESRCH)
            close_stream(&st[i], -EINTR);
}

/* sndd was killed, or its port is gone: Paula silent, every kept request answered, the end */
static void quit(void)
{
    chan_off(0xF);
    for (int i = 1; i <= SND_MAXSTREAM; i++) {
        struct stream *s = &st[i];
        int c = (s->wmsg && (s->wmsg->kflags & UMSG_CANCEL)) || (s->dmsg && (s->dmsg->kflags & UMSG_CANCEL)) ||
                (s->fmsg && (s->fmsg->kflags & UMSG_CANCEL));
        reply(&s->wmsg, c ? -EINTR : -ECONNRESET);
        reply(&s->dmsg, c ? -EINTR : -ECONNRESET);
        reply(&s->fmsg, c ? -EINTR : -ECONNRESET);
    }
    sys_exit(1);
}

#define IDLE_EXIT 30            /* seconds with no stream: sndd ends (D11 starts it again), the bell comes back */

int main(void)
{
    gen = sys_svc_self() & 0x7FFF;
    port = sys_port_create(SND_PORT);           /* first: the kernel knows the service's process by its port */
    if (port < 0) { say("port audio busy"); return 1; }
    claim = sys_drv(DRV_CLAIM, DRV_PAULA, 0);
    if (claim < 0) {
        dprintf(1, "sndd: Paula could not be claimed (%d): not started by the service manager?\n", claim);
        return 1;
    }
    /* after the claim (D60 stage 3: with full memory protection the chips' page is ours only from then on) */
    kpaula = prot_full();                       /* D60 6.2: the chips' page is not ours; the kernel writes Paula */
    ntsc = (get(VPOSR) & 0x1000) != 0;          /* the Agnus/Alice NTSC bit, as the kernel reads it */
    clock_hz = ntsc ? 3579545u : 3546895u;
    eclock_hz = ntsc ? 715909u : 709379u;
    silent = sys_memalloc_chip(4);
    if (!silent) { say("no chip RAM"); return 1; }
    memset(silent, 0, 4);
    put(ADKCON, 0x00FF);                        /* no channel modulates another (a program before may have left it) */
    flush();
    unsigned lastreap = now(), idle = now();
    for (;;) {
        unsigned want = 0;
        int any = 0, open = 0;
        for (int i = 1; i <= SND_MAXSTREAM; i++) {
            struct stream *s = &st[i];
            if (!s->used) continue;
            unsigned t = s->flags & SNDF_FRAMES ? frames_service(s) : pcm_service(s);
            if (t && (!any || (int)(t - want) < 0)) { want = t; any = 1; }
            open |= s->used;
        }
        int ticks = -1;
        if (any) {
            int d = until(want);
            ticks = d <= 0 ? 1 : (int)((unsigned)d / (eclock_hz / 100u));
            if (ticks < 1) ticks = 1;
        }
        if (open && (ticks < 0 || ticks > 50)) ticks = 50;     /* half a second: the owners are looked at */
        if (open) idle = now();
        else if (now() - idle > IDLE_EXIT * eclock_hz) return 0;
        else if (ticks < 0) ticks = IDLE_EXIT * 100 + 10;
        flush();                /* (kpaula: what the turn wrote, before sndd sleeps) */
        struct pollfd pf = { port, POLLPORT | POLLIN | POLLPRI, 0 };
        if (sys_evwait(&pf, 1, ticks) < 0)
            quit();             /* -EINTR: killed (sndd catches no signal) */
        if (pf.revents & POLLPRI)
            cancels();
        for (;;) {
            struct umsg *m = sys_recv(port, 0);
            if (!m) break;
            if ((int)m < 0) quit();
            request(m);
        }
        if (open && now() - lastreap > eclock_hz / 2) { reap(); lastreap = now(); }
    }
}
