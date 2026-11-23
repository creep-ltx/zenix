/* SPDX-License-Identifier: GPL-2.0-only */
/* A download through the plipbox, on the model's clock (DESIGN D39 section 7): where a TCP transfer's time goes.
 *
 * The driver's line protocol and its turn's waits are the real code (net/drv/plipbox-core.c); the box is the model
 * of its firmware (tools/plipbox-model.h) with the ENC28J60's receive ring at its real size (plipbox 0.6's
 * enc28j60.c: 0x0000-0x19FF, 6,656 bytes: four full frames); beyond it the 10 Mbit half-duplex wire (the ENC28J60
 * has no faster mode) fed by a switch that queues, and a TCP sender as Linux's without SACK (lwIP sends none): Reno
 * with NewReno's partial ACKs, 10 segments to start, a 200 ms minimum RTO. On the Amiga netd is lwIP's receive rules
 * (an ACK for every second segment, at once for one out of order, the delayed one at the 250 ms timer; the reader
 * keeps up, so the window is always whole) with a CPU time per frame; netd hands its ACKs back in the same DRV_XFER,
 * as do_xfer does.
 *
 * Not modelled: Linux's CUBIC and RACK (Reno's halving is close enough for a 4-frame queue), the half-duplex wire's
 * collisions between the box's ACKs and the switch's frames (an ACK is 64 bytes), other processes than netd.
 *
 * tools/plipbox-flow [KB] [window] [enc_bytes] [netd_us] [ack_us]: one run with those numbers; no arguments: the
 * test (the owner's 880 KB download, checked; the ring's overflows; a window of four segments; the driver's own
 * figures as it logs them at its end). Run by tools/plipbox-test.sh. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
#include "../kernel/netproto.h"
#include "../net/drv/plipbox.h"
#include "plipbox-model.h"

static unsigned char pb_rd(u32 a) { return m_rd(a); }
static void pb_wr(u32 a, unsigned char v) { m_wr(a, v); }
static void pb_bset(u32 a, int bit) { m_wr(a, (unsigned char)(m_rd(a) | 1u << bit)); }
static void pb_bclr(u32 a, int bit) { m_wr(a, (unsigned char)(m_rd(a) & ~(1u << bit))); }
static void pb_clrbits(u32 a, unsigned m) { m_wr(a, (unsigned char)(m_rd(a) & ~m)); }
static u32 pb_ticks(void) { return (u32)(M.t / 10000000ull); }
static long pb_acks(void) { return (long)(M.acks & 0x7FFFFFFFu); }
static void pb_sleep(u32 t) { m_idle(t * 10000000ull); }
struct pb;
static void pb_log(struct pb *b, const char *fmt, ...);
static int pb_yield(void) { return 0; }
#define PB_FN
#include "../net/drv/plipbox-line.c"
static int pb_line(struct pb *b, struct udrvparx *x, unsigned char *buf) { (void)b; return pb_xfer(x, buf); }
#include "../net/drv/plipbox-core.c"
static void pb_log(struct pb *b, const char *fmt, ...)
{
    if (!m_trace) return;
    va_list ap;
    va_start(ap, fmt);
    printf("    plipbox: %s: ", b->ifn);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); bad++; } else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

#define MSS     1460u
#define HDRS    54u                     /* Ethernet, IP and TCP headers */
#define NSEG    4096u
#define MS      1000000ull
#define WIRE_NS(len) ((uint64_t)((len) + 4 + 8 + 12) * 800)     /* 10 Mbit: CRC, preamble, gap */

static struct pb b;
static unsigned nnetd_us = 200;         /* netd's CPU per data frame (lwIP, the copy to the reader) */
static uint64_t ack_delay_ns = 300000;     /* the box's ACK to the sender and its answer: the switch, the NAS */

/* ---- the network: the switch's queue onto the 10 Mbit wire, the ACKs back ---- */
struct wf { uint64_t at; u32 seq, len; };
static struct wf wire[8192]; static unsigned whead, wn;
static uint64_t wire_free;
struct wa { uint64_t at; u32 ack, wnd; };
static struct wa acks[8192]; static unsigned ahead, an;
static unsigned long outseen;

/* ---- the sender: Reno with NewReno's partial ACKs, no SACK ---- */
static struct {
    u32 total, una, nxt, high, rwnd, recover;
    double cwnd, ssthresh;
    int dupacks, inrec;
    uint64_t rto, rto_at, sent_at[NSEG];
    unsigned char retx[NSEG];
    double srtt, rttvar;
    int have_rtt;
    unsigned long sent, resent, fast, timeouts;
} S;

/* ---- netd: lwIP's receive rules ---- */
static struct {
    u32 nxt, wnd;
    unsigned char have[NSEG];
    int delayed, ring;
    uint64_t next_tmr;
    unsigned long acks_sent, frames;
} R;
static u32 pend[256]; static unsigned npend;     /* ACKs netd handed to the driver, not sent yet */

static void seg_out(uint64_t now, u32 seq)
{
    u32 len = S.total - seq < MSS ? S.total - seq : MSS;
    uint64_t dep = now > wire_free ? now : wire_free;
    wire_free = dep + WIRE_NS(len + HDRS);
    struct wf *w = &wire[(whead + wn++) % 8192];
    w->at = wire_free; w->seq = seq; w->len = len;
    unsigned k = seq / MSS;
    if (S.sent_at[k]) { S.retx[k] = 1; S.resent++; }
    S.sent_at[k] = now;
    S.sent++;
    if (!S.rto_at) S.rto_at = now + S.rto;
}
static void s_send(uint64_t now)
{
    for (;;) {
        u32 flight = S.nxt - S.una, lim = (u32)S.cwnd < S.rwnd ? (u32)S.cwnd : S.rwnd;
        if (S.nxt >= S.total || flight + MSS > lim + (flight ? 0 : MSS)) return;
        seg_out(now, S.nxt);
        S.nxt += S.total - S.nxt < MSS ? S.total - S.nxt : MSS;
        if (S.nxt > S.high) S.high = S.nxt;
    }
}
static void s_ack(uint64_t now, u32 ack, u32 wnd)
{
    S.rwnd = wnd;
    if (ack > S.una) {
        u32 acked = ack - S.una;
        unsigned k = (ack - 1) / MSS;
        if (!S.retx[k] && S.sent_at[k]) {               /* Karn: a segment sent once */
            double r = (double)(now - S.sent_at[k]);
            if (!S.have_rtt) { S.srtt = r; S.rttvar = r / 2; S.have_rtt = 1; }
            else { S.rttvar = 0.75 * S.rttvar + 0.25 * (r > S.srtt ? r - S.srtt : S.srtt - r); S.srtt = 0.875 * S.srtt + 0.125 * r; }
            double rto = S.srtt + 4 * S.rttvar;
            S.rto = rto < 200 * MS ? 200 * MS : (uint64_t)rto;
        }
        S.una = ack;
        if (S.nxt < S.una) S.nxt = S.una;
        S.dupacks = 0;
        if (S.inrec) {
            if (ack >= S.recover) { S.inrec = 0; S.cwnd = S.ssthresh; }
            else {                                      /* a partial ACK: the next hole at once */
                seg_out(now, S.una);
                S.cwnd = S.cwnd - acked + MSS;
                if (S.cwnd < MSS) S.cwnd = MSS;
            }
        } else if (S.cwnd < S.ssthresh) S.cwnd += acked < 2 * MSS ? acked : 2 * MSS;
        else S.cwnd += (double)MSS * MSS / S.cwnd;
        S.rto_at = S.una < S.nxt ? now + S.rto : 0;
    } else if (ack == S.una && S.una < S.nxt) {
        if (++S.dupacks == 3 && !S.inrec) {
            u32 flight = S.nxt - S.una;
            S.ssthresh = flight / 2 > 2 * MSS ? flight / 2 : 2 * MSS;
            S.cwnd = S.ssthresh + 3 * MSS;
            S.inrec = 1;
            S.recover = S.nxt;
            S.fast++;
            seg_out(now, S.una);
        } else if (S.inrec) S.cwnd += MSS;
    }
    s_send(now);
}

static void net(uint64_t now)
{
    while (an && acks[ahead].at <= now) {
        struct wa *a = &acks[ahead];
        ahead = (ahead + 1) % 8192; an--;
        s_ack(a->at, a->ack, a->wnd);
    }
    if (S.rto_at && now >= S.rto_at && S.una < S.total) {       /* the retransmission timer: go back */
        u32 flight = S.nxt - S.una;
        S.ssthresh = flight / 2 > 2 * MSS ? flight / 2 : 2 * MSS;
        S.cwnd = MSS;
        S.inrec = 0;
        S.dupacks = 0;
        S.nxt = S.una;
        S.timeouts++;
        S.rto = S.rto * 2 > 60000 * MS ? 60000 * MS : S.rto * 2;
        S.rto_at = 0;
        s_send(now);
    }
    while (wn && wire[whead].at <= now) {               /* into the ENC28J60's ring (or dropped: full) */
        struct wf *w = &wire[whead];
        whead = (whead + 1) % 8192; wn--;
        unsigned char f[1514];
        unsigned len = w->len + HDRS;
        memset(f, 0x5A, len);
        memcpy(f, b.mac, 6);
        f[12] = 0x08; f[13] = 0x00;
        f[14] = (unsigned char)(w->seq >> 24); f[15] = (unsigned char)(w->seq >> 16);
        f[16] = (unsigned char)(w->seq >> 8); f[17] = (unsigned char)w->seq;
        f[18] = (unsigned char)(w->len >> 8); f[19] = (unsigned char)w->len;
        m_net_in(f, len);
    }
    while (outseen < M.out_total) {                     /* the box sent an ACK on: at the sender 0.3 ms later */
        const struct mframe *o = &M.out[outseen++ % 2048];
        struct wa *a = &acks[(ahead + an++) % 8192];
        a->at = now + WIRE_NS(64) + ack_delay_ns;
        a->ack = (u32)o->d[14] << 24 | (u32)o->d[15] << 16 | (u32)o->d[16] << 8 | o->d[17];
        a->wnd = (u32)o->d[18] << 8 | o->d[19];
    }
    if (now >= R.next_tmr) {                            /* lwIP's fast timer: the delayed ACK */
        R.next_tmr += 250 * MS;
        if (R.delayed) { R.delayed = 0; pend[npend++] = R.nxt; R.ring = 1; }
    }
}

/* netd's buffers (always free: the reader keeps up), the frames of a turn */
static unsigned char rbuf[DRV_BATCH][PB_BUF];
static unsigned rlen[DRV_BATCH], nrx;
static int f_take(void *ctx, unsigned char **p) { (void)ctx; if (nrx >= DRV_BATCH) return -1; *p = rbuf[nrx]; return (int)nrx; }
static void f_give(void *ctx, int num, unsigned len) { (void)ctx; rlen[num] = len; nrx++; }
static const struct pb_sink fsink = { f_take, f_give, 0 };

/* DRV_XFER: netd takes the turn's frames through lwIP's receive rules; its ACKs come back in the same call */
static void xfer(void)
{
    for (unsigned i = 0; i < nrx; i++) {
        const unsigned char *f = rbuf[i];
        u32 seq = (u32)f[14] << 24 | (u32)f[15] << 16 | (u32)f[16] << 8 | f[17], len = (u32)f[18] << 8 | f[19];
        m_idle(nnetd_us * 1000ull);
        R.frames++;
        if (seq == R.nxt) {
            R.have[seq / MSS] = 1;
            int ooseq = 0;
            while (R.nxt < S.total && R.have[R.nxt / MSS]) {
                R.nxt += S.total - R.nxt < MSS ? S.total - R.nxt : MSS;
                ooseq++;
            }
            (void)len;
            if (ooseq > 1 || R.delayed) { R.delayed = 0; pend[npend++] = R.nxt; }  /* tcp_ack: every second, or the queue */
            else R.delayed = 1;
        } else {
            if (seq > R.nxt) R.have[seq / MSS] = 1;
            pend[npend++] = R.nxt;                      /* out of order, or old: an ACK at once (a duplicate) */
        }
    }
    nrx = 0;
    m_idle(50000);                                      /* the call itself */
}
static int send_acks(void)
{
    unsigned k = 0;
    for (; k < npend; k++) {
        unsigned char *f = b.tx + 2;
        memset(f, 0, 60);
        memcpy(f, "\x00\x11\x32\x00\x00\x01", 6);
        memcpy(f + 6, b.mac, 6);
        f[12] = 0x08; f[13] = 0x00;
        f[14] = (unsigned char)(pend[k] >> 24); f[15] = (unsigned char)(pend[k] >> 16);
        f[16] = (unsigned char)(pend[k] >> 8); f[17] = (unsigned char)pend[k];
        f[18] = (unsigned char)(R.wnd >> 8); f[19] = (unsigned char)R.wnd;
        int r = pb_send(&b, 60);
        pb_result(&b, r, "sending");
        R.acks_sent++;
    }
    npend = 0;
    return k != 0;
}
/* drv_wait(ticks): the doorbell (netd's timer had an ACK) or the kernel's tick */
static void dwait(int ticks)
{
    if (!ticks) return;
    uint64_t until = (M.t / (10 * MS) + (uint64_t)ticks) * 10 * MS;
    while (M.t < until && !R.ring) m_idle(20000);
    R.ring = 0;
}

struct run { double secs, kbs, rxms; unsigned long drops, resent, fast, timeouts, sleeps, late; double asleep; };

/* one download of kb KB through a window of wnd bytes; the driver's turn as net/drv/plipbox.c's main loop */
static struct run download(unsigned kb, u32 wnd, unsigned enc)
{
    struct run o;
    memset(&o, 0, sizeof o);
    m_reset(1, 7);
    memset(&b, 0, sizeof b);
    b.ifn = "eth0";
    b.burst = 1;
    m_net = 0;
    if (pb_start(&b, 1)) { printf("FAIL: the start\n"); bad++; return o; }
    m_idle(1000000);
    memset(&S, 0, sizeof S); memset(&R, 0, sizeof R);
    whead = wn = ahead = an = 0; npend = nrx = 0;
    outseen = M.out_total;
    M.enc_cap = enc;
    S.total = kb * 1024u;
    S.cwnd = 10 * MSS; S.ssthresh = 1e9; S.rwnd = wnd; S.rto = 1000 * MS;
    R.wnd = wnd;
    uint64_t t0 = M.t;
    wire_free = t0;
    R.next_tmr = t0 + 250 * MS;
    m_net = net;
    s_send(t0);
    for (int idle = 0; R.nxt < S.total && M.t - t0 < 600000 * MS;) {
        int w = pb_idle_ticks(&b, idle);
        uint64_t s0 = M.t;
        dwait(w);
        pb_woke(&b, w);
        if (w) { o.sleeps++; o.asleep += (double)(M.t - s0); }
        int moved = pb_poll(&b, &fsink, DRV_BATCH) > 0;
        moved |= send_acks();
        pb_keepalive(&b);
        unsigned got = nrx;
        xfer();
        moved |= send_acks() | (got != 0);
        int asks = (u32)pb_acks() != b.acks;
        idle = moved || asks ? 0 : idle + 1;
    }
    m_net = 0;
    o.rxms = b.rx_n ? b.rx_ticks * 10.0 / b.rx_n : 0;
    o.late = b.late;
    o.secs = (double)(M.t - t0) / 1e9;
    o.kbs = kb / o.secs;
    o.drops = M.enc_full;
    o.resent = S.resent; o.fast = S.fast; o.timeouts = S.timeouts;
    o.asleep /= 1e9;
    return o;
}

static void show(const char *what, const struct run *r)
{
    printf("      %s: %.2f s, %.0f KB/s; the ring full %lu times, %lu sent again (%lu fast, %lu timeouts); "
           "%lu sleeps (%lu with a frame waiting), %.2f s asleep; the driver's figure: %.1f ms a frame in\n", what, r->secs,
           r->kbs, r->drops, r->resent, r->fast, r->timeouts, r->sleeps, r->late, r->asleep, r->rxms);
}

int main(int argc, char **argv)
{
    m_trace = getenv("TRACE") != 0;
    if (argc > 1) {
        unsigned kb = (unsigned)atoi(argv[1]);
        u32 wnd = argc > 2 ? (u32)atoi(argv[2]) : 16384;
        unsigned enc = argc > 3 ? (unsigned)atoi(argv[3]) : 6656;
        if (argc > 4) nnetd_us = (unsigned)atoi(argv[4]);
        if (argc > 5) ack_delay_ns = (uint64_t)atoi(argv[5]) * 1000;
        struct run r = download(kb, wnd, enc);
        show("download", &r);
        return 0;
    }
    /* the owner's measurement (2026-11-04): wget of 880 KB from the NAS, the large profile's 16 KB window */
    struct run big = download(880, 16384, 6656);
    show("880 KB, window 16 KB (netd's large profile), the ENC28J60's 6,656 bytes", &big);
    struct run fit = download(880, 4 * MSS, 6656);
    show("880 KB, a window of four segments (what the box's ring holds)", &fit);
    struct run ideal = download(880, 16384, 0);
    show("880 KB, window 16 KB, a ring without limit (the line alone)", &ideal);
    nnetd_us = 2000;
    struct run slow = download(880, 16384, 6656);
    show("880 KB, window 16 KB, netd 2 ms a frame", &slow);
    nnetd_us = 200;
    CHECK(big.kbs >= 135, "a download on the model: %.0f KB/s (the line's handshaked receive, every byte answered)", big.kbs);
    CHECK(big.secs < 1.12 * ideal.secs, "the ring's overflows (%lu, the sender's %lu sent again) cost %.0f%% on the model",
          big.drops, big.resent, (big.secs / ideal.secs - 1) * 100);
    CHECK(fit.drops == 0 && fit.kbs >= 0.97 * ideal.kbs, "a window of four segments: no overflow, the line kept busy (%.0f KB/s)", fit.kbs);
    CHECK(big.sleeps < 60 && big.late < 10, "the driver seldom sleeps while frames come (%lu sleeps, %lu with a frame waiting)",
          big.sleeps, big.late);
    CHECK(big.rxms > 8 && big.rxms < 10.5, "the driver's own figure for the line: %.1f ms a frame in (the model's 9.0)", big.rxms);
    printf("%s\n", bad ? "plipbox-flow: FAILED" : "plipbox-flow: ok");
    return bad != 0;
}
