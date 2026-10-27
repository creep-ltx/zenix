/* SPDX-License-Identifier: GPL-2.0-only */
/* A model of the CM4's Ethernet for testing net/drv/genet-core.c on the PC (DESIGN D19 8.1 "GENET's register and ring
 * model", step 9): GENET v5's registers, its two DMA engines on ring 16 with the descriptors in the register window,
 * the UniMAC (its command register, the receive filter, the local loopback, the frame limit), the receive buffer's
 * status-block and EEE switches, the MDIO bus with a BCM54213PE behind it (clause 22 registers, the AUX_CTL and SHD
 * shadows, the expansion registers, a cable and a link partner), and the ARM's data cache between the CPU and the
 * DMA (the CPU's view, RAM, dirty lines).
 *
 * Strict, as tools/emmc-fake.h: what the real chip would do wrong with, or what would corrupt memory, is counted as a
 * fault: a ring set up while its engine runs, an engine started on a ring with no size, DMA outside the memory or into
 * a buffer that is not 64-byte aligned, a consumer index that gives the chip more than the ring, the DMA reading a
 * line the CPU wrote and never cleaned, a dirty line written back over what the DMA wrote. Frames are garbled when the
 * RGMII clock delays are not "rxid" (the PHY delays the receive clock, the MAC the send clock), dropped while EEE is
 * on or the MAC's speed differs from the link's. It shows the driver's order and ring arithmetic are consistent with
 * the chip as Linux and U-Boot drive it; it cannot show the real chip behaves the same (only the A1200 can). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_BASE  0x10000000u             /* the model's DMA memory, as the driver sees it */
#define M_SIZE  (256 * 1024)
#define M_LINE  64

static struct {
    uint32_t R[GN_WINDOW / 4];
    /* the DMA engines */
    uint16_t rprod, rcons, rdisc;       /* receive: the chip's producer index, the driver's consumer index */
    unsigned rslot;                     /* the descriptor the chip fills next */
    uint16_t tprod, tcons;
    unsigned tslot;
    int stuck_tx;                       /* the send DMA never moves */
    int no_loopback;                    /* the local loopback does nothing */
    int dma_hang;                       /* the DMA engines never say "stopped" */
    int faults, dma_writes;
    int expect_cache_faults;            /* a test that breaks the cache calls on purpose */
    /* the memory: what the CPU sees (cache and RAM together), RAM, the CPU's view at its last cache call, and the
     * lines the DMA wrote since */
    unsigned char cpu[M_SIZE], ram[M_SIZE], snap[M_SIZE];
    unsigned char dmaw[M_SIZE / M_LINE];
    /* the wire: frames to the Amiga, frames from it */
    struct mframe { unsigned len, flags; unsigned char d[1600]; } in[256], out[512];
    unsigned nin, nout, filtered, eee_drops, speed_drops, garbled, lost_nolink;
    /* MDIO and the PHY */
    int mdio_busy;
    int phy_addr;                       /* -1: no PHY */
    uint32_t phy_id;
    uint16_t bmcr, adv, ctrl1000, misc, shd[32], expsel, mii_buf_cntl0;
    int auxsel, shdsel, reset_reads;
    int cable, partner;                 /* partner: its abilities, ADV_* bits | ADV_1000FULL << 1 for 1000 full */
    int link, latched_down, speed, full;
    uint32_t aneg_at;                   /* the microsecond the negotiation ends */
    uint32_t us;                        /* the clock */
} M;

static int m_trace;
#define FAULT(...) do { M.faults++; printf("MODEL FAULT: " __VA_ARGS__); printf("\n"); } while (0)

static uint32_t m_ticks(void) { M.us += 20; return M.us / 10000; }

static void m_phy_defaults(void)
{
    M.bmcr = BMCR_ANENABLE;
    M.adv = ADV_CSMA | ADV_10HALF | ADV_10FULL | ADV_100HALF | ADV_100FULL | 0x0c00;   /* pause bits too */
    M.ctrl1000 = ADV_1000FULL | 0x0100;
    M.misc = 0;                         /* the receive-clock skew off: the driver must set it */
    memset(M.shd, 0, sizeof M.shd);
    M.shd[SHD_CLK_CTL] = SHD_CLK_GTXCLK_EN;     /* the send-clock delay on: the driver must clear it */
    M.mii_buf_cntl0 = AUTOGREEEN_EN;
    M.link = 0;
    M.aneg_at = 0;
}

/* a fresh model: a chip that something else used before (status blocks, EEE, interrupts unmasked, the indexes far
 * from zero and near their wrap) */
static void m_init(void)
{
    memset(&M, 0, sizeof M);
    for (unsigned i = 0; i < M_SIZE; i++) M.ram[i] = (unsigned char)(i * 7 + (i >> 8) * 13 + 1);
    memcpy(M.cpu, M.ram, M_SIZE);
    memcpy(M.snap, M.ram, M_SIZE);
    M.R[SYS_REV_CTRL / 4] = 0x06010000;         /* v5.1 */
    M.R[RBUF_CTRL / 4] = RBUF_64B_EN | RBUF_ALIGN_2B;
    M.R[TBUF_CTRL / 4] = 1;
    M.R[RBUF_ENERGY_CTRL / 4] = ENERGY_EEE_EN;
    M.R[EXT_RGMII_OOB_CTRL / 4] = OOB_DISABLE | ID_MODE_DIS;
    M.rprod = 0xfff0;
    M.tcons = 0xfff8;
    M.tprod = 0xfff8;
    M.phy_addr = 1;
    M.phy_id = BCM54213PE_ID;
    M.cable = 1;
    M.partner = ADV_10HALF | ADV_10FULL | ADV_100HALF | ADV_100FULL | ADV_1000FULL << 1;
    m_phy_defaults();
}

/* ---- the cache ---- */
static int m_addr_ok(uint32_t a, uint32_t n) { return a >= M_BASE && n <= M_SIZE && a - M_BASE <= M_SIZE - n; }
static int m_dirty(unsigned l) { return memcmp(M.cpu + l * M_LINE, M.snap + l * M_LINE, M_LINE) != 0; }
static void m_cache(unsigned how, uint32_t a, uint32_t n)
{
    if (!n) return;
    uint32_t lo = a & ~63u, hi = (a + n + 63) & ~63u;
    if (!m_addr_ok(lo, hi - lo)) { FAULT("a cache call outside the memory: %x %u", a, n); return; }
    for (uint32_t p = lo; p < hi; p += M_LINE) {
        unsigned l = (p - M_BASE) / M_LINE, o = l * M_LINE;
        if (m_dirty(l)) {
            if (M.dmaw[l] && !M.expect_cache_faults) FAULT("a dirty line at %x written back over what the DMA wrote", p);
            memcpy(M.ram + o, M.cpu + o, M_LINE);
        }
        if (how == DRVC_INVAL) memcpy(M.cpu + o, M.ram + o, M_LINE);
        memcpy(M.snap + o, M.cpu + o, M_LINE);
        M.dmaw[l] = 0;
    }
}
/* after a test that broke the cache on purpose: RAM and the CPU agree again */
static void m_sync_all(void)
{
    memcpy(M.ram, M.cpu, M_SIZE);
    memcpy(M.snap, M.cpu, M_SIZE);
    memset(M.dmaw, 0, sizeof M.dmaw);
}
static int m_dma_read(uint32_t a, unsigned n, unsigned char *to)
{
    if (!m_addr_ok(a, n)) { FAULT("the send DMA reads outside the memory: %x %u", a, n); return -1; }
    for (uint32_t p = a & ~63u; p < a + n; p += M_LINE)
        if (m_dirty((p - M_BASE) / M_LINE) && !M.expect_cache_faults) FAULT("the send DMA reads a line the CPU never cleaned: %x", p);
    memcpy(to, M.ram + (a - M_BASE), n);
    return 0;
}
static int m_dma_write(uint32_t a, const unsigned char *d, unsigned n)
{
    if (!m_addr_ok(a, n)) { FAULT("the receive DMA writes outside the memory: %x %u", a, n); return -1; }
    memcpy(M.ram + (a - M_BASE), d, n);
    for (uint32_t p = a & ~63u; p < a + n; p += M_LINE) M.dmaw[(p - M_BASE) / M_LINE] = 1;
    M.dma_writes++;
    return 0;
}

/* ---- the PHY ---- */
static void m_phy_time(void)
{
    if (M.aneg_at && M.us >= M.aneg_at) {
        M.aneg_at = 0;
        int ours = M.adv & 0x1e0, both = ours & M.partner;
        M.link = M.cable && (both || ((M.ctrl1000 & ADV_1000FULL) && (M.partner & ADV_1000FULL << 1)));
        if (M.link) {
            if ((M.ctrl1000 & ADV_1000FULL) && (M.partner & ADV_1000FULL << 1)) M.speed = 1000, M.full = 1;
            else if (both & ADV_100FULL) M.speed = 100, M.full = 1;
            else if (both & ADV_100HALF) M.speed = 100, M.full = 0;
            else if (both & ADV_10FULL) M.speed = 10, M.full = 1;
            else M.speed = 10, M.full = 0;
        }
    }
}
static void m_cable(int plugged)
{
    M.cable = plugged;
    if (!plugged) {
        if (M.link) M.latched_down = 1;
        M.link = 0;
    } else if (M.bmcr & BMCR_ANENABLE)
        M.aneg_at = M.us + 1500000;     /* 1.5 s to negotiate */
}
static int m_phy_rd(unsigned reg)
{
    m_phy_time();
    switch (reg) {
    case MII_BMCR:
        if (M.reset_reads > 0 && --M.reset_reads == 0) M.bmcr &= (uint16_t)~BMCR_RESET;
        return M.bmcr;
    case MII_BMSR: {
        int l = M.link && !M.latched_down;
        M.latched_down = 0;
        return 0x7949 | (l ? BMSR_LSTATUS : 0) | (M.link ? BMSR_ANEGCOMPLETE : 0);
    }
    case MII_PHYSID1: return (int)(M.phy_id >> 16);
    case MII_PHYSID2: return (int)(M.phy_id & 0xffff);
    case MII_ADVERTISE: return M.adv;
    case MII_LPA: return M.link ? (M.partner & 0x1e0) | ADV_CSMA | 0x4000 : 0;
    case MII_CTRL1000: return M.ctrl1000;
    case MII_STAT1000: return M.link && (M.partner & ADV_1000FULL << 1) ? LPA_1000FULL : 0;
    case BCM_AUX_CTL: return M.auxsel == AUXCTL_SHDWSEL_MISC ? (M.misc | AUXCTL_SHDWSEL_MISC) : 0;
    case BCM_SHD: return M.shdsel << 10 | M.shd[M.shdsel];
    case BCM_EXP_DATA: return M.expsel == BCM_EXP_MII_BUF_CNTL0 ? M.mii_buf_cntl0 : 0;
    }
    return 0;
}
static void m_phy_wr(unsigned reg, unsigned v)
{
    m_phy_time();
    switch (reg) {
    case MII_BMCR:
        if (v & BMCR_RESET) {
            m_phy_defaults();
            M.bmcr = BMCR_RESET | BMCR_ANENABLE;
            M.reset_reads = 3;
            if (M.link) M.latched_down = 1;
            return;
        }
        M.bmcr = (uint16_t)(v & ~BMCR_ANRESTART);
        if ((v & BMCR_ANENABLE) && (v & BMCR_ANRESTART)) {
            if (M.link) M.latched_down = 1;
            M.link = 0;
            if (M.cable) M.aneg_at = M.us + 1500000;
        }
        return;
    case MII_ADVERTISE: M.adv = (uint16_t)v; return;
    case MII_CTRL1000: M.ctrl1000 = (uint16_t)v; return;
    case BCM_AUX_CTL:
        if ((v & 7) == AUXCTL_SHDWSEL_MISC && (v & AUXCTL_MISC_WREN)) M.misc = (uint16_t)(v & ~(AUXCTL_MISC_WREN | 7));
        else if ((v & 7) == 7) M.auxsel = (v >> AUXCTL_READ_SHIFT) & 7;
        return;
    case BCM_SHD:
        if (v & SHD_WRITE) M.shd[(v >> 10) & 31] = (uint16_t)(v & 0x3ff);
        else M.shdsel = (v >> 10) & 31;
        return;
    case BCM_EXP_SEL: M.expsel = (uint16_t)v; return;
    case BCM_EXP_DATA: if (M.expsel == BCM_EXP_MII_BUF_CNTL0) M.mii_buf_cntl0 = (uint16_t)v; return;
    }
}

/* ---- the engines ---- */
static uint32_t *m_reg(uint32_t off) { return &M.R[off / 4]; }
static int m_rx_on(void) { return (*m_reg(RDMA_CTRL) & DMA_EN) && (*m_reg(RDMA_CTRL) & DMA_RING16_EN) && (*m_reg(RDMA_RING_CFG) & 1u << 16); }
static int m_tx_on(void) { return (*m_reg(TDMA_CTRL) & DMA_EN) && (*m_reg(TDMA_CTRL) & DMA_RING16_EN) && (*m_reg(TDMA_RING_CFG) & 1u << 16); }
static unsigned m_ring_n(uint32_t bufsize_reg) { return *m_reg(bufsize_reg) >> 16; }
static int m_rgmii_ok(void)
{
    return *m_reg(SYS_PORT_CTRL) == PORT_MODE_EXT_GPHY && (*m_reg(EXT_RGMII_OOB_CTRL) & RGMII_MODE_EN) &&
           !(*m_reg(EXT_RGMII_OOB_CTRL) & (ID_MODE_DIS | OOB_DISABLE)) && (M.misc & AUXCTL_MISC_RGMII_SKEW) &&
           !(M.shd[SHD_CLK_CTL] & SHD_CLK_GTXCLK_EN);
}
static int m_mac_speed(void) { int s = (*m_reg(UMAC_CMD) >> CMD_SPEED_SHIFT) & 3; return s == 2 ? 1000 : s == 1 ? 100 : 10; }
static int m_accept(const unsigned char *d)
{
    uint32_t cmd = *m_reg(UMAC_CMD), ctl = *m_reg(UMAC_MDF_CTRL);
    if (cmd & CMD_PROMISC) return 1;
    for (unsigned k = 0; k < 17; k++) {
        if (!(ctl & 1u << (16 - k))) continue;
        uint32_t w0 = *m_reg(UMAC_MDF_ADDR + k * 8), w1 = *m_reg(UMAC_MDF_ADDR + k * 8 + 4);
        unsigned char a[6] = { (unsigned char)(w0 >> 8), (unsigned char)w0, (unsigned char)(w1 >> 24), (unsigned char)(w1 >> 16),
                               (unsigned char)(w1 >> 8), (unsigned char)w1 };
        if (!memcmp(a, d, 6)) return 1;
    }
    if (!ctl) {                         /* no filter set: the station address only (and broadcast) */
        uint32_t m0 = *m_reg(UMAC_MAC0), m1 = *m_reg(UMAC_MAC1);
        unsigned char a[6] = { (unsigned char)(m0 >> 24), (unsigned char)(m0 >> 16), (unsigned char)(m0 >> 8), (unsigned char)m0,
                               (unsigned char)(m1 >> 8), (unsigned char)m1 };
        static const unsigned char b[6] = { 255, 255, 255, 255, 255, 255 };
        return !memcmp(a, d, 6) || !memcmp(b, d, 6);
    }
    return 0;
}
/* one frame into the receive ring (from the wire or the loopback): 1 taken (or dropped), 0 no room yet */
static int m_rx_frame(struct mframe *f, int loop)
{
    uint32_t cmd = *m_reg(UMAC_CMD);
    unsigned n = m_ring_n(RDMA_RING_BUF_SIZE), bufsz = *m_reg(RDMA_RING_BUF_SIZE) & 0xffff;
    if (!(cmd & CMD_RX_EN) || (cmd & CMD_SW_RESET) || !m_rx_on()) return 0;
    if (!loop) {
        if (!M.link || !(*m_reg(EXT_RGMII_OOB_CTRL) & RGMII_LINK)) { M.lost_nolink++; return 1; }
        if (m_mac_speed() != M.speed) { M.speed_drops++; return 1; }
    }
    if (*m_reg(RBUF_ENERGY_CTRL) & ENERGY_EEE_EN) { M.eee_drops++; return 1; }
    if (f->len > *m_reg(UMAC_MAX_FRAME_LEN)) return 1;
    if (!m_accept(f->d)) { M.filtered++; return 1; }
    if ((uint16_t)(M.rprod - M.rcons) >= n) {   /* no descriptor: the chip counts it and drops it */
        M.rdisc++;
        return 1;
    }
    unsigned char tmp[1700];
    unsigned off = (*m_reg(RBUF_CTRL) & RBUF_64B_EN ? 64 : 0) + (*m_reg(RBUF_CTRL) & RBUF_ALIGN_2B ? 2 : 0), len = f->len + off;
    memset(tmp, 0, off);
    memcpy(tmp + off, f->d, f->len);
    if (!loop && !m_rgmii_ok() && f->len > 20) { tmp[off + 20] ^= 0x40; M.garbled++; }  /* a bit misread on the wire */
    uint32_t d = GN_RXD + M.rslot * GN_DESC;
    uint32_t a = *m_reg(d + DESC_ADDR_LO);
    if (*m_reg(d + DESC_ADDR_HI)) FAULT("a receive descriptor with a high address word");
    if (a & 63) FAULT("receive buffer %x is not 64-byte aligned", a);
    uint32_t st = DS_SOP | DS_EOP | (f->flags & DS_RX_ERRORS);
    if (len > bufsz) { st |= 0x10; len = bufsz; }   /* too long for the buffer */
    m_dma_write(a, tmp, len);
    *m_reg(d + DESC_LEN_STAT) = (uint32_t)len << 16 | st;
    M.rprod++;
    M.rslot = (M.rslot + 1) % n;
    return 1;
}
static void m_step(void)
{
    m_phy_time();
    /* send */
    uint32_t cmd = *m_reg(UMAC_CMD);
    unsigned tn = m_ring_n(TDMA_RING_BUF_SIZE);
    int loop = (cmd & CMD_LCL_LOOP_EN) != 0;
    while (M.tprod != M.tcons && m_tx_on() && (cmd & CMD_TX_EN) && !(cmd & CMD_SW_RESET) && !M.stuck_tx && tn) {
        if (!loop && (!M.link || !(*m_reg(EXT_RGMII_OOB_CTRL) & RGMII_LINK))) break;    /* no link: the frames wait */
        uint32_t d = GN_TXD + M.tslot * GN_DESC, ls = *m_reg(d + DESC_LEN_STAT), a = *m_reg(d + DESC_ADDR_LO);
        unsigned len = (ls >> 16) & 0xfff;
        struct mframe f = { 0 };
        if ((ls & (DS_SOP | DS_EOP)) != (DS_SOP | DS_EOP)) FAULT("a send descriptor without SOP and EOP (%x)", ls);
        if (!(ls & DS_TX_APPEND_CRC)) FAULT("a send descriptor without APPEND_CRC");
        if (len < 14 || len > 1536) FAULT("a send descriptor of %u bytes", len);
        else if (len < 60) FAULT("a frame of %u bytes sent unpadded", len);
        if (len <= 1536 && m_dma_read(a, len, f.d) == 0) {
            f.len = len;
            if (loop) {
                if (!M.no_loopback && M.nin < 256) {
                    memmove(M.in + 1, M.in, M.nin * sizeof M.in[0]);    /* ahead of the wire's frames */
                    M.in[0] = f;
                    M.nin++;
                }
            } else if (M.nout < 512) {
                if (!m_rgmii_ok() && f.len > 20) { f.d[20] ^= 0x40; M.garbled++; }
                if (m_mac_speed() != M.speed) M.speed_drops++;
                else M.out[M.nout++] = f;
            }
        }
        M.tcons++;
        M.tslot = (M.tslot + 1) % tn;
    }
    /* receive */
    while (M.nin && m_rx_frame(&M.in[0], loop)) {
        M.nin--;
        memmove(M.in, M.in + 1, M.nin * sizeof M.in[0]);
    }
}
/* a frame on the wire to the Amiga */
static void m_wire(const unsigned char *d, unsigned len, unsigned flags)
{
    if (M.nin >= 256) return;
    M.in[M.nin].len = len;
    M.in[M.nin].flags = flags;
    memcpy(M.in[M.nin].d, d, len);
    M.nin++;
    m_step();
}

/* ---- the registers ---- */
static uint32_t m_rd(uint32_t off)
{
    if (off >= GN_WINDOW || (off & 3)) { FAULT("a read outside the window: %x", off); return 0; }
    m_step();
    switch (off) {
    case RDMA_STATUS: return *m_reg(RDMA_CTRL) & DMA_EN || M.dma_hang ? 0 : DMA_DISABLED;
    case TDMA_STATUS: return *m_reg(TDMA_CTRL) & DMA_EN ? 0 : DMA_DISABLED;
    case RDMA_PROD_INDEX: return (uint32_t)M.rdisc << 16 | M.rprod;
    case RDMA_CONS_INDEX: return M.rcons;
    case TDMA_CONS_INDEX: return M.tcons;
    case TDMA_PROD_INDEX: return M.tprod;
    case UMAC_MDIO_CMD:
        if (M.mdio_busy && --M.mdio_busy == 0) {
            uint32_t c = M.R[off / 4];
            int a = (c >> MDIO_PMD_SHIFT) & 31, r = (c >> MDIO_REG_SHIFT) & 31;
            c &= ~MDIO_START_BUSY;
            if (a != M.phy_addr) c = (c & ~0xffffu) | MDIO_READ_FAIL | 0xffff;
            else if ((c & (3u << 26)) == MDIO_RD) c = (c & ~0xffffu & ~MDIO_READ_FAIL) | (uint32_t)m_phy_rd((unsigned)r);
            else m_phy_wr((unsigned)r, c & 0xffff);
            M.R[off / 4] = c;
        }
        return M.R[off / 4];
    }
    return M.R[off / 4];
}
static void m_wr(uint32_t off, uint32_t v)
{
    if (off >= GN_WINDOW || (off & 3)) { FAULT("a write outside the window: %x", off); return; }
    m_step();
    int rx_on = m_rx_on(), tx_on = m_tx_on();
    switch (off) {
    case RDMA_RING_BUF_SIZE: case RDMA_START_ADDR: case RDMA_END_ADDR: case RDMA_WRITE_PTR: case RDMA_READ_PTR: case RDMA_RING_CFG:
        if (rx_on) FAULT("the receive ring set up while its DMA runs (%x)", off);
        if (off == RDMA_WRITE_PTR) M.rslot = v / 3;
        break;
    case TDMA_RING_BUF_SIZE: case TDMA_START_ADDR: case TDMA_END_ADDR: case TDMA_WRITE_PTR: case TDMA_READ_PTR: case TDMA_RING_CFG:
        if (tx_on) FAULT("the send ring set up while its DMA runs (%x)", off);
        if (off == TDMA_READ_PTR) M.tslot = v / 3;
        break;
    case RDMA_CTRL: case TDMA_CTRL:
        if (v & DMA_EN) {
            uint32_t bs = off == RDMA_CTRL ? RDMA_RING_BUF_SIZE : TDMA_RING_BUF_SIZE;
            uint32_t ea = off == RDMA_CTRL ? RDMA_END_ADDR : TDMA_END_ADDR;
            unsigned n = m_ring_n(bs);
            if (!n || (*m_reg(ea) + 1) * 4 != n * GN_DESC || (*m_reg(bs) & 0xffff) < 1536)
                FAULT("a DMA engine started on a ring that is not set up (%x: size %x, end %x)", off, *m_reg(bs), *m_reg(ea));
        }
        break;
    case RDMA_CONS_INDEX:
        M.rcons = (uint16_t)v;
        if ((uint16_t)(M.rprod - M.rcons) > m_ring_n(RDMA_RING_BUF_SIZE))
            FAULT("the receive consumer index %x gives the chip more than the ring (producer %x)", v, M.rprod);
        break;
    case TDMA_PROD_INDEX:
        M.tprod = (uint16_t)v;
        if ((uint16_t)(M.tprod - M.tcons) > m_ring_n(TDMA_RING_BUF_SIZE))
            FAULT("the send producer index %x is more than a ring ahead (consumer %x)", v, M.tcons);
        break;
    case RDMA_PROD_INDEX: case TDMA_CONS_INDEX: case SYS_REV_CTRL: case RDMA_STATUS: case TDMA_STATUS:
        return;                         /* the chip's own */
    case UMAC_MDIO_CMD:
        if ((v & MDIO_START_BUSY) && !(M.R[off / 4] & MDIO_START_BUSY)) M.mdio_busy = 3;
        else if (M.mdio_busy) FAULT("MDIO_CMD written while a transfer runs");
        break;
    case INTRL2_0 + INTRL2_CPU_CLEAR: case INTRL2_1 + INTRL2_CPU_CLEAR:
        return;
    case UMAC_CMD:
        if (v & CMD_SW_RESET) v &= CMD_SW_RESET | CMD_LCL_LOOP_EN;
        break;
    }
    M.R[off / 4] = v;
    m_step();
}

/* the kernel running a quiesce program (DESIGN D19 2.4) as on Emu68: a 68k store of the step's value, so the register
 * gets its bytes swapped; every address checked as kernel/drv.c does */
static int m_quiesce(uint32_t base, const struct udrvquiesce *q)
{
    if (q->n > DRV_QSTEPS) { FAULT("a quiesce program of %u steps", q->n); return -1; }
    for (unsigned k = 0; k < q->n; k++) {
        const struct udrvstep *s = &q->step[k];
        if ((s->op != DRVS_WRITE && s->op != DRVS_RMW) || s->size != 4 || (s->addr & 3) || s->addr < base ||
            s->addr + 4 > base + GN_WINDOW) { FAULT("quiesce step %u is not one the kernel takes", k); return -1; }
        uint32_t off = s->addr - base, cur = __builtin_bswap32(m_rd(off));
        uint32_t nv = s->op == DRVS_RMW ? (cur & ~s->mask) | s->value : s->value;
        m_wr(off, __builtin_bswap32(nv));
    }
    return 0;
}
