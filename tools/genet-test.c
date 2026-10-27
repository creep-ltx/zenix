/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the CM4 Ethernet driver's chip code (net/drv/genet-core.c) against the model in tools/genet-model.h
 * (DESIGN D19 step 9): the bring-up from a used chip, the PHY and its RGMII delays, the link up, down and at another
 * speed, frames both ways through netd-like buffers with the ring and the 16-bit indexes wrapping, a slow netd (the
 * chip's own drops), bad frames, a stuck send DMA and a confused index (the chip restarted), the quiesce program
 * stopping the chip in the middle of traffic, the self-test with working and with broken cache calls, a missing
 * loopback, and the model's own strictness. Build and run: tools/genet-test.sh   (TRACE=1 shows the driver's log) */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#include "../kernel/netproto.h"
#include "../net/drv/genet.h"
#include "genet-model.h"

static int broken_cache;                /* the cache calls do nothing (a test of the self-test) */
static char lastlog[64][200];
static int nlog;

static u32 gn_rd(struct genet *g, u32 off) { (void)g; return m_rd(off); }
static void gn_wr(struct genet *g, u32 off, u32 v) { (void)g; m_wr(off, v); }
static u32 gn_ticks(void) { return m_ticks(); }
static void gn_sleep(u32 t) { M.us += t * 10000; m_step(); }
static void gn_cache(struct genet *g, unsigned how, u32 a, u32 n) { (void)g; if (!broken_cache) m_cache(how, a, n); }
static unsigned char *gn_mem(u32 a)
{
    if (!m_addr_ok(a, 1)) { FAULT("the driver reaches outside the memory: %x", a); return M.cpu; }
    return M.cpu + (a - M_BASE);
}
static void gn_log(struct genet *g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lastlog[nlog % 64], sizeof lastlog[0], fmt, ap);
    va_end(ap);
    if (m_trace) printf("    genet: %s: %s\n", g->ifn, lastlog[nlog % 64]);
    nlog++;
}
static int logged(const char *s)
{
    for (int i = nlog > 64 ? nlog - 64 : 0; i < nlog; i++) if (strstr(lastlog[i % 64], s)) return 1;
    return 0;
}

#include "../net/drv/genet-core.c"

/* stage0's stop of a GENET a previous owner left running (boot/genet-stop.h): the registers as the chip means them,
 * the beam's line moving every 64 us of the model's clock */
static u32 gs_rd(u32 base, u32 off) { (void)base; return m_rd(off); }
static void gs_wr(u32 base, u32 off, u32 v) { (void)base; m_wr(off, v); }
static u32 gs_clock(void) { M.us += 20; return M.us / 64; }
#include "../boot/genet-stop.h"

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); bad++; } else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- netd, as it treats a DMA interface (net/netd/iface.c): its 32 receive buffers and 16 send buffers in the
 * region; a received frame copied out (through the CPU's view: the cache) and its buffer given back at once; a frame
 * to send written into a send buffer by the CPU (a dirty line until cleaned) and its tag held until it is done ---- */
static const unsigned char MAC[6] = { 0xdc, 0xa6, 0x32, 0x11, 0x22, 0x33 };
#define RXB(i) (M_BASE + (u32)(i) * GN_BUF)
#define TXB(i) (M_BASE + (u32)(GN_NRX + (i)) * GN_BUF)
static struct genet g;
static u32 expect_seed[8192];           /* each frame sent to the Amiga (made again from its seed), in order */
static unsigned expect_len[8192], expect_dst[8192], nexp, ngot, wrong;
static int slow;                        /* netd takes no frame this turn */
static int txbusy[16];
static unsigned donecount;
static int give_back[GN_NRX], ngive;

static void mkframe(unsigned char *f, unsigned len, const unsigned char *dst, u32 seed);
static int netd_rx(void *ctx, int b, unsigned len)
{
    (void)ctx;
    if (slow) return -1;
    unsigned char *p = M.cpu + (RXB(b) - M_BASE), f[1600];
    unsigned i = ngot % 8192;
    if (ngot < nexp) mkframe(f, expect_len[i], expect_dst[i] == 1 ? MAC : (const unsigned char *)"\xff\xff\xff\xff\xff\xff", expect_seed[i]);
    if (ngot >= nexp || len != expect_len[i] || memcmp(p, f, len)) {
        if (wrong++ < 3) printf("    frame %u: %u bytes, expected %u%s\n", ngot, len, ngot < nexp ? expect_len[ngot] : 0, " (or other bytes)");
    }
    ngot++;
    give_back[ngive++] = b;
    return 0;
}
static void netd_done(void *ctx, unsigned tag)
{
    (void)ctx;
    if (tag >= 16 || !txbusy[tag]) { FAULT("a tag given back that netd did not lend: %u", tag); return; }
    txbusy[tag] = 0;
    donecount++;
}
static const struct gn_sink sink = { netd_rx, netd_done, 0 };

static void mkframe(unsigned char *f, unsigned len, const unsigned char *dst, u32 seed)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, "\x02\x00\x00\x00\x00\x01", 6);
    f[12] = 0x08; f[13] = 0x00;
    for (unsigned i = 14; i < len; i++) { seed = seed * 1103515245u + 12345u; f[i] = (unsigned char)(seed >> 16); }
}
static void wire_in(unsigned len, u32 seed, int to_us, unsigned flags)
{
    static const unsigned char other[6] = { 0x02, 0x99, 0x99, 0x99, 0x99, 0x99 }, bc[6] = { 255, 255, 255, 255, 255, 255 };
    unsigned char f[1600];
    mkframe(f, len, to_us == 1 ? MAC : to_us == 2 ? bc : other, seed);
    if (to_us && !(flags & DS_RX_ERRORS)) {
        expect_seed[nexp % 8192] = seed;
        expect_len[nexp % 8192] = len;
        expect_dst[nexp % 8192] = (unsigned)to_us;
        nexp++;
    }
    m_wire(f, len, flags);
}
/* one turn of the driver's loop as genet.c runs it: done tags, received frames, then netd's answer (buffers back) */
static int turn(void)
{
    int r = gn_tx_done(&g, &sink);
    if (r >= 0) r = gn_rx(&g, &sink);
    if (r < 0) { gn_restart(&g, &sink); r = 1; }
    for (int i = 0; i < ngive; i++) gn_give(&g, give_back[i]);
    ngive = 0;
    gn_refill(&g);
    gn_link_poll(&g, 0);
    return r;
}
/* netd sends a frame: written by the CPU into a free send buffer, its tag lent to the driver */
static unsigned sent_seed[8192], sent_len[8192], nsent;
static int netd_send(unsigned len, u32 seed)
{
    int t = -1;
    for (int i = 0; i < 16; i++) if (!txbusy[i]) { t = i; break; }
    if (t < 0) return -1;
    unsigned char *p = M.cpu + (TXB(t) - M_BASE);
    mkframe(p, len, (const unsigned char *)"\x02\x00\x00\x00\x00\x01", seed);
    txbusy[t] = 1;
    int r = gn_tx(&g, (unsigned)t, TXB(t), len);
    if (r < 0) { txbusy[t] = 0; return r; }
    sent_seed[nsent % 8192] = seed;
    sent_len[nsent % 8192] = len;
    nsent++;
    return 0;
}
static int check_out(unsigned from)
{
    int ok = 1;
    for (unsigned i = 0; i < M.nout; i++) {
        unsigned char f[1600];
        unsigned n = sent_len[(from + i) % 8192];
        mkframe(f, n, (const unsigned char *)"\x02\x00\x00\x00\x00\x01", sent_seed[(from + i) % 8192]);
        if (n < 60) memset(f + n, 0, 60 - n), n = 60;
        if (M.out[i].len != n || memcmp(M.out[i].d, f, n)) { ok = 0; if (m_trace) printf("    sent frame %u differs\n", i); }
    }
    return ok;
}

static void bring_up(int phyaddr)
{
    memset(&g, 0, sizeof g);
    g.ifn = "eth0";
    g.base = 0xF7580000;
    memcpy(g.mac, MAC, 6);
    nexp = ngot = wrong = 0;
    memset(txbusy, 0, sizeof txbusy);
    gn_probe(&g);
    gn_reset(&g);
    gn_phy_start(&g, phyaddr);
    g.nbuf = GN_NRX;
    for (int i = 0; i < GN_NRX; i++) { g.buf[i] = RXB(i); gn_give(&g, i); }
    gn_rings(&g);
}
static int wait_link(int up)
{
    for (int i = 0; i < 400; i++) {
        gn_sleep(1);
        gn_link_poll(&g, 0);
        if (g.link == up) return 1;
    }
    return 0;
}

int main(void)
{
    m_trace = getenv("TRACE") != NULL;

    /* 1. the chip: a GENET v4 refused, v5 taken; the reset undoes what a driver before left */
    m_init();
    M.R[SYS_REV_CTRL / 4] = 0x05020000;
    memset(&g, 0, sizeof g);
    g.ifn = "eth0";
    g.base = 0xF7580000;
    CHECK(gn_probe(&g) == -ENODEV && logged("not a GENET v5"), "a GENET v4 is refused (\"not a GENET v5\")");
    m_init();
    bring_up(1);
    CHECK(logged("GENET v5.1 at f7580000"), "GENET v5.1 found");
    CHECK(!(M.R[RBUF_CTRL / 4] & (RBUF_64B_EN | RBUF_ALIGN_2B)) && !M.R[TBUF_CTRL / 4] && !(M.R[RBUF_ENERGY_CTRL / 4] & 3),
          "the reset turns off the status blocks, the 2-byte pad and EEE that the last user left on");
    CHECK(M.R[(INTRL2_0 + INTRL2_CPU_MASK_SET) / 4] == 0xffffffff && M.R[(INTRL2_1 + INTRL2_CPU_MASK_SET) / 4] == 0xffffffff,
          "every interrupt masked at the chip (polled; never at the GIC)");
    CHECK(M.R[SYS_PORT_CTRL / 4] == PORT_MODE_EXT_GPHY && (M.R[EXT_RGMII_OOB_CTRL / 4] & (RGMII_MODE_EN | ID_MODE_DIS | OOB_DISABLE)) == RGMII_MODE_EN,
          "RGMII to an external PHY, the MAC delays the send clock (ID_MODE_DIS clear)");
    CHECK(M.R[UMAC_MAC0 / 4] == 0xdca63211 && M.R[UMAC_MAC1 / 4] == 0x2233 && M.R[UMAC_MDF_CTRL / 4] == 3u << 15 &&
          M.R[UMAC_MAX_FRAME_LEN / 4] == 1536, "the MAC address, the receive filter (broadcast and ours) and the 1536-byte limit");
    CHECK(M.R[RDMA_CTRL / 4] == (DMA_RING16_EN | DMA_EN) && M.R[RDMA_RING_BUF_SIZE / 4] == (32u << 16 | 1536) &&
          M.R[TDMA_RING_BUF_SIZE / 4] == (16u << 16 | 1536) && M.rcons == M.rprod,
          "the rings: 32 receive descriptors all posted, 16 send, ring 16 on, the chip's index followed (%x %x %x %x %x)", M.rprod, M.rcons, M.R[RDMA_CTRL / 4], M.R[RDMA_RING_BUF_SIZE / 4], M.R[TDMA_RING_BUF_SIZE / 4]);

    /* 2. the PHY: found where the device tree says, reset, the RGMII "rxid" delays, AutogrEEEn off, negotiating */
    CHECK(g.phy == 1 && g.phy_id == BCM54213PE_ID && logged("(BCM54213PE)"), "the BCM54213PE at MDIO address 1");
    CHECK((M.misc & AUXCTL_MISC_RGMII_SKEW) && !(M.shd[SHD_CLK_CTL] & SHD_CLK_GTXCLK_EN) && !(M.mii_buf_cntl0 & AUTOGREEEN_EN),
          "the PHY delays the receive clock, not the send clock; AutogrEEEn off");
    CHECK(M.adv == (ADV_CSMA | ADV_10HALF | ADV_10FULL | ADV_100HALF | ADV_100FULL) && M.ctrl1000 == ADV_1000FULL,
          "advertised: 10/100 half and full, 1000 full, no pause");
    CHECK(wait_link(1) && g.speed == 1000 && g.full && ((M.R[UMAC_CMD / 4] >> 2) & 3) == 2 && (M.R[UMAC_CMD / 4] & 3) == 3 &&
          (M.R[EXT_RGMII_OOB_CTRL / 4] & RGMII_LINK) && logged("link up: 1000 Mbit/s, full duplex"),
          "link up after the negotiation: 1000 Mbit/s full duplex, the MAC set to it");

    /* 3. frames both ways: 3000 in (random sizes, ours and broadcast, others' filtered), 600 out; the 16-bit indexes
     * wrap (they started at fff0 and fff8) and the rings go round many times */
    u32 seed = 1;
    unsigned nsend = 0, from = 0;
    for (int i = 0; i < 3000; i++) {
        seed = seed * 69069 + 1;
        wire_in(14 + seed % 1501, seed, (i % 7) == 3 ? 0 : (i % 11) == 5 ? 2 : 1, 0);
        if (i % 5 == 0) {
            if (netd_send(14 + (seed >> 8) % 1501, seed ^ 0x5555) == 0) nsend++;
        }
        if (i % 3 == 0) turn();
        if (M.nout > 400) { if (!check_out(from)) bad++; from += M.nout; M.nout = 0; }
    }
    for (int i = 0; i < 20; i++) turn();
    int outok = check_out(from);
    from += M.nout;
    M.nout = 0;
    CHECK(ngot == nexp && !wrong, "3000 frames on the wire: %u for us arrived whole and in order (%u filtered: not ours)", ngot, M.filtered);
    CHECK(outok && donecount == nsend && from == nsend, "%u frames sent: on the wire byte for byte (short ones padded to 60), every tag back", nsend);
    CHECK(M.rprod < 0x1000 && M.tcons < 0x1000 && !M.faults, "the indexes wrapped (receive %x, send %x) with no fault", M.rprod, M.tcons);

    /* 4. a slow netd: the ring fills, the chip drops (counted), nothing is corrupted, then everything flows again */
    slow = 1;
    for (int i = 0; i < 50; i++) { seed = seed * 69069 + 1; wire_in(100 + seed % 900, seed, 1, 0); turn(); }
    slow = 0;
    unsigned dropped = M.rdisc;
    nexp -= dropped;                    /* the chip dropped the last ones: netd never sees them */
    for (int i = 0; i < 5; i++) turn();
    CHECK(dropped == 18 && g.rx_drop == 18 && ngot == nexp && !wrong && !M.faults,
          "netd slow: 32 frames wait in the ring, the chip drops 18 more and the driver counts them; the 32 arrive whole (%u %u %u %u %d)", dropped, g.rx_drop, ngot, nexp, wrong);
    g.rx_drop = 0;
    for (int i = 0; i < 100; i++) { seed = seed * 69069 + 1; wire_in(60 + seed % 1400, seed, 1, 0); turn(); }
    CHECK(ngot == nexp && !wrong, "and the next 100 arrive");

    /* 5. bad frames (CRC, overrun): counted, their buffers straight back in the ring */
    for (int i = 0; i < 40; i++) { seed = seed * 69069 + 1; wire_in(200, seed, 1, i % 2 ? 0x2 : 0); turn(); }
    CHECK(g.rx_bad == 20 && ngot == nexp && !wrong && !M.faults, "20 bad frames counted and dropped, the 20 good ones arrive");
    g.rx_bad = 0;

    /* 6. the link: the cable pulled (link down, frames to send refused), plugged into a 100 Mbit/s switch */
    m_cable(0);
    CHECK(wait_link(0) && !(M.R[EXT_RGMII_OOB_CTRL / 4] & RGMII_LINK) && logged("link down"), "cable pulled: link down, the MAC told");
    CHECK(netd_send(100, 7) == -ENETDOWN, "a frame to send without a link: refused (netd gets it back as an error)");
    M.partner = ADV_10HALF | ADV_10FULL | ADV_100HALF | ADV_100FULL;
    m_cable(1);
    CHECK(wait_link(1) && g.speed == 100 && ((M.R[UMAC_CMD / 4] >> 2) & 3) == 1 && logged("link up: 100 Mbit/s, full duplex"),
          "plugged into a 100 Mbit/s switch: link up at 100 full duplex");
    /* down and up again between two looks (at another speed): seen as down, then up at the new speed */
    M.partner = ADV_10HALF | ADV_10FULL;
    M.latched_down = 1;
    M.link = 0;
    M.aneg_at = M.us + 10;
    nlog = 0;
    for (int i = 0; i < 30 && !(g.link && g.speed == 10); i++) { gn_sleep(1); gn_link_poll(&g, 0); }
    CHECK(logged("link down") && g.link && g.speed == 10 && ((M.R[UMAC_CMD / 4] >> 2) & 3) == 0,
          "the link dropped and came back at 10 Mbit/s between two looks: seen, the MAC follows");
    for (int i = 0; i < 20; i++) { seed = seed * 69069 + 1; wire_in(300, seed, 1, 0); if (i % 2) netd_send(300, seed); turn(); }
    for (int i = 0; i < 5; i++) turn();
    CHECK(ngot == nexp && !wrong && check_out(from) && !M.speed_drops && !M.garbled, "frames both ways at 10 Mbit/s");
    from += M.nout;
    M.nout = 0;

    /* 7. a stuck send DMA: after a second the driver restarts the chip, the frames' tags come back as errors */
    M.stuck_tx = 1;
    unsigned d0 = donecount;
    for (int i = 0; i < 3; i++) netd_send(500, 99 + i);
    int r = 0;
    for (int i = 0; i < 150 && r >= 0; i++) { gn_sleep(1); r = gn_tx_done(&g, &sink); }
    CHECK(r == -ETIMEDOUT && logged("the send DMA is stuck: 3 frames have waited 1 s"), "the send DMA stuck: noticed after 1 s");
    M.stuck_tx = 0;
    gn_restart(&g, &sink);
    CHECK(donecount == d0 + 3 && g.tx_err == 3 && logged("the chip reset and started again"), "the chip restarted, the 3 tags back as errors");
    nsent -= 3;
    g.tx_err = 0;
    M.nout = 0;
    for (int i = 0; i < 30; i++) { seed = seed * 69069 + 1; wire_in(600, seed, 1, 0); if (i % 3 == 0) netd_send(400, seed); turn(); }
    for (int i = 0; i < 5; i++) turn();
    CHECK(ngot == nexp && !wrong && check_out(from) && !M.faults, "and frames flow both ways after the restart");
    from += M.nout;
    M.nout = 0;

    /* 8. an index that cannot be right (the chip's receive producer far past the ring): restart */
    M.rprod += 100;
    r = gn_rx(&g, &sink);
    CHECK(r == -EIO && logged("the receive DMA's index is wrong"), "a receive index past the ring: noticed");
    gn_restart(&g, &sink);
    for (int i = 0; i < 30; i++) { seed = seed * 69069 + 1; wire_in(800, seed, 1, 0); turn(); }
    CHECK(ngot == nexp && !wrong && !M.faults, "after the restart the frames arrive again");

    /* 9. the quiesce program, run as the kernel runs it when the driver dies, in the middle of traffic */
    struct udrvquiesce qz;
    gn_quiesce(&g, &qz);
    M.stuck_tx = 1;
    netd_send(300, 1234);
    for (int i = 0; i < 10; i++) { seed = seed * 69069 + 1; wire_in(700, seed, 1, 0); }
    int ok = qz.n <= DRV_QSTEPS && m_quiesce(g.base, &qz) == 0;
    M.stuck_tx = 0;                     /* the send DMA could move now: it must not */
    int w0 = M.dma_writes;
    uint16_t p0 = M.rprod, c0 = M.tcons;
    for (int i = 0; i < 20; i++) { seed = seed * 69069 + 1; wire_in(700, seed, 1, 0); gn_sleep(1); }
    CHECK(ok && qz.n == 6 && M.dma_writes == w0 && M.rprod == p0 && M.tcons == c0 && !(M.R[RDMA_CTRL / 4] & DMA_EN) &&
          !(M.R[TDMA_CTRL / 4] & DMA_EN) && !(M.R[UMAC_CMD / 4] & 3) && !(M.R[EXT_RGMII_OOB_CTRL / 4] & RGMII_LINK) &&
          M.R[(INTRL2_0 + INTRL2_CPU_MASK_SET) / 4] == 0xffffffff && !M.faults,
          "the quiesce program (%u steps, the kernel's checks): no DMA after it, nothing sent or received, the MAC off", qz.n);

    /* 10. the self-test: right with working cache calls, wrong frames with none (it catches them), nothing without a
     * loopback; the chip reset after it */
    m_init();
    bring_up(0xff);
    CHECK(g.phy == 1, "no address in the device tree: the PHY found by looking (address 1)");
    r = gn_selftest(&g, M_BASE, (GN_NRX + 16) * GN_BUF, 4242);
    CHECK(r == 0 && !M.faults && !(M.R[UMAC_CMD / 4] & CMD_LCL_LOOP_EN) && !(M.R[RDMA_CTRL / 4] & DMA_EN),
          "self-test with the cache calls: 4 frames back through the loopback, right; afterwards the chip reset");
    broken_cache = 1;
    M.expect_cache_faults = 1;
    r = gn_selftest(&g, M_BASE, (GN_NRX + 16) * GN_BUF, 777);
    CHECK(r == -EIO && logged("came back wrong"), "self-test with cache calls that do nothing: the frames come back wrong, and it says so");
    broken_cache = 0;
    M.expect_cache_faults = 0;
    m_sync_all();
    M.no_loopback = 1;
    r = gn_selftest(&g, M_BASE, (GN_NRX + 16) * GN_BUF, 99);
    CHECK(r == -ETIMEDOUT, "a chip whose loopback gives nothing: \"nothing came back\" (no verdict)");
    M.no_loopback = 0;
    /* after the self-test, the rings again with netd's buffers, and traffic */
    M.faults = 0;
    g.nbuf = GN_NRX;
    for (int i = 0; i < GN_NRX; i++) gn_give(&g, i);
    gn_rings(&g);
    wait_link(1);
    nexp = ngot = 0;
    for (int i = 0; i < 100; i++) { seed = seed * 69069 + 1; wire_in(64 + seed % 1400, seed, 1, 0); turn(); }
    CHECK(ngot == nexp && !wrong && !M.faults, "after the self-test: 100 frames arrive");

    /* 11. the PHY elsewhere than the device tree says; no PHY at all */
    m_init();
    M.phy_addr = 3;
    nlog = 0;
    bring_up(1);
    CHECK(g.phy == 3 && logged("no PHY answers at MDIO address 1"), "the device tree says 1, the PHY is at 3: said, and found");
    m_init();
    M.phy_addr = -1;
    memset(&g, 0, sizeof g);
    g.ifn = "eth0";
    g.base = 0xF7580000;
    gn_reset(&g);
    CHECK(gn_phy_start(&g, 1) == -ENODEV && logged("no PHY answers on the MDIO bus"), "no PHY: refused, and said");

    /* 12. the model's own strictness: a PHY that is not Broadcom keeps its delays (the driver says so), and the model
     * garbles the frames, as wrong RGMII delays would */
    m_init();
    M.phy_id = 0x0022161a;
    nlog = 0;
    bring_up(1);
    wait_link(1);
    nexp = ngot = wrong = 0;
    for (int i = 0; i < 10; i++) { seed = seed * 69069 + 1; wire_in(200, seed, 1, 0); turn(); }
    CHECK(logged("not a Broadcom PHY") && M.garbled == 10 && wrong, "the model garbles frames when the RGMII delays are wrong (a PHY the driver does not know)");

    /* 13. a previous owner left the DMA running (AmigaOS's genet.device before bootzenix): its rings posted, a frame
     * waiting to go out, nobody taking frames any more. stage0's stop: no descriptor write and no frame sent after it,
     * however much arrives; then Zenix's driver takes the chip over as usual */
    m_init();
    bring_up(1);
    wait_link(1);
    nexp = ngot = wrong = 0;
    for (int i = 0; i < 10; i++) { seed = seed * 69069 + 1; wire_in(300, seed, 1, 0); turn(); }
    M.stuck_tx = 1;                     /* a frame in the send ring that has not gone yet */
    netd_send(300, 4321);
    seed = seed * 69069 + 1;
    wire_in(500, seed, 1, 0);           /* one frame lands before the stop (the owner is gone: never taken) */
    M.nout = 0;
    int w1 = M.dma_writes;
    uint16_t p1 = M.rprod, c1 = M.tcons;
    nlog = 0;
    u32 st = genet_stop(g.base);
    int writes_at_stop = M.dma_writes;
    M.stuck_tx = 0;                     /* the send DMA could move now: it must not */
    for (int i = 0; i < 60; i++) { seed = seed * 69069 + 1; wire_in(64 + seed % 1400, seed, 2, 0); m_step(); }
    CHECK(st == (GSTOP_DONE | GSTOP_RX | GSTOP_TX | GSTOP_MAC) && w1 > 0 && writes_at_stop == w1 && M.dma_writes == w1 &&
          M.rprod == p1 && M.tcons == c1 && !M.nout && !(M.R[RDMA_CTRL / 4] & DMA_EN) && !(M.R[TDMA_CTRL / 4] & DMA_EN) &&
          !(M.R[UMAC_CMD / 4] & (CMD_RX_EN | CMD_TX_EN)) && M.R[(INTRL2_0 + INTRL2_CPU_MASK_SET) / 4] == 0xffffffff &&
          M.R[(INTRL2_1 + INTRL2_CPU_MASK_SET) / 4] == 0xffffffff && !M.faults,
          "a previous owner left the DMA running: after stage0's stop (%x) no descriptor write and no frame sent, 60 frames on the wire", st);
    M.nin = 0;                          /* what came while the MAC's receive was off is gone (the model keeps it queued) */
    nlog = 0;
    bring_up(1);                        /* Zenix's driver later: the chip as it finds it after the stop */
    wait_link(1);
    nexp = ngot = wrong = 0;
    for (int i = 0; i < 30; i++) { seed = seed * 69069 + 1; wire_in(600, seed, 1, 0); if (i % 3 == 0) netd_send(400, seed); turn(); }
    for (int i = 0; i < 5; i++) turn();
    CHECK(ngot == nexp && ngot == 30 && !wrong && !M.faults, "and Zenix's driver starts on that chip: frames flow again");
    /* a chip already stopped (a cold start): only "done"; a DMA that never says stopped: 100 ms, then "slow" */
    CHECK(genet_stop(g.base) == (GSTOP_DONE | GSTOP_RX | GSTOP_TX | GSTOP_MAC) && genet_stop(g.base) == GSTOP_DONE,
          "stopped twice: the second time there was nothing running");
    m_init();
    bring_up(1);
    M.dma_hang = 1;
    u32 us0 = M.us;
    st = genet_stop(g.base);
    CHECK(st == (GSTOP_DONE | GSTOP_RX | GSTOP_TX | GSTOP_SLOW) && M.us - us0 >= 99000 && M.us - us0 < 120000,
          "a DMA that never says stopped: the wait ends after 100 ms of the beam's clock (%u us), said", (unsigned)(M.us - us0));
    M.dma_hang = 0;
    /* not a GENET v5 at that address: nothing written */
    m_init();
    bring_up(1);
    wait_link(1);
    M.R[SYS_REV_CTRL / 4] = 0x05020000;
    st = genet_stop(g.base);
    CHECK(st == GSTOP_NOTV5 && (M.R[RDMA_CTRL / 4] & DMA_EN) && (M.R[UMAC_CMD / 4] & CMD_RX_EN), "not a GENET v5 there: nothing written");

    printf("%s: %d failed\n", bad ? "genet-test FAILED" : "genet-test passed", bad);
    return bad != 0;
}
