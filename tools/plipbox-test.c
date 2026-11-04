/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the plipbox driver's line protocol (net/drv/plipbox-core.c, DESIGN D39) against the model of the box in
 * tools/plipbox-model.h: the start (the box's own MAC, a MAC given, a box held in a burst, a box whose last request was
 * never taken), frames both ways checked byte by byte (bursts and handshaked sends), with interrupts and other
 * processes cutting in, the box missing, deaf, going away in a frame and coming back restarted, a size too big, a
 * frame too short, a burst that lost an edge, no free buffer, the loopback test frame, the quiesce program; the
 * frame times on the model's clock; and why the box's own burst receive is not used (cut by a gap, it reads wrong
 * bytes that nothing can see). Build and run: tools/plipbox-test.sh   (TRACE=1 shows the driver's log) */
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
static void pb_bset(u32 a, int bit) { m_wr(a, (unsigned char)(m_rd(a) | 1u << bit)); }     /* two accesses, as bset */
static void pb_bclr(u32 a, int bit) { m_wr(a, (unsigned char)(m_rd(a) & ~(1u << bit))); }
static u32 pb_now(void) { return (u32)(M.t * 709379ull / 1000000000ull); }
static long pb_acks(void) { return (long)(M.acks & 0x7FFFFFFFu); }
static void pb_sleep(u32 t) { m_idle(t * 10000000ull); }
struct pb;
static char lastlog[64][200];
static int nlog;
static void pb_log(struct pb *b, const char *fmt, ...);
#include "../net/drv/plipbox-core.c"
static void pb_log(struct pb *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lastlog[nlog % 64], sizeof lastlog[0], fmt, ap);
    va_end(ap);
    if (m_trace) printf("    plipbox: %s: %s\n", b->ifn, lastlog[nlog % 64]);
    nlog++;
}
static int logged(const char *s)
{
    for (int i = nlog > 64 ? nlog - 64 : 0; i < nlog; i++) if (strstr(lastlog[i % 64], s)) return 1;
    return 0;
}

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); bad++; } else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- netd's side: 16 buffers, the frames received kept for checking ---- */
#define NB 16
static unsigned char nbuf[NB][1536];
static int nown[NB];
static int nfree_limit = NB;            /* buffers netd has free (a slow netd: fewer) */
static struct mframe got[4096];
static unsigned ngot;
static int s_take(void *ctx, unsigned char **p)
{
    (void)ctx;
    int nfree = 0;
    for (int i = 0; i < NB; i++) if (!nown[i]) nfree++;
    if (nfree <= NB - nfree_limit) return -1;
    for (int i = 0; i < NB; i++) if (!nown[i]) { *p = nbuf[i]; return i; }
    return -1;
}
static void s_give(void *ctx, int num, unsigned len)
{
    (void)ctx;
    if (ngot < 4096) { got[ngot].len = len; memcpy(got[ngot].d, nbuf[num], len); ngot++; }
    nown[num] = 0;                      /* netd copies it and gives it back at once */
}
static const struct pb_sink sink = { s_take, s_give, 0 };

static struct pb b;

static void mkframe(struct mframe *f, unsigned len)
{
    f->len = len;
    for (unsigned i = 0; i < len; i++) f->d[i] = (unsigned char)m_rand();
    memcpy(f->d, b.mac, 6);
    f->d[12] = 0x08; f->d[13] = 0x00;   /* IPv4: never a magic type */
}
static unsigned rlen(void)
{
    switch (m_rand() % 4) {
    case 0: return 14 + (unsigned)m_rnd(50);
    case 1: return 1514 - (unsigned)m_rnd(10);
    default: return 14 + (unsigned)m_rnd(1501);
    }
}
static int same(const struct mframe *a, const struct mframe *c) { return a->len == c->len && !memcmp(a->d, c->d, a->len); }
/* the box sends on whole words (pb_proto.c: the size it hands on is the bytes it took): an odd frame gets the
 * driver's zero byte at its end */
static int same_out(const struct mframe *o, const struct mframe *f)
{
    return o->len == f->len + (f->len & 1) && !memcmp(o->d, f->d, f->len) && (!(f->len & 1) || !o->d[f->len]);
}

static void machine(int present, uint64_t seed, unsigned cpu)
{
    m_reset(present, seed);
    M.cpu = cpu;
    memset(&b, 0, sizeof b);
    b.ifn = "eth0";
    b.burst = 1;
    memset(nown, 0, sizeof nown);
    nfree_limit = NB;
    ngot = 0;
    nlog = 0;
}
static int lines_clean(void)            /* nothing of the port touched but ours; nothing driven twice */
{
    return !M.contention && !M.halfdrive && !M.busy_driven && !M.serial_touched;
}
static int lines_off(void)              /* the port as the machine starts it */
{
    return M.ddrb == 0 && (M.ddra & 7) == 0 && (M.ddra & 0xF8) == M.ser_ddra && (M.pra & 0xF8) == M.ser_pra;
}

/* the driver's turn: the box's requests, then up to n frames to send */
static int turn(struct mframe *tx, unsigned n)
{
    int r = pb_poll(&b, &sink, NB);
    for (unsigned i = 0; i < n; i++) {
        memcpy(b.tx + 2, tx[i].d, tx[i].len);
        int e = pb_send(&b, tx[i].len);
        pb_result(&b, e, "sending");
        if (e) b.tx_err++; else b.tx_ok++;
    }
    return r;
}

static struct mframe txf[2000], rxf[2000];

/* n frames each way, (both: at once); -> frames that arrived wrong (not lost: wrong) */
static unsigned traffic(unsigned ntx, unsigned nrx, unsigned *lost_tx, unsigned *lost_rx)
{
    unsigned sent = 0, queued = 0, out0 = (unsigned)M.out_total, wrong = 0;
    unsigned got0 = ngot;
    for (unsigned i = 0; i < ntx; i++) mkframe(&txf[i], rlen());
    for (unsigned i = 0; i < nrx; i++) mkframe(&rxf[i], rlen());
    for (int rounds = 0; rounds < 200000 && (sent < ntx || ngot - got0 + M.in_dropped < nrx + 0 || M.nin); rounds++) {
        while (queued < nrx && M.nin < 8) m_net_in(rxf[queued].d, rxf[queued].len), queued++;
        unsigned k = sent < ntx ? 1 : 0;
        turn(&txf[sent], k);
        sent += k;
        m_idle(20000);
        if (queued == nrx && !M.nin && sent == ntx && !M.req_pending && M.acks == (b.acks | 0)) break;
    }
    m_idle(5000000);                    /* the last frame through the box */
    turn(0, 0);
    /* to the network: each frame the box sent on, in order, matched against what was sent (a lost one skipped) */
    unsigned nout = (unsigned)M.out_total - out0, j = 0;
    for (unsigned i = 0; i < nout; i++) {
        const struct mframe *o = &M.out[(out0 + i) % 2048];
        while (j < ntx && !same_out(o, &txf[j])) j++;
        if (j == ntx) {
            wrong++;
            if (m_trace) printf("      wrong out frame %u: len %u (first sent len %u)\n", i, o->len, txf[0].len);
        } else j++;
    }
    *lost_tx = ntx - (nout < ntx ? nout : ntx);
    j = 0;
    for (unsigned i = got0; i < ngot; i++) {
        while (j < nrx && !same(&got[i], &rxf[j])) j++;
        if (j == nrx) wrong++; else j++;
    }
    *lost_rx = nrx - (ngot - got0);
    return wrong;
}

/* the box's own burst receive (0x44) as plipbox.device does it (hwpar.asm's hwburstrecv, its interrupts off there):
 * toggle POUT, read the byte, no answer waited for. Here with the model's gaps, as a Zenix process would have them */
static int burst_recv_as_amigaos(unsigned char *buf, unsigned *len)
{
    pb_begin(&b, PB_WAIT);
    if (pb_wait(&b, 0, "b")) return -1;
    pb_wr(PB_DDATA, 0xFF); pb_wr(PB_DATA, PB_CMD_RECVB); pb_bset(PB_CTRL, PB_SEL);
    if (pb_wait(&b, 1, "b")) goto out;
    pb_wr(PB_DDATA, 0); pb_bset(PB_CTRL, PB_POUT);
    if (pb_wait(&b, 0, "b")) goto out;
    unsigned n = (unsigned)pb_rd(PB_DATA) << 8; pb_bclr(PB_CTRL, PB_POUT);
    if (pb_wait(&b, 1, "b")) goto out;
    n |= pb_rd(PB_DATA); pb_bset(PB_CTRL, PB_POUT);
    if (pb_wait(&b, 0, "b")) goto out;
    for (unsigned i = 0; i < (n + 1) / 2; i++) {
        pb_bclr(PB_CTRL, PB_POUT); buf[2 * i] = pb_rd(PB_DATA);
        pb_bset(PB_CTRL, PB_POUT); buf[2 * i + 1] = pb_rd(PB_DATA);
    }
    pb_bclr(PB_CTRL, PB_POUT);
    if (pb_wait(&b, 1, "b")) goto out;
    pb_bset(PB_CTRL, PB_POUT);
    (void)pb_wait(&b, 0, "b");
    *len = n;
    pb_end();
    return 0;
out:
    pb_end();
    return -1;
}

/* the driver's start, and the box's time to take the online frame in */
static int start(int adopt)
{
    int r = pb_start(&b, adopt);
    m_idle(1000000);
    return r;
}

int main(void)
{
    m_trace = getenv("TRACE") != 0;
    unsigned lt, lr, w;

    /* ---- 1. the start: the box's own MAC adopted, nothing written into its EEPROM ---- */
    machine(1, 1, 0);
    static const unsigned char other[6] = { 0x02, 0x00, 0x5a, 0x10, 0x20, 0x30 };
    memcpy(M.mac, other, 6);            /* a MAC someone set in the box (AmigaOS's Roadshow, its console) */
    int r = start(1);
    CHECK(r == 0 && !memcmp(b.mac, other, 6), "start: the box's own MAC adopted (%02x:..:%02x)", b.mac[0], b.mac[5]);
    CHECK(M.online && M.magic_online == 1 && M.magic_loop == 1 && !M.eeprom_writes,
          "start: loopback, the box's answer, online; its EEPROM not written (%d %lu %lu %u)", M.online, M.magic_online, M.magic_loop, M.eeprom_writes);
    CHECK(!M.garbage_reads && lines_clean() && b.acks == M.acks, "start: no request taken that the box did not make; the port clean");

    /* ---- 2. a MAC given (mac=): the box takes it (one EEPROM write) ---- */
    machine(1, 2, 0);
    static const unsigned char given[6] = { 0x1a, 0x11, 0xa1, 0xa0, 0x00, 0x01 };
    memcpy(b.mac, given, 6);
    r = start(0);
    CHECK(r == 0 && !memcmp(M.mac, given, 6) && M.eeprom_writes == 1 && M.online, "start with mac=: the box keeps the MAC given");

    /* ---- 3. frames both ways, a 68060 (no CPU time) and a 14 MHz 68020 (0.4 us an access) ---- */
    for (int cpu = 0; cpu < 2; cpu++) {
        for (int burst = 1; burst >= 0; burst--) {
            machine(1, 30 + (uint64_t)cpu * 2 + (uint64_t)burst, cpu ? 400 : 0);
            b.burst = burst;
            start(1);
            w = traffic(1500, 0, &lt, &lr);
            CHECK(!w && !lt && b.tx_ok == 1500 + 0 && !b.tx_err, "%s, %s sends: 1,500 frames to the box, byte for byte (%u wrong, %u lost)",
                  cpu ? "68020" : "68060", burst ? "burst" : "handshaked", w, lt);
            w = traffic(0, 1500, &lt, &lr);
            CHECK(!w && !lr && !M.garbage_reads, "%s: 1,500 frames from the box, byte for byte (%u wrong, %u lost), no request made up",
                  cpu ? "68020" : "68060", w, lr);
            w = traffic(800, 800, &lt, &lr);
            CHECK(!w && !lt && !lr && !M.garbage_reads && lines_clean(), "%s, %s: 800 each way at once (%u wrong, %u+%u lost); the port clean",
                  cpu ? "68020" : "68060", burst ? "burst" : "handshaked", w, lt, lr);
        }
    }

    /* ---- 4. interrupts and other processes cutting in (gaps of 5 us to 30 ms before 1 access in 200) ---- */
    for (int burst = 1; burst >= 0; burst--) {
        machine(1, 40 + (uint64_t)burst, 0);
        b.burst = burst;
        start(1);
        M.gap_p = 0.005; M.gap_min = 5000; M.gap_max = 30000000;
        w = traffic(600, 600, &lt, &lr);
        CHECK(!w && !lt && !lr && !M.garbage_reads, "gaps (%lu of up to 30 ms), %s sends: 600 each way, none wrong, none lost (%u, %u+%u)",
              M.gaps, burst ? "burst" : "handshaked", w, lt, lr);
        /* gaps beyond the box's 500 ms: transfers fail, frames are lost, none arrives wrong, the line recovers */
        M.gap_p = 0.0005; M.gap_min = 400000000; M.gap_max = 700000000;
        w = traffic(100, 100, &lt, &lr);
        M.gap_p = 0;
        unsigned lt2, lr2, w2 = traffic(100, 100, &lt2, &lr2);
        CHECK(!w && !w2 && !lt2 && !lr2 && !M.garbage_reads, "gaps of 0.4-0.7 s (%s): %u+%u lost, none wrong; afterwards 100 each way again, none lost",
              burst ? "burst" : "handshaked", lt, lr);
    }

    /* ---- 5. the frame times on the model's clock (E-clock bound; the box's ENC28J60 time included) ---- */
    {
        machine(1, 50, 0);
        start(1);
        struct mframe f;
        mkframe(&f, 1514);
        memcpy(b.tx + 2, f.d, 1514);
        uint64_t t0 = M.t; pb_send(&b, 1514); uint64_t tb = M.t - t0;
        b.burst = 0;
        memcpy(b.tx + 2, f.d, 1514);
        t0 = M.t; pb_send(&b, 1514); uint64_t th = M.t - t0;
        m_idle(10000000);
        m_net_in(f.d, 1514);
        m_idle(100000);
        unsigned char bb[PB_BUF]; unsigned n;
        t0 = M.t; r = pb_recv(&b, bb, sizeof bb, &n); uint64_t tr = M.t - t0;
        printf("      model: a 1,514-byte frame: burst send %.2f ms (%.0f KB/s), handshaked send %.2f ms (%.0f KB/s), "
               "receive %.2f ms (%.0f KB/s, the box's 2.3 ms from its chip included)\n",
               tb / 1e6, 1514 / (tb / 1e9) / 1024, th / 1e6, 1514 / (th / 1e9) / 1024, tr / 1e6, 1514 / (tr / 1e9) / 1024);
        CHECK(r == 0 && n == 1514 && tb < th && tb < 9000000 && th < 15000000 && tr < 15000000,
              "frame times: burst %.1f ms < handshaked %.1f ms; receive %.1f ms", tb / 1e6, th / 1e6, tr / 1e6);
    }

    /* ---- 6. no box: BUSY pulled high ---- */
    machine(0, 60, 0);
    uint64_t t0 = M.t;
    r = start(1);
    CHECK(r == -ETIMEDOUT && logged("nothing answers on the parallel port: BUSY stays high"),
          "no box: \"nothing answers ... BUSY stays high\" after %.0f ms", (M.t - t0) / 1e6);
    CHECK((M.t - t0) < 700000000ull && lines_off() && lines_clean(), "no box: given up within 0.7 s, the port left as found, the serial port's bits untouched");

    /* ---- 7. a box that never answers (powered, BUSY low) ---- */
    machine(1, 70, 0);
    M.deaf = 1;
    r = start(1);
    CHECK(r == -ETIMEDOUT && logged("does not answer (the command's answer)") && lines_off(), "a deaf box: \"does not answer (the command's answer)\", the port left");

    /* ---- 8. a box whose last request was never taken (the Amiga restarted with the box online): no pulse comes ---- */
    machine(1, 80, 0);
    M.online = 1;
    M.req_pending = 1;                  /* its pulse went before Zenix was there */
    struct mframe f8;
    mkframe(&f8, 200);
    m_net_in(f8.d, f8.len);
    r = start(1);
    pb_poll(&b, &sink, NB);
    int f8in = ngot == 1 && same(&got[0], &f8);
    w = traffic(0, 50, &lt, &lr);
    CHECK(r == 0 && f8in && !M.garbage_reads && !w && !lr, "a request never taken: the start takes it (the loopback's answer); the frame waiting and 50 more come in (%u lost)", lr);

    /* ---- 9. a box held in a burst's end (a driver killed in a burst: its quiesce dropped SEL) ---- */
    machine(1, 90, 0);
    start(1);
    struct udrvquiesce qz;
    pb_quiesce(&qz);
    mkframe(&f8, 1514);
    memcpy(b.tx + 2, f8.d, 1514);
    M.die_after = 0;
    /* the send cut short half way: the model's box in its burst, SEL dropped by the quiesce program */
    {
        unsigned char *p = b.tx; p[0] = 1514 >> 8; p[1] = 1514 & 255;
        pb_begin(&b, PB_WAIT);
        pb_wait(&b, 0, "x");
        pb_wr(PB_DDATA, 0xFF); pb_wr(PB_DATA, PB_CMD_SENDB); pb_bset(PB_CTRL, PB_SEL);
        pb_wait(&b, 1, "x"); pb_wr(PB_DATA, p[0]); pb_bset(PB_CTRL, PB_POUT);
        pb_wait(&b, 0, "x"); pb_wr(PB_DATA, p[1]); pb_bclr(PB_CTRL, PB_POUT);
        pb_wait(&b, 1, "x");
        for (int i = 0; i < 100; i++) { pb_wr(PB_DATA, p[2 + i]); if (i & 1) pb_bclr(PB_CTRL, PB_POUT); else pb_bset(PB_CTRL, PB_POUT); }
        m_quiesce(&qz);
    }
    m_idle(2000000000ull);
    if (m_trace) printf("      after: pc %d i %u held %d busy %d\n", M.pc, M.i, M.held, m_busy());
    CHECK(lines_off() && lines_clean(), "killed in a burst: the quiesce program leaves the port as found (the box still in its burst)");
    memset(&b, 0, sizeof b); b.ifn = "eth0"; b.burst = 1;
    r = start(1);
    w = traffic(50, 50, &lt, &lr);
    CHECK(r == 0 && !M.held && !w && !lt && !lr && !M.garbage_reads, "the next start frees the box, then 50 frames each way (%u wrong)", w);

    /* ---- 10. a burst in which the box missed an edge: the end is not answered; the box freed, the next frames go ---- */
    machine(1, 100, 0);
    start(1);
    M.miss_edge = 1;
    mkframe(&f8, 600);
    memcpy(b.tx + 2, f8.d, 600);
    r = pb_send(&b, 600);
    int held10 = M.held;
    w = traffic(50, 50, &lt, &lr);
    CHECK(r == -ETIMEDOUT && !held10 && !w && !lt && !lr, "a burst short of an edge: no answer at its end (%d), the box freed at once, 50 each way after", r);

    /* ---- 11. damaged frames from the box: a size too big, a frame too short ---- */
    machine(1, 110, 0);
    start(1);
    M.bad_size = 3000;
    m_net_in(f8.d, 600);
    m_idle(1000000);
    pb_poll(&b, &sink, NB);
    CHECK(b.rx_bad == 1 && ngot == 0 && logged("more than 1514 bytes") && !b.down, "a size of 3,000: dropped before its bytes, counted, the line fine");
    M.bad_size = 9;
    m_net_in(f8.d, 600);
    m_idle(1000000);
    pb_poll(&b, &sink, NB);
    CHECK(b.rx_bad == 2 && ngot == 0 && logged("a frame of 9 bytes"), "a frame of 9 bytes: dropped, counted");
    w = traffic(50, 50, &lt, &lr);
    CHECK(!w && !lt && !lr && !M.garbage_reads, "then 50 each way again");

    /* ---- 12. the box goes away in the middle of a frame, then comes back restarted (offline) ---- */
    for (int dir = 0; dir < 2; dir++) {
        machine(1, 120 + (uint64_t)dir, 0);
        b.burst = dir;
        start(1);
        M.die_after = 300;
        mkframe(&f8, 1000);
        if (dir) { memcpy(b.tx + 2, f8.d, 1000); r = pb_send(&b, 1000); pb_result(&b, r, "sending"); }
        else { m_net_in(f8.d, 1000); m_idle(1000000); pb_poll(&b, &sink, NB); r = b.fails ? -ETIMEDOUT : 0; }
        for (int i = 0; i < 4; i++) { memcpy(b.tx + 2, f8.d, 100); pb_result(&b, pb_send(&b, 100), "sending"); }
        CHECK(r == -ETIMEDOUT && b.down && logged("link down") && lines_clean(), "the box gone in a frame (%s): timed out, link down after %d failures",
              dir ? "a burst send" : "a receive", PB_DOWNAFTER);
        m_box_restart();
        if (pb_rd(PB_CTRL) & 1) pb_unstick(&b);
        r = pb_hello(&b, 0);
        if (!r) pb_result(&b, 0, "looking");
        w = traffic(50, 50, &lt, &lr);
        CHECK(r == 0 && !b.down && M.online && logged("answers again") && !w && !lt && !lr,
              "it comes back restarted: online again with the same MAC, link up, 50 each way (%u, %u+%u)", w, lt, lr);
    }

    /* ---- 13. the box restarted while the driver ran (it does not say): the next frame out makes it ask ---- */
    machine(1, 130, 0);
    start(1);
    m_box_restart();
    w = traffic(5, 0, &lt, &lr);        /* the box sends these on, and asks for the online frame */
    m_idle(1000000);
    pb_poll(&b, &sink, NB);
    w += traffic(20, 20, &lt, &lr);
    CHECK(M.online && b.online_asks == 1 && !w && !lr && !M.garbage_reads, "a box restarted unseen: it asks, the driver answers, frames in again (%u lost)", lr);

    /* ---- 14. a slow netd: no free buffer, the request waits; nothing lost ---- */
    machine(1, 140, 0);
    start(1);
    nfree_limit = 0;
    mkframe(&f8, 300);
    m_net_in(f8.d, 300);
    m_idle(1000000);
    pb_poll(&b, &sink, NB);
    int waited = ngot == 0 && M.req_pending;
    nfree_limit = NB;
    pb_poll(&b, &sink, NB);
    CHECK(waited && ngot == 1 && same(&got[0], &f8), "no free buffer: the box's request waits; a buffer back, the frame comes");

    /* ---- 15. the box's loopback test frame is sent back as it came ---- */
    machine(1, 150, 0);
    start(1);
    mkframe(&f8, 100);
    f8.d[12] = 0xFF; f8.d[13] = 0xFD;
    m_net_in(f8.d, 100);
    m_idle(1000000);
    pb_poll(&b, &sink, NB);
    m_idle(2000000);
    CHECK(b.loops == 1 && ngot == 0 && M.magic_loop == 2, "a loopback frame from the box: sent back, not given to netd");

    /* ---- 16. the end: the offline frame, the port let go; the quiesce program as the kernel checks it ---- */
    machine(1, 160, 0);
    start(1);
    pb_stop(&b);
    m_idle(1000000);
    CHECK(!M.online && M.magic_offline == 1 && lines_off() && lines_clean(), "the end: the box offline, the port as found");
    pb_quiesce(&qz);
    int qok = qz.n == 3;
    for (u32 k = 0; k < qz.n; k++) {    /* kernel/drv.c's par_step_ok */
        const struct udrvstep *s = &qz.step[k];
        if (s->size != 1) qok = 0;
        else if (s->addr == PB_DATA || s->addr == PB_DDATA) ;
        else if (s->addr == PB_CTRL || s->addr == PB_DCTRL) { if (s->op != DRVS_RMW || (s->mask & ~7u) || (s->value & ~s->mask)) qok = 0; }
        else qok = 0;
    }
    machine(1, 161, 0);
    start(1);
    m_quiesce(&qz);
    CHECK(qok && lines_off() && lines_clean(), "the quiesce program: 3 steps the kernel accepts; SEL down, the port as found, the serial bits kept");

    /* ---- 17. why not the box's burst receive: with the gaps of a process, bytes come wrong, and nothing sees it ---- */
    {
        unsigned wrong0 = 0, wrong1 = 0;
        for (int gaps = 0; gaps < 2; gaps++) {
            machine(1, 170 + (uint64_t)gaps, 0);
            start(1);
            M.gap_p = gaps ? 0.005 : 0; M.gap_min = 5000; M.gap_max = 50000;
            for (int i = 0; i < 200; i++) {
                struct mframe f, g;
                mkframe(&f, rlen());
                m_net_in(f.d, f.len);
                m_idle(500000);
                unsigned n = 0;
                if (!burst_recv_as_amigaos(g.d, &n)) { g.len = n; if (!same(&f, &g)) { if (gaps) wrong1++; else wrong0++; } }
                b.acks = M.acks;
                m_idle(500000);
            }
        }
        CHECK(wrong0 == 0 && wrong1 > 20, "the box's burst receive (0x44) as AmigaOS does it: 0 of 200 wrong without gaps, %u of 200 wrong with them: not used", wrong1);
    }

    printf(bad ? "plipbox-test: %d FAILED\n" : "plipbox-test: all passed\n", bad);
    return bad != 0;
}
