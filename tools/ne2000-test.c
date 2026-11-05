/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the PC Card NE2000 driver's slot, CIS and chip code (net/drv/ne2000-core.c) against the model in
 * tools/ne2000-model.h (DESIGN D36): an FA411-like AX88790 card, an AX88190 whose PHY is powered down, a plain
 * DP8390 card, a card named FA411 with a DP8390 inside, an 8-bit NE1000, a memory card, an empty slot, damaged CISes;
 * frames both ways with the ring wrapping, a slow netd, an overrun, a header that cannot be right, a stuck send, the
 * link going down and up, the card taken out in the middle of traffic and put back, the quiesce program.
 * Build and run: tools/ne2000-test.sh   (TRACE=1 shows the driver's log) */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
#include "../kernel/netproto.h"
#include "../net/drv/ne2000.h"
#include "ne2000-model.h"

static char lastlog[64][200];
static int nlog;
static u32 ne_ticks(void) { M.us += 20; return (u32)(M.us / 10000); }
static void ne_sleep(u32 t) { M.us += t * 10000; }
static void ne_log(struct ne *n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lastlog[nlog % 64], sizeof lastlog[0], fmt, ap);
    va_end(ap);
    if (m_trace) printf("    ne2000: %s: %s\n", n->ifn, lastlog[nlog % 64]);
    nlog++;
}
static int logged(const char *s)
{
    for (int i = nlog > 64 ? nlog - 64 : 0; i < nlog; i++) if (strstr(lastlog[i % 64], s)) return 1;
    return 0;
}

#include "../net/drv/ne2000-core.c"

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); bad++; } else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- the cards' CISes (each a list of CIS bytes) ---- */
/* An FA411 as Linux and FreeBSD know it (the strings, an AX88x90, its registers at 0x3c0 with IOBASE registers): the
 * rest of the CIS (MANFID, the entry's I/O lines) is a stand-in until the real card's CIS is read on the A1200 */
static const unsigned char CIS_FA411[] = {
    0x15, 0x1A, 0x04, 0x01, 'N', 'E', 'T', 'G', 'E', 'A', 'R', 0, 'F', 'A', '4', '1', '1', 0,
    'F', 'a', 's', 't', ' ', 'E', 't', 'h', 0, 0xFF,
    0x20, 0x04, 0x49, 0x01, 0x11, 0x04,
    0x21, 0x02, 0x06, 0x00,
    0x1A, 0x05, 0x01, 0x05, 0xC0, 0x03, 0x63,                          /* registers at 0x3c0: COR, CCSR, IOBASE0/1 */
    0x1B, 0x07, 0xC1, 0x41, 0x09, 0x01, 0x55, 0x6A, 0x00,              /* entry 1, default: 5 V, any base on 10 lines */
    0x1B, 0x02, 0x02, 0x00,                                            /* entry 2: as the default (empty features) */
    0xFF };
/* a plain NE2000 card: registers at 0x3f8 (COR only), entry 0x20 at 0x300-0x31f, with power and timing to skip */
static const unsigned char CIS_NE[] = {
    0x01, 0x03, 0x00, 0x00, 0xFF,                                       /* DEVICE: skipped */
    0x15, 0x0E, 0x04, 0x01, 'A', 'c', 'm', 'e', 0, 'N', 'E', '2', '0', '0', '0', 0,
    0x21, 0x02, 0x06, 0x00,
    0x1A, 0x05, 0x01, 0x23, 0xF8, 0x03, 0x01,
    0x1B, 0x0E, 0xE0, 0x01, 0x0D, 0x03, 0xB5, 0x1E, 0x55, 0xFD, 0x35, 0x80 | 0x05, 0x60, 0x00, 0x03, 0x1F,
    0x14, 0x00,                                                         /* NO_LINK */
    0xFF };
/* a memory card: no network function, no configuration */
static const unsigned char CIS_SRAM[] = { 0x01, 0x03, 0x53, 0x06, 0xFF, 0x21, 0x02, 0x01, 0x00, 0xFF };

static const unsigned char MAC[6] = { 0x00, 0x40, 0xF4, 0x12, 0x34, 0x56 };
static struct ne n;

static void card(int chip, const unsigned char *cis, unsigned len, unsigned cor_at, int iobase_regs, unsigned fixed_io,
                 unsigned cor_index)
{
    m_card(chip, MAC);
    M.cor_index = cor_index;
    m_cis(cis, len);
    M.cor_at = cor_at;
    M.has_iobase = iobase_regs;
    M.fixed_io = fixed_io;
    memset(&n, 0, sizeof n);
    n.ifn = "eth0";
}
/* as ne2000.c's start: -> 0 or the first step's error */
static int bring_up(void)
{
    if (!ne_card_in()) return -ENXIO;
    return ne_bring_up(&n);
}

/* ---- netd: 8 buffers, frames checked as they come ---- */
static unsigned char bufs[8][NE_BUF];
static int held[8], slow;
static unsigned got, wrong, expect_next;
static u32 seeds[100000];
static unsigned lens[100000], nexp;
static void mkframe(unsigned char *f, unsigned len, const unsigned char *dst, u32 seed)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, "\x02\x00\x00\x00\x00\x01", 6);
    f[12] = 0x08; f[13] = 0x00;
    for (unsigned i = 14; i < len; i++) { seed = seed * 1103515245u + 12345u; f[i] = (unsigned char)(seed >> 16); }
}
static int s_take(void *ctx, unsigned char **p)
{
    (void)ctx;
    if (slow) return -1;
    for (int i = 0; i < 8; i++) if (!held[i]) { held[i] = 1; *p = bufs[i]; return i; }
    return -1;
}
static void s_give(void *ctx, int b, unsigned len)
{
    (void)ctx;
    unsigned char f[1600];
    /* the next frame we sent that was not dropped: frames are counted by seed in order */
    while (expect_next < nexp) {
        mkframe(f, lens[expect_next], MAC, seeds[expect_next]);
        if (lens[expect_next] == len && !memcmp(f, bufs[b], len)) break;
        expect_next++;                                  /* this one was dropped (an overrun, a slow netd) */
    }
    if (expect_next >= nexp) wrong++;
    else expect_next++;
    got++;
    held[b] = 0;                                        /* netd copies it out at once */
}
static const struct ne_sink sink = { s_take, s_give, 0 };

static u32 rnd = 12345;
static u32 rand32(void) { rnd = rnd * 1664525u + 1013904223u; return rnd; }

/* frames to the Amiga: count of them, each delivered then the driver polls every `every` frames */
static unsigned feed(unsigned count, unsigned every, unsigned maxlen)
{
    unsigned char f[1600];
    unsigned in = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned len = 60 + rand32() % (maxlen - 59);
        u32 seed = rand32();
        mkframe(f, len, MAC, seed);
        if (m_receive(f, len)) { seeds[nexp] = seed; lens[nexp] = len; nexp++; in++; }
        if ((i + 1) % every == 0) {
            int r = ne_poll(&n, &sink, 16);
            if (r < 0) { ne_start(&n); }
        }
    }
    for (int k = 0; k < 20; k++) if (ne_poll(&n, &sink, 16) < 0) ne_start(&n);
    return in;
}

static void test_fa411(void)
{
    printf("-- an FA411-like card: an AX88790 with its internal PHY --\n");
    card(CHIP_AX88790, CIS_FA411, sizeof CIS_FA411, 0x3C0, 1, 0, 5);
    int r = bring_up();
    CHECK(r == 0, "the card brought up (%d)", r);
    CHECK(!strcmp(n.cis.vers[0], "NETGEAR") && !strcmp(n.cis.vers[1], "FA411") && n.cis.funcid == FUNCID_NETWORK &&
          n.cis.manf == 0x0149 && n.cis.card == 0x0411, "the CIS read: \"%s\" \"%s\", function %u, manufacturer %x card %x",
          n.cis.vers[0], n.cis.vers[1], n.cis.funcid, n.cis.manf, n.cis.card);
    CHECK(n.cis.cor == 0x3C0 && n.cis.rmask == 0x63 && n.cis.index == 1 && n.cis.anybase && n.io == NE_IOBASE,
          "configuration: registers at %x (mask %x), entry %u, any base -> %x", n.cis.cor, n.cis.rmask, n.cis.index, n.io);
    CHECK(M.cor == (0x05 | COR_LEVEL) && M.iob0 == 0x00 && M.iob1 == 0x03 && M.ccsr == 0,
          "the card configured as Linux's axnet_cs does: COR %x, IOBASE %x%02x, CCSR %x", M.cor, M.iob1, M.iob0, M.ccsr);
    CHECK(M.gcfg == CFG_0V_250NS && (M.gcs & CS_WR) && !M.gayle_irq_writes, "Gayle: 0 V, 250 ns, writes on, its interrupt registers untouched");
    CHECK(n.ax == 2 && M.gpio == 0x10 && n.phy == 0x10, "the chip: AX88790 (TEST register), the internal PHY selected and found at %d", n.phy);
    CHECK(!memcmp(n.mac, MAC, 6) && !memcmp(M.par, MAC, 6), "the MAC from 0x400 in its memory, in the station address");
    CHECK(M.mac_reads == 2 && !logged("the MAC address"), "the MAC read twice, the same, no word said (%u reads)", M.mac_reads);

    /* the real FA411 once read ff:ff:ff:ff:36:8b (2026-11-04): reads that come out wrong first */
    for (unsigned flaky = 1; flaky <= 5; flaky += 2) {
        card(CHIP_AX88790, CIS_FA411, sizeof CIS_FA411, 0x3C0, 1, 0, 5);
        M.mac_flaky = flaky;
        nlog = 0;
        unsigned long us0 = M.us;
        r = bring_up();
        CHECK(r == 0 && !memcmp(n.mac, MAC, 6) && logged("the MAC address read as ff:ff:ff:ff:34:56") &&
              logged("the MAC address read right on try"), "%u flaky reads of the MAC (ff:ff:ff:ff:34:56): read again after "
              "a settle of the CIA's ticks, the right one taken, what was read said (%u reads, %.0f ms)", flaky, M.mac_reads,
              (double)(M.us - us0) / 1000);
    }
    card(CHIP_AX88790, CIS_FA411, sizeof CIS_FA411, 0x3C0, 1, 0, 5);
    M.mac_flaky = 100;
    nlog = 0;
    r = bring_up();
    CHECK(r != 0 && M.mac_reads == 2 * NE_MACTRIES && logged("is not a usable one"), "a MAC that never reads right: %d tries, "
          "then the card refused, \"not a usable one\" (%u reads)", NE_MACTRIES, M.mac_reads);
    card(CHIP_AX88790, CIS_FA411, sizeof CIS_FA411, 0x3C0, 1, 0, 5);
    r = bring_up();
    CHECK(M.imr == 0 && (M.cr & CR_STA) && M.rcr == (RCR_AB | RCR_AX_INTT) && M.dcr == DCR_WORD && M.pstart == NE_RXSTART &&
          M.pstop == NE_MEMEND && M.curr == NE_RXSTART, "started: polled (IMR 0), broadcasts, word mode, the ring %x-%x", M.pstart, M.pstop);
    M.link = 0;
    M.link_latched_low = 1;
    ne_link_poll(&n, 1);
    CHECK(!n.link, "no cable: the link is down");
    M.link = 1;
    M.full100 = 1;
    M.us += NE_LINKEVERY * 10000 + 1;
    r = ne_link_poll(&n, 0);
    CHECK(r == 1 && n.link && n.full && n.speed == 100 && (M.tcr & TCR_AX_FDU) && logged("link up, 100 Mbit/s full duplex"),
          "the cable in: link up, 100 Mbit/s full duplex (TCR %x)", M.tcr);

    /* frames to the Amiga: 3,000, polled every 4: the ring wraps many times */
    unsigned in = feed(3000, 4, 1514);
    CHECK(in == 3000 && got == 3000 && !wrong && M.overruns_seen == 0, "3,000 frames received, checked byte by byte (%u got, %u wrong)", got, wrong);
    /* frames from the Amiga: 3,000, with the doorbell's batches */
    unsigned char f[1600];
    unsigned sent = 0, okw = 0;
    M.nsent = 0;
    for (unsigned i = 0; i < 3000; i++) {
        unsigned len = 14 + rand32() % 1501;
        u32 seed = rand32();
        mkframe(f, len, (const unsigned char *)"\x02\x11\x22\x33\x44\x55", seed);
        while (ne_tx(&n, f, len) == -ENOSPC) { if (ne_poll(&n, &sink, 16) < 0) ne_start(&n); }
        sent++;
        ne_poll(&n, &sink, 16);
        unsigned k = (M.nsent - 1) % 64, want = len < 60 ? 60 : len;
        unsigned char g[1600];
        memset(g, 0, sizeof g);
        mkframe(g, len, (const unsigned char *)"\x02\x11\x22\x33\x44\x55", seed);
        if (M.nsent == sent && M.sent_len[k] == want && !memcmp(M.sent[k], g, want)) okw++;
    }
    CHECK(okw == 3000, "3,000 frames sent (14-1514 bytes, the short ones padded to 60), each on the wire as given (%u ok)", okw);
    /* both ways at once */
    unsigned g0 = got;
    for (unsigned i = 0; i < 500; i++) {
        unsigned len = 60 + rand32() % 1455;
        u32 seed = rand32();
        mkframe(f, len, MAC, seed);
        if (m_receive(f, len)) { seeds[nexp] = seed; lens[nexp] = len; nexp++; }
        mkframe(f, 100, (const unsigned char *)"\xff\xff\xff\xff\xff\xff", seed);
        while (ne_tx(&n, f, 100) == -ENOSPC) ne_poll(&n, &sink, 16);
        ne_poll(&n, &sink, 16);
    }
    for (int k = 0; k < 10; k++) ne_poll(&n, &sink, 16);
    CHECK(got - g0 == 500 && !wrong, "500 frames each way at once (%u received)", got - g0);
    CHECK(!M.faults, "the model saw nothing wrong so far (%u faults)", M.faults);

    /* a slow netd: no buffers, the frames are dropped and counted, nothing breaks */
    slow = 1;
    n.rx_drop = 0;
    feed(10, 1, 300);
    slow = 0;
    CHECK(n.rx_drop == 10, "a netd with no free buffer: 10 frames dropped and counted (%u)", (unsigned)n.rx_drop);
    g0 = got;
    feed(20, 2, 600);
    CHECK(got - g0 == 20 && !wrong, "then frames flow again (%u)", got - g0);

    /* an overrun: 200 frames arrive without a look */
    g0 = got;
    unsigned ov0 = n.overruns;
    for (unsigned i = 0; i < 200; i++) {
        unsigned len = 1000;
        u32 seed = rand32();
        mkframe(f, len, MAC, seed);
        if (m_receive(f, len)) { seeds[nexp] = seed; lens[nexp] = len; nexp++; }
    }
    unsigned kept = got;
    for (int k = 0; k < 10; k++) ne_poll(&n, &sink, 16);
    CHECK(M.overruns_seen > 0 && n.overruns > ov0 && !(M.isr & ISR_OVW) && (M.cr & CR_STA) && !(M.tcr & TCR_LB) && !wrong,
          "an overrun: National's procedure, the frames that fitted read (%u), OVW cleared, out of loopback", got - kept);
    feed(50, 3, 1514);
    CHECK(!wrong && got - g0 >= 50, "frames flow after the overrun");

    /* a header that cannot be right: the chip is started again */
    M.mem[(unsigned)M.curr << 8 | 1] = 0x10;            /* a next page below the ring, written where the next frame lands */
    mkframe(f, 200, MAC, 1);
    unsigned at = M.curr;
    m_receive(f, 200);
    M.mem[at << 8 | 1] = 0x10;
    r = ne_poll(&n, &sink, 16);
    CHECK(r == -EIO && logged("a receive header that cannot be right"), "a damaged receive header: -EIO, said (%d)", r);
    r = ne_start(&n);
    g0 = got;
    feed(30, 2, 1514);
    CHECK(r == 0 && got - g0 == 30 && !wrong, "started again, frames flow (%u)", got - g0);

    /* a header whose next page is its own page (it would be read for ever): -EIO too */
    at = M.curr;
    mkframe(f, 200, MAC, 2);
    m_receive(f, 200);
    M.mem[at << 8 | 1] = (unsigned char)at;
    r = ne_poll(&n, &sink, 16);
    CHECK(r == -EIO, "a receive header pointing at its own page: -EIO (%d)", r);
    ne_start(&n);

    /* a stuck send: one second, then -EIO */
    M.stuck_tx = 1;
    mkframe(f, 100, (const unsigned char *)"\x02\x11\x22\x33\x44\x55", 7);
    ne_tx(&n, f, 100);
    r = 0;
    for (int k = 0; k < 300 && r >= 0; k++) { M.us += 10000; r = ne_poll(&n, &sink, 16); }
    CHECK(r == -EIO && logged("a send did not end in 1 s"), "a send that never ends: -EIO after a second, said");
    M.stuck_tx = 0;
    ne_start(&n);

    /* the quiesce program: the steps stop the chip and unconfigure the card, inside the slot's window */
    struct udrvquiesce qz;
    ne_quiesce(&n, &qz);
    int inside = 1;
    for (u32 k = 0; k < qz.n; k++)
        if (qz.step[k].addr < DRV_PCMCIA || qz.step[k].addr >= DRV_PCMCIA + DRV_PCMCIA_SIZE || qz.step[k].size != 1) inside = 0;
    for (u32 k = 0; k < qz.n; k++) ne_wb(qz.step[k].addr, (unsigned char)qz.step[k].value);
    CHECK(inside && qz.n == 2 && (M.cr & CR_STP) && !(M.cr & CR_STA) && M.cor == 0, "the quiesce program: %u byte writes in the slot's window stop the chip and unconfigure the card", (unsigned)qz.n);

    /* the card's comings and goings, as ne2000.c's loop drives them: ne_watch at the top of every turn (a tick
     * apart here), then a look at the chip while the card is in */
    r = bring_up();
    feed(20, 2, 1514);
    unsigned ev[8] = { 0 };
    #define TURN() do { int e_ = ne_watch(&n); ev[e_]++; if (!n.gone && ne_poll(&n, &sink, 16) < 0 && ne_card_in()) ne_start(&n); M.us += 10000; } while (0)
    M.present = 0;                                      /* out in the middle of a look at the chip */
    int pr = ne_poll(&n, &sink, 16);
    unsigned late0 = M.late_access, got0 = got;
    for (int t = 0; t < 30; t++) TURN();
    CHECK(pr == -EIO && ev[NE_LEFT] == 1 && ev[NE_AWAY] == 29 && M.late_access == late0 && got == got0 && n.gone && !n.link &&
          logged("the card left the slot"), "the card out during a look: -EIO, then \"left\" once, away for 29 turns, nothing touched, no frame made up");
    memset(ev, 0, sizeof ev);
    for (int t = 0; t < 40; t++) { if (t & 1) m_insert(); else M.present = 0; TURN(); }    /* it bounces */
    CHECK(ev[NE_BACK] == 0 && ev[NE_AWAY] == 40 && M.cor == 0, "a card bouncing in and out for 0.4 s: not touched (COR %x)", M.cor);
    m_insert();
    memset(ev, 0, sizeof ev);
    for (int t = 0; t < 60; t++) TURN();
    g0 = got;
    expect_next = nexp;
    feed(40, 2, 1514);
    CHECK(ev[NE_BACK] == 1 && ev[NE_AWAY] >= 45 && !n.gone && n.link && M.cor == 0x45 && got - g0 == 40 && !wrong && logged("the card is back"),
          "in for half a second: back once (after %u turns away), configured, the link up, 40 frames flow", ev[NE_AWAY]);
    M.present = 0;
    TURN();
    static const unsigned char MAC2[6] = { 0x00, 0x40, 0xF4, 0x65, 0x43, 0x21 };
    m_set_mac(MAC2);
    m_insert();
    memset(ev, 0, sizeof ev);
    for (int t = 0; t < 60 && !ev[NE_OTHER]; t++) TURN();
    CHECK(ev[NE_OTHER] == 1 && M.cor == 0 && n.gone && !memcmp(n.mac, MAC2, 6), "another card put in: NE_OTHER (the driver ends, netd starts it for the new address), left unconfigured");
    m_set_mac(MAC);
    M.present = 0;
    TURN();
    m_cis(CIS_SRAM, sizeof CIS_SRAM);
    m_insert();
    memset(ev, 0, sizeof ev);
    for (int t = 0; t < 60 && !ev[NE_BAD]; t++) TURN();
    CHECK(ev[NE_BAD] == 1 && M.cor == 0 && logged("not a network card"), "a memory card put in instead: NE_BAD, not touched");
    m_cis(CIS_FA411, sizeof CIS_FA411);
    CHECK(!M.faults, "no model faults in the FA411 run (%u)", M.faults);
}

static void test_ax88190_pwrdwn(void)
{
    printf("-- an AX88190 whose PHY is powered down until the CCSR bit --\n");
    card(CHIP_AX88190, CIS_FA411, sizeof CIS_FA411, 0x3C0, 1, 0, 5);
    M.phy_powered = 0;
    M.phy_addr = 3;
    int r = bring_up();
    CHECK(r == 0 && n.ax == 1 && n.phy == 3 && (M.ccsr & CCSR_PWRDWN), "the PHY found at 3 after the CCSR write (%d, ax %d)", r, n.ax);
    unsigned g0 = got;
    expect_next = nexp;
    feed(100, 3, 1514);
    CHECK(got - g0 == 100 && !wrong && !M.faults, "100 frames (%u), no faults", got - g0);
}

static void test_dp8390(void)
{
    printf("-- a plain NE2000 card (a DP8390) --\n");
    card(CHIP_DP8390, CIS_NE, sizeof CIS_NE, 0x3F8, 0, 0x300, 0x20);
    int r = bring_up();
    CHECK(r == 0 && !n.ax && n.phy < 0 && n.cis.index == 0x20 && n.io == 0x300 && !n.cis.anybase && M.cor == 0x20,
          "brought up: entry %x at %x (the power and timing descriptions skipped), COR %x (%d)", n.cis.index, n.io, M.cor, r);
    CHECK(!memcmp(n.mac, MAC, 6) && !strcmp(n.cis.vers[0], "Acme"), "the MAC from the doubled PROM");
    card(CHIP_DP8390, CIS_NE, sizeof CIS_NE, 0x3F8, 0, 0x300, 0x20);
    M.mac_flaky = 2;
    nlog = 0;
    r = bring_up();
    CHECK(r == 0 && !memcmp(n.mac, MAC, 6) && logged("read right on try 2"), "the PROM read wrong twice (ff:ff:ff:ff:..): "
          "read again, the right MAC taken (%u reads)", M.mac_reads);
    ne_link_poll(&n, 1);
    CHECK(n.link && M.rcr == RCR_AB, "no MII: the link taken as up; RCR %x", M.rcr);
    unsigned g0 = got;
    expect_next = nexp;
    feed(2000, 5, 1514);
    unsigned char f[1600];
    unsigned okw = 0;
    M.nsent = 0;
    for (unsigned i = 0; i < 1000; i++) {
        unsigned len = 60 + rand32() % 1455;
        mkframe(f, len, (const unsigned char *)"\x02\x11\x22\x33\x44\x55", i);
        while (ne_tx(&n, f, len) == -ENOSPC) ne_poll(&n, &sink, 16);
        ne_poll(&n, &sink, 16);
        if (M.sent_len[(M.nsent - 1) % 64] == len && !memcmp(M.sent[(M.nsent - 1) % 64], f, len)) okw++;
    }
    CHECK(got - g0 == 2000 && !wrong && okw == 1000 && !M.faults, "2,000 frames in, 1,000 out, checked (%u, %u), no faults (%u)", got - g0, okw, M.faults);
}

static void test_fa411_dp8390(void)
{
    printf("-- a card named FA411 with a DP8390 inside (FreeBSD: the DFE-670TXD shares its ID) --\n");
    card(CHIP_DP8390, CIS_FA411, sizeof CIS_FA411, 0x3C0, 1, 0, 1);
    int r = bring_up();
    CHECK(r == 0 && !n.ax && !memcmp(n.mac, MAC, 6) && M.cor == 1 && logged("no AX88x90 answers") && M.wrong_cfg > 0,
          "configuration 5 is not its own (no I/O answers): configured again as its CIS says (COR %x), read as a DP8390 (%d)", M.cor, r);
    unsigned g0 = got;
    expect_next = nexp;
    feed(100, 3, 1514);
    CHECK(got - g0 == 100 && !wrong && !M.faults, "100 frames (%u), no faults (%u)", got - g0, M.faults);
}

static void test_refusals(void)
{
    printf("-- cards and slots the driver refuses --\n");
    card(CHIP_NE1000, CIS_NE, sizeof CIS_NE, 0x3F8, 0, 0x300, 0x20);
    int r = bring_up();
    CHECK(r == -ENODEV && logged("8-bit NE1000"), "an 8-bit NE1000: refused (%d)", r);
    card(CHIP_DP8390, CIS_SRAM, sizeof CIS_SRAM, 0xFFFF, 0, 0, 0x20);
    r = bring_up();
    CHECK(r == -ENODEV && n.cis.funcid == 1 && M.cor == 0 && logged("not a network card (function 1): left alone"),
          "a memory card: function %u, \"not a network card\", nothing written (%d)", n.cis.funcid, r);
    card(CHIP_DP8390, CIS_NE, sizeof CIS_NE, 0x3F8, 0, 0x300, 0x20);
    M.present = 0;
    r = bring_up();
    CHECK(r == -ENXIO && !ne_card_in(), "an empty slot: no card (%d)", r);
    card(CHIP_DP8390, CIS_NE, sizeof CIS_NE, 0x3F8, 0, 0x300, 0x20);
    memset(M.attr, 0xFF, sizeof M.attr);
    r = bring_up();
    CHECK(r == -ENODEV, "a card with no CIS: refused (%d)", r);
    /* the chip does not answer (a card whose I/O decode is elsewhere): the reset times out */
    card(CHIP_DP8390, CIS_NE, sizeof CIS_NE, 0x3F8, 0, 0x340, 0x20);
    int f0 = (int)M.faults;
    M.quiet = 1;
    ne_slot_reset();
    ne_read_cis(&n);
    ne_card_on(&n);
    r = ne_probe(&n);
    CHECK(r == -ENODEV && (int)M.faults > f0, "a card that does not decode the ports given: no chip, refused (%d)", r);
    M.faults = 0;
}

/* damaged CISes: every mutation of the two good ones read without reading or configuring outside */
static void test_fuzz(void)
{
    printf("-- 20,000 damaged CISes --\n");
    unsigned accepted = 0, outside = 0;
    for (int i = 0; i < 20000; i++) {
        const unsigned char *src = i & 1 ? CIS_FA411 : CIS_NE;
        unsigned len = i & 1 ? sizeof CIS_FA411 : sizeof CIS_NE;
        unsigned char c[512];
        memcpy(c, src, len);
        for (int k = 0, nm = 1 + (int)(rand32() % 6); k < nm; k++) c[rand32() % len] = (unsigned char)rand32();
        if (i % 7 == 0) for (unsigned k = len; k < sizeof c; k++) c[k] = (unsigned char)rand32();   /* no end tuple */
        card(CHIP_DP8390, c, i % 7 == 0 ? sizeof c : len, 0xFFFF, 0, 0x300, 0x20);
        if (ne_read_cis(&n) == 0 && n.cis.index) {
            accepted++;
            if ((n.cis.cor != 0xFFFFFFFF && (n.cis.cor & 1 || n.cis.cor > PC_ATTR_SIZE / 2 - 0x20)) || n.cis.iobase > 0xFFE0 || (n.cis.iobase & 31))
                outside++;
        }
    }
    CHECK(!outside, "%u accepted, none with registers or ports outside the slot (ASan watches the reads)", accepted);
}

int main(void)
{
    m_trace = getenv("TRACE") != 0;
    test_fa411();
    test_ax88190_pwrdwn();
    test_dp8390();
    test_fa411_dp8390();
    test_refusals();
    test_fuzz();
    printf("ne2000-test: %s\n", bad ? "FAIL" : "all passed");
    return bad != 0;
}
