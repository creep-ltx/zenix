/* SPDX-License-Identifier: GPL-2.0-only */
/* GENET v5: the chip and its PHY (DESIGN D19 5.2, step 9). Plain C with no system call of its own: the file that
 * includes it gives the register access, the clock, the cache calls and the log, so the same code runs against the
 * hardware under Zenix (genet.c) and against the model on the PC (tools/genet-test.c, tools/genet-model.h).
 *
 * The includer defines, before including this file:
 *   u32 gn_rd(struct genet *, u32 off), void gn_wr(struct genet *, u32 off, u32 v)    a register (its value, not its
 *                                                      bytes: the little-endian swap is the includer's)
 *   u32 gn_ticks(void), void gn_sleep(u32 ticks)       the 100 Hz clock
 *   void gn_cache(struct genet *, unsigned how, u32 addr, u32 len)   DRVC_CLEAN or DRVC_INVAL over whole lines
 *   unsigned char *gn_mem(u32 addr)                    a DMA address as the CPU reaches it
 *   void gn_log(struct genet *, const char *fmt, ...)  a line for the kernel log
 *
 * Bring-up, in Linux's and U-Boot's order (genet.h names the sources): every interrupt masked (polled; on Emu68 the
 * chip's own mask is the place, never the GIC), both DMA engines stopped, the RGMII mode set (the PHY delays the
 * receive clock, the MAC the send clock: "rgmii-rxid"), the receive buffer flushed, the UniMAC reset, its counters
 * cleared, the frame limit 1536, no status blocks and no EEE (both survive a reset, so whatever ran before is undone),
 * the MAC address and the receive filter (broadcast and our own), then the rings, then the PHY. One ring each way,
 * ring 16 (the default queue): GN_NRX receive and GN_NTX send descriptors. Every wait has a time limit and says what
 * it waited for. */

#define W16(x) ((unsigned short)(x))

static void gn_set(struct genet *g, u32 off, u32 clr, u32 set) { gn_wr(g, off, (gn_rd(g, off) & ~clr) | set); }

/* ---- MDIO: the PHY's registers through the UniMAC (a clause 22 transfer takes about 25 us) ---- */
static int mdio_wait(struct genet *g)
{
    u32 t0 = gn_ticks();
    while (gn_rd(g, UMAC_MDIO_CMD) & MDIO_START_BUSY)
        if (gn_ticks() - t0 > 2) {
            gn_log(g, "the MDIO transfer did not finish in 20 ms");
            return -ETIMEDOUT;
        }
    return 0;
}
static int mdio_read(struct genet *g, int phy, unsigned reg)
{
    gn_wr(g, UMAC_MDIO_CMD, MDIO_RD | (u32)(phy & 31) << MDIO_PMD_SHIFT | (u32)(reg & 31) << MDIO_REG_SHIFT);
    gn_set(g, UMAC_MDIO_CMD, 0, MDIO_START_BUSY);
    if (mdio_wait(g)) return -ETIMEDOUT;
    u32 v = gn_rd(g, UMAC_MDIO_CMD);
    return v & MDIO_READ_FAIL ? -EIO : (int)(v & 0xffff);   /* nobody drove the bus: no PHY there */
}
static int mdio_write(struct genet *g, int phy, unsigned reg, unsigned v)
{
    gn_wr(g, UMAC_MDIO_CMD, MDIO_WR | (u32)(phy & 31) << MDIO_PMD_SHIFT | (u32)(reg & 31) << MDIO_REG_SHIFT | (v & 0xffff));
    gn_set(g, UMAC_MDIO_CMD, 0, MDIO_START_BUSY);
    return mdio_wait(g);
}

/* ---- the chip ---- */

/* GENET v5 at g->base? 0, or -ENODEV */
int gn_probe(struct genet *g)
{
    u32 rev = gn_rd(g, SYS_REV_CTRL);
    unsigned major = (rev >> 24) & 15;
    if (major != 6) {                   /* 6 is v5 (5 is v4, 0 is v1: Linux's numbering) */
        gn_log(g, "not a GENET v5 at %x (revision register %x)", g->base, rev);
        return -ENODEV;
    }
    gn_log(g, "GENET v5.%u at %x", (unsigned)((rev >> 16) & 15), g->base);
    return 0;
}

static void dma_off(struct genet *g)
{
    gn_wr(g, RDMA_CTRL, 0);             /* the receive engine first: it is the one writing into memory */
    gn_wr(g, TDMA_CTRL, 0);
    u32 t0 = gn_ticks();
    while (!(gn_rd(g, RDMA_STATUS) & DMA_DISABLED) || !(gn_rd(g, TDMA_STATUS) & DMA_DISABLED))
        if (gn_ticks() - t0 > 10) {
            gn_log(g, "the DMA did not stop in 100 ms (status %x %x)", gn_rd(g, RDMA_STATUS), gn_rd(g, TDMA_STATUS));
            break;
        }
}

static void mdf_addr(struct genet *g, unsigned k, const unsigned char *a)
{
    gn_wr(g, UMAC_MDF_ADDR + k * 8, (u32)a[0] << 8 | a[1]);
    gn_wr(g, UMAC_MDF_ADDR + k * 8 + 4, (u32)a[2] << 24 | (u32)a[3] << 16 | (u32)a[4] << 8 | a[5]);
}

/* everything but the rings and the PHY, from whatever state the chip was left in (AmigaOS's driver, a dead
 * driver of ours, a warm reset): 0 */
int gn_reset(struct genet *g)
{
    static const unsigned char bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    for (unsigned k = 0; k < GN_NRX; k++) g->rxs[k] = -1;      /* no buffer in any slot (the caller's pool keeps them) */
    gn_wr(g, INTRL2_0 + INTRL2_CPU_MASK_SET, 0xffffffff);      /* polled: no interrupt reaches the GIC */
    gn_wr(g, INTRL2_0 + INTRL2_CPU_CLEAR, 0xffffffff);
    gn_wr(g, INTRL2_1 + INTRL2_CPU_MASK_SET, 0xffffffff);
    gn_wr(g, INTRL2_1 + INTRL2_CPU_CLEAR, 0xffffffff);
    dma_off(g);
    gn_wr(g, UMAC_CMD, 0);
    gn_wr(g, SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);
    gn_set(g, EXT_RGMII_OOB_CTRL, OOB_DISABLE | ID_MODE_DIS | RGMII_LINK, RGMII_MODE_EN);
    gn_set(g, SYS_RBUF_FLUSH_CTRL, 0, 2);
    gn_sleep(1);
    gn_set(g, SYS_RBUF_FLUSH_CTRL, 2, 0);
    gn_sleep(1);
    gn_wr(g, SYS_RBUF_FLUSH_CTRL, 0);
    gn_wr(g, UMAC_TX_FLUSH, 1);
    gn_sleep(1);
    gn_wr(g, UMAC_TX_FLUSH, 0);
    gn_wr(g, UMAC_CMD, CMD_SW_RESET | CMD_LCL_LOOP_EN);       /* the loopback keeps the receive clock steady */
    gn_sleep(1);
    gn_wr(g, UMAC_CMD, 0);
    gn_wr(g, UMAC_MIB_CTRL, 7);                                 /* the counters: receive, runt, send */
    gn_wr(g, UMAC_MIB_CTRL, 0);
    gn_wr(g, UMAC_MAX_FRAME_LEN, GN_BUF);
    gn_set(g, RBUF_CTRL, RBUF_64B_EN | RBUF_ALIGN_2B, 0);      /* a frame starts at its buffer's first byte */
    gn_wr(g, TBUF_CTRL, 0);
    gn_wr(g, RBUF_CHK_CTRL, 0);
    gn_set(g, RBUF_ENERGY_CTRL, ENERGY_EEE_EN | ENERGY_PM_EN, 0);
    gn_set(g, TBUF_ENERGY_CTRL, ENERGY_EEE_EN | ENERGY_PM_EN, 0);
    gn_wr(g, RBUF_TBUF_SIZE_CTRL, 1);
    gn_wr(g, UMAC_MAC0, (u32)g->mac[0] << 24 | (u32)g->mac[1] << 16 | (u32)g->mac[2] << 8 | g->mac[3]);
    gn_wr(g, UMAC_MAC1, (u32)g->mac[4] << 8 | g->mac[5]);
    mdf_addr(g, 0, bcast);                                      /* the receive filter: broadcast and our own */
    mdf_addr(g, 1, g->mac);
    gn_wr(g, UMAC_MDF_CTRL, 3u << 15);                          /* filters 0 and 1: bits 16 and 15 */
    return 0;
}

/* the rings: every buffer of ours posted (g->buf[0..nbuf-1] are ours when this is called, or those in the pool after a
 * restart), the DMA engines on. 0 */
int gn_rings(struct genet *g)
{
    unsigned k, n = 0;
    dma_off(g);
    /* receive: the chip may fill GN_NRX descriptors past the consumer index; slots without a buffer are kept out of
     * its reach by starting the consumer index that many behind */
    gn_wr(g, RDMA_BURST, 8);
    gn_wr(g, RDMA_START_ADDR, 0);
    gn_wr(g, RDMA_READ_PTR, 0);
    gn_wr(g, RDMA_WRITE_PTR, 0);
    gn_wr(g, RDMA_END_ADDR, GN_NRX * GN_DESC / 4 - 1);
    gn_wr(g, RDMA_RING_BUF_SIZE, (u32)GN_NRX << 16 | GN_BUF);
    gn_wr(g, RDMA_XON_XOFF_THRESH, 5u << 16 | 16);
    for (k = 0; k < GN_NRX; k++) {
        g->rxs[k] = -1;
        if (g->npool) {
            int b = g->pool[--g->npool];
            gn_cache(g, DRVC_INVAL, g->buf[b], GN_BUF);         /* nothing of ours written back over its frame */
            g->rxs[k] = (signed char)b;
            n++;
        }
        gn_wr(g, GN_RXD + k * GN_DESC + DESC_ADDR_LO, g->rxs[k] >= 0 ? g->buf[g->rxs[k]] : 0);
        gn_wr(g, GN_RXD + k * GN_DESC + DESC_ADDR_HI, 0);
        gn_wr(g, GN_RXD + k * GN_DESC + DESC_LEN_STAT, (u32)GN_BUF << 16 | DS_OWN);
    }
    g->rx_base = g->rx_p = W16(gn_rd(g, RDMA_PROD_INDEX));      /* the chip's index cannot be set: follow it */
    g->rx_c = W16(g->rx_base + n - GN_NRX);
    g->rx_disc = W16(gn_rd(g, RDMA_PROD_INDEX) >> 16);
    gn_wr(g, RDMA_CONS_INDEX, g->rx_c);
    gn_wr(g, RDMA_RING_CFG, 1u << 16);
    /* send */
    gn_wr(g, TDMA_BURST, 8);
    gn_wr(g, TDMA_START_ADDR, 0);
    gn_wr(g, TDMA_READ_PTR, 0);
    gn_wr(g, TDMA_WRITE_PTR, 0);
    gn_wr(g, TDMA_END_ADDR, GN_NTX * GN_DESC / 4 - 1);
    g->tx_base = g->tx_p = g->tx_c = W16(gn_rd(g, TDMA_CONS_INDEX));
    gn_wr(g, TDMA_PROD_INDEX, g->tx_p);
    gn_wr(g, TDMA_MBUF_DONE_THRESH, 1);
    gn_wr(g, TDMA_FLOW_PERIOD, 0);
    gn_wr(g, TDMA_RING_BUF_SIZE, (u32)GN_NTX << 16 | GN_BUF);
    gn_wr(g, TDMA_RING_CFG, 1u << 16);
    g->tx_moved = gn_ticks();
    gn_wr(g, TDMA_CTRL, DMA_RING16_EN | DMA_EN);
    gn_wr(g, RDMA_CTRL, DMA_RING16_EN | DMA_EN);
    return 0;
}

/* the chip stopped: no DMA, no frames (as the quiesce program, which the kernel runs if the driver cannot) */
void gn_stop(struct genet *g)
{
    dma_off(g);
    gn_set(g, UMAC_CMD, CMD_TX_EN | CMD_RX_EN, 0);
    gn_set(g, EXT_RGMII_OOB_CTRL, RGMII_LINK, 0);
}

/* the kernel's copy of gn_stop for when the driver or netd ends (DESIGN D19 2.4): the kernel writes the 68k's way, so
 * each value is given in the bytes the register wants (little-endian). 6 steps of the 8 */
static u32 le(u32 v) { return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24; }
void gn_quiesce(const struct genet *g, struct udrvquiesce *q)
{
    static const u32 off[6] = { RDMA_CTRL, TDMA_CTRL, UMAC_CMD, EXT_RGMII_OOB_CTRL, INTRL2_0 + INTRL2_CPU_MASK_SET,
                                INTRL2_1 + INTRL2_CPU_MASK_SET };
    memset(q, 0, sizeof *q);
    for (unsigned k = 0; k < 6; k++) {
        q->step[k].op = DRVS_WRITE;
        q->step[k].size = 4;
        q->step[k].addr = g->base + off[k];
    }
    q->step[2].op = q->step[3].op = DRVS_RMW;                   /* the MAC: send and receive off; the link flag off */
    q->step[2].mask = le(CMD_TX_EN | CMD_RX_EN);
    q->step[3].mask = le(RGMII_LINK);
    q->step[4].value = q->step[5].value = 0xffffffff;
    q->n = 6;
}

/* ---- the PHY ---- */
static int bcm_config(struct genet *g)
{
    /* RGMII "rxid": the PHY delays the receive clock, not the send clock (the MAC does that one: ID_MODE_DIS clear) */
    if (mdio_write(g, g->phy, BCM_AUX_CTL, AUXCTL_SHDWSEL_MISC << AUXCTL_READ_SHIFT | AUXCTL_SHDWSEL_MISC)) return -EIO;
    int v = mdio_read(g, g->phy, BCM_AUX_CTL);
    if (v < 0) return v;
    if (mdio_write(g, g->phy, BCM_AUX_CTL, ((unsigned)v | AUXCTL_MISC_WREN | AUXCTL_MISC_RGMII_SKEW | AUXCTL_SHDWSEL_MISC) & 0xffff))
        return -EIO;
    if (mdio_write(g, g->phy, BCM_SHD, (u32)SHD_CLK_CTL << 10)) return -EIO;
    if ((v = mdio_read(g, g->phy, BCM_SHD)) < 0) return v;
    if (mdio_write(g, g->phy, BCM_SHD, SHD_WRITE | (u32)SHD_CLK_CTL << 10 | ((unsigned)v & 0x3ff & ~SHD_CLK_GTXCLK_EN)))
        return -EIO;
    /* AutogrEEEn off: the PHY would otherwise save power behind the MAC's back */
    if (mdio_write(g, g->phy, BCM_EXP_SEL, BCM_EXP_MII_BUF_CNTL0)) return -EIO;
    if ((v = mdio_read(g, g->phy, BCM_EXP_DATA)) < 0) return v;
    if (mdio_write(g, g->phy, BCM_EXP_DATA, (unsigned)v & ~AUTOGREEEN_EN)) return -EIO;
    mdio_write(g, g->phy, BCM_EXP_SEL, 0);
    return 0;
}

/* find the PHY (the device tree's address, else the first that answers), reset it, set the RGMII delays, start the
 * negotiation (10, 100 and 1000 Mbit/s full duplex, 10 and 100 half; no pause frames). 0, or -ENODEV / -EIO */
int gn_phy_start(struct genet *g, int hwaddr)
{
    int a, id1 = -1, id2 = -1;
    for (a = hwaddr >= 0 && hwaddr < 32 ? hwaddr : 0; a < 32; a++) {
        id1 = mdio_read(g, a, MII_PHYSID1);
        id2 = mdio_read(g, a, MII_PHYSID2);
        if (id1 >= 0 && id2 >= 0 && !(id1 == 0xffff && id2 == 0xffff) && !(id1 == 0 && id2 == 0)) break;
        if (hwaddr >= 0 && hwaddr < 32) {
            gn_log(g, "no PHY answers at MDIO address %d (the device tree's): looking at the others", hwaddr);
            hwaddr = -1;
            a = -1;
        }
    }
    if (a >= 32) {
        gn_log(g, "no PHY answers on the MDIO bus");
        return -ENODEV;
    }
    g->phy = a;
    g->phy_id = (u32)id1 << 16 | (u32)id2;
    g->bcm = (g->phy_id & BCM_OUI_MASK) == (BCM54213PE_ID & BCM_OUI_MASK);
    if (mdio_write(g, a, MII_BMCR, BMCR_RESET)) return -EIO;
    u32 t0 = gn_ticks();
    int v;
    while ((v = mdio_read(g, a, MII_BMCR)) < 0 || (v & BMCR_RESET))
        if (gn_ticks() - t0 > 50) {
            gn_log(g, "the PHY did not finish its reset in 500 ms");
            return -EIO;
        }
    if (g->bcm && bcm_config(g)) {
        gn_log(g, "the PHY's RGMII delays could not be set");
        return -EIO;
    }
    mdio_write(g, a, MII_ADVERTISE, ADV_CSMA | ADV_10HALF | ADV_10FULL | ADV_100HALF | ADV_100FULL);
    mdio_write(g, a, MII_CTRL1000, ADV_1000FULL);
    mdio_write(g, a, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
    gn_log(g, "PHY %x at MDIO address %d%s: negotiating", g->phy_id, a,
           g->bcm ? (g->phy_id == BCM54213PE_ID ? " (BCM54213PE)" : " (a BCM54xx)") : " (not a Broadcom PHY: its RGMII delays left as they are)");
    g->link = 0;
    g->link_at = gn_ticks();
    return 0;
}

/* the MAC set for the link the PHY negotiated (speed, duplex), or the link flag cleared */
static void mac_link(struct genet *g)
{
    if (!g->link) {
        gn_set(g, EXT_RGMII_OOB_CTRL, RGMII_LINK, 0);
        return;
    }
    u32 cmd = (u32)(g->speed == 1000 ? 2 : g->speed == 100 ? 1 : 0) << CMD_SPEED_SHIFT | CMD_RX_PAUSE_IGNORE | CMD_TX_PAUSE_IGNORE;
    if (!g->full) cmd |= CMD_HD_EN;
    gn_set(g, EXT_RGMII_OOB_CTRL, 0, RGMII_LINK);
    gn_wr(g, UMAC_CMD, cmd);
    gn_wr(g, UMAC_CMD, cmd | CMD_TX_EN | CMD_RX_EN);
}

/* a look at the PHY at most every 100 ms (now: at once): 1 when the link changed (logged), else 0 */
int gn_link_poll(struct genet *g, int now)
{
    if (!now && gn_ticks() - g->link_at < 10) return 0;
    g->link_at = gn_ticks();
    int s0 = mdio_read(g, g->phy, MII_BMSR), s;                /* the first read: "it went down" since the last look */
    if (s0 < 0 || (s = mdio_read(g, g->phy, MII_BMSR)) < 0) return 0;
    int up = (s & BMSR_LSTATUS) && (s & BMSR_ANEGCOMPLETE), changed = 0;
    if (g->link && (!up || !(s0 & BMSR_LSTATUS))) {             /* down, or down and up again (perhaps at another speed) */
        g->link = 0;
        mac_link(g);
        gn_log(g, "link down");
        changed = 1;
    }
    if (!up || g->link) return changed;
    int adv = mdio_read(g, g->phy, MII_ADVERTISE), lpa = mdio_read(g, g->phy, MII_LPA);
    int c1000 = mdio_read(g, g->phy, MII_CTRL1000), s1000 = mdio_read(g, g->phy, MII_STAT1000);
    if (adv < 0 || lpa < 0 || c1000 < 0 || s1000 < 0) return 0;
    int common = adv & lpa;
    if ((c1000 & ADV_1000FULL) && (s1000 & LPA_1000FULL)) g->speed = 1000, g->full = 1;
    else if (common & ADV_100FULL) g->speed = 100, g->full = 1;
    else if (common & ADV_100HALF) g->speed = 100, g->full = 0;
    else if (common & ADV_10FULL) g->speed = 10, g->full = 1;
    else g->speed = 10, g->full = 0;
    g->link = 1;
    mac_link(g);
    gn_log(g, "link up: %d Mbit/s, %s duplex", g->speed, g->full ? "full" : "half");
    return 1;
}

/* ---- frames ---- */
static unsigned rx_slot(const struct genet *g, unsigned short i) { return W16(i - g->rx_base) % GN_NRX; }
static unsigned tx_slot(const struct genet *g, unsigned short i) { return W16(i - g->tx_base) % GN_NTX; }

/* a buffer of ours, empty: into the pool (posted by gn_refill) */
void gn_give(struct genet *g, int b)
{
    if (b < 0 || (unsigned)b >= g->nbuf || g->npool >= GN_NRX) return;
    for (unsigned k = 0; k < g->npool; k++) if (g->pool[k] == b) return;
    for (unsigned k = 0; k < GN_NRX; k++) if (g->rxs[k] == b) return;
    g->pool[g->npool++] = (unsigned char)b;
}

/* empty slots get buffers from the pool, in the ring's order; the consumer index then lets the chip use them */
void gn_refill(struct genet *g)
{
    unsigned short c = g->rx_c;
    while (c != g->rx_p && g->npool) {
        unsigned k = rx_slot(g, c);
        int b = g->pool[--g->npool];
        gn_cache(g, DRVC_INVAL, g->buf[b], GN_BUF);
        g->rxs[k] = (signed char)b;
        gn_wr(g, GN_RXD + k * GN_DESC + DESC_ADDR_LO, g->buf[b]);
        gn_wr(g, GN_RXD + k * GN_DESC + DESC_ADDR_HI, 0);
        gn_wr(g, GN_RXD + k * GN_DESC + DESC_LEN_STAT, (u32)GN_BUF << 16 | DS_OWN);
        c = W16(c + 1);
    }
    if (c != g->rx_c) {
        g->rx_c = c;
        gn_wr(g, RDMA_CONS_INDEX, c);
    }
}

/* frames the chip received, in order, to s->rx (until it says no room); a bad frame's buffer stays ours. The number
 * taken, or -EIO when the chip's index cannot be right (the caller restarts the chip) */
int gn_rx(struct genet *g, const struct gn_sink *s)
{
    u32 pi = gn_rd(g, RDMA_PROD_INDEX);
    unsigned short p = W16(pi), disc = W16(pi >> 16);
    int n = 0;
    if (disc != g->rx_disc) {           /* frames the chip had no descriptor for */
        g->rx_drop += W16(disc - g->rx_disc);
        g->rx_disc = disc;
    }
    if (W16(p - g->rx_c) > GN_NRX || W16(p - g->rx_p) > W16(p - g->rx_c)) {
        gn_log(g, "the receive DMA's index is wrong (%x; consumer %x, taken %x)", (u32)p, (u32)g->rx_c, (u32)g->rx_p);
        return -EIO;
    }
    while (g->rx_p != p) {
        unsigned k = rx_slot(g, g->rx_p);
        int b = g->rxs[k];
        u32 ls = gn_rd(g, GN_RXD + k * GN_DESC + DESC_LEN_STAT);
        unsigned len = (ls >> 16) & 0xfff;
        if (b < 0) {                    /* cannot happen: the chip filled a slot it was not given */
            gn_log(g, "the receive DMA used an empty descriptor (%u)", k);
            return -EIO;
        }
        if ((ls & (DS_SOP | DS_EOP)) != (DS_SOP | DS_EOP) || (ls & DS_RX_ERRORS) || len < 14 || len > GN_BUF) {
            g->rx_bad++;                /* a bad or split frame: the buffer goes straight back */
        } else {
            gn_cache(g, DRVC_INVAL, g->buf[b], len);            /* the CPU sees what the chip wrote */
            if (s->rx(s->ctx, b, len) < 0) break;               /* no room: the frame waits for the next turn */
            b = -1;
            n++;
        }
        g->rxs[k] = -1;
        if (b >= 0) g->pool[g->npool++] = (unsigned char)b;
        g->rx_p = W16(g->rx_p + 1);
    }
    return n;
}

/* a frame of netd's (one piece in the DMA region) onto the send ring: 0; -ENOSPC the ring is full (try later);
 * -ENETDOWN no link (the caller gives the tag back as an error) */
int gn_tx(struct genet *g, unsigned tag, u32 addr, unsigned len)
{
    if (!g->link) return -ENETDOWN;
    if (W16(g->tx_p - g->tx_c) >= GN_NTX) return -ENOSPC;
    if (len < 14 || len > GN_BUF - 4) return -EINVAL;
    if (len < 60) {                     /* Ethernet's shortest frame (the buffer is 1536 bytes: room to pad) */
        memset(gn_mem(addr) + len, 0, 60 - len);
        len = 60;
    }
    if (W16(g->tx_p - g->tx_c) == 0) g->tx_moved = gn_ticks();
    unsigned k = tx_slot(g, g->tx_p);
    gn_cache(g, DRVC_CLEAN, addr, len);                         /* the chip reads RAM, not the CPU's cache */
    g->txtag[k] = (unsigned short)tag;
    gn_wr(g, GN_TXD + k * GN_DESC + DESC_ADDR_LO, addr);
    gn_wr(g, GN_TXD + k * GN_DESC + DESC_ADDR_HI, 0);
    gn_wr(g, GN_TXD + k * GN_DESC + DESC_LEN_STAT, (u32)len << 16 | DS_TX_QTAG | DS_TX_APPEND_CRC | DS_SOP | DS_EOP);
    g->tx_p = W16(g->tx_p + 1);
    gn_wr(g, TDMA_PROD_INDEX, g->tx_p);
    return 0;
}

/* frames the chip has sent: their tags to s->done. The number, or -ETIMEDOUT when frames have waited a second with no
 * progress while the link is up (the send DMA is stuck: the caller restarts the chip), -EIO for an index that cannot
 * be right */
int gn_tx_done(struct genet *g, const struct gn_sink *s)
{
    unsigned short c = W16(gn_rd(g, TDMA_CONS_INDEX));
    int n = 0;
    if (W16(c - g->tx_c) > W16(g->tx_p - g->tx_c)) {
        gn_log(g, "the send DMA's index is wrong (%x; ours %x .. %x)", (u32)c, (u32)g->tx_c, (u32)g->tx_p);
        return -EIO;
    }
    while (g->tx_c != c) {
        s->done(s->ctx, g->txtag[tx_slot(g, g->tx_c)]);
        g->tx_c = W16(g->tx_c + 1);
        n++;
    }
    if (n || g->tx_c == g->tx_p || !g->link) g->tx_moved = gn_ticks();
    else if (gn_ticks() - g->tx_moved > GN_TXSTUCK) {
        gn_log(g, "the send DMA is stuck: %u frames have waited 1 s", (unsigned)W16(g->tx_p - g->tx_c));
        return -ETIMEDOUT;
    }
    return n;
}

/* after a stuck or confused DMA: frames on the send ring given back as errors, every buffer of ours posted again, the
 * chip reset and the link set again (the PHY keeps its link). 0 */
int gn_restart(struct genet *g, const struct gn_sink *s)
{
    gn_stop(g);
    while (g->tx_c != g->tx_p) {
        s->done(s->ctx, g->txtag[tx_slot(g, g->tx_c)]);
        g->tx_err++;
        g->tx_c = W16(g->tx_c + 1);
    }
    for (unsigned k = 0; k < GN_NRX; k++)
        if (g->rxs[k] >= 0) {
            g->pool[g->npool++] = (unsigned char)g->rxs[k];
            g->rxs[k] = -1;
        }
    g->resets++;
    gn_reset(g);
    gn_rings(g);
    mac_link(g);
    gn_log(g, "the chip reset and started again (%u times)", g->resets);
    return 0;
}

/* ---- the self-test: frames sent to ourselves through the UniMAC's local loopback, checked byte by byte. It runs with
 * the cache calls the driver will use, so it shows whether they work (a missing clean sends what RAM held, a missing
 * invalidate reads what the cache held). mem: len bytes of the DMA region (whole buffers), nothing else in use. 0 all
 * frames came back right; -ETIMEDOUT none came back (no loopback: nothing shown); -EIO frames came back wrong ---- */
#define ST_FRAMES 4
int gn_selftest(struct genet *g, u32 mem, u32 len, u32 seed)
{
    unsigned nb = len / GN_BUF, k, got = 0, bad = 0;
    if (nb < ST_FRAMES * 2) return -EINVAL;
    if (nb > GN_NRX) nb = GN_NRX;
    unsigned nrx = nb - ST_FRAMES;
    u32 save_buf[GN_NRX];
    unsigned save_n = g->nbuf;
    memcpy(save_buf, g->buf, sizeof save_buf);
    g->nbuf = nrx;
    g->npool = 0;
    for (k = 0; k < nrx; k++) {
        g->buf[k] = mem + (k + ST_FRAMES) * GN_BUF;
        memset(gn_mem(g->buf[k]), 0xa5, GN_BUF);                /* the CPU's stale idea of the buffer */
        g->pool[g->npool++] = (unsigned char)k;
    }
    gn_rings(g);
    gn_set(g, EXT_RGMII_OOB_CTRL, 0, RGMII_LINK);
    gn_wr(g, UMAC_CMD, 2u << CMD_SPEED_SHIFT | CMD_LCL_LOOP_EN | CMD_RX_PAUSE_IGNORE | CMD_TX_PAUSE_IGNORE);
    gn_wr(g, UMAC_CMD, 2u << CMD_SPEED_SHIFT | CMD_LCL_LOOP_EN | CMD_RX_PAUSE_IGNORE | CMD_TX_PAUSE_IGNORE | CMD_TX_EN | CMD_RX_EN);
    int link = g->link;
    g->link = 1;
    for (k = 0; k < ST_FRAMES; k++) {
        unsigned char *f = gn_mem(mem + k * GN_BUF);
        unsigned n = 60 + k * 400, i;
        u32 x = seed + k * 0x9e3779b9u;
        memcpy(f, g->mac, 6);
        memcpy(f + 6, g->mac, 6);
        f[12] = 0x88;                   /* IEEE's local experimental type */
        f[13] = 0xb5;
        for (i = 14; i < n; i++) {
            x = x * 1103515245u + 12345u;
            f[i] = (unsigned char)(x >> 16);
        }
        gn_tx(g, 0x100 + k, mem + k * GN_BUF, n);
    }
    u32 t0 = gn_ticks();
    while (got + bad < ST_FRAMES && gn_ticks() - t0 < 20) {
        u32 pi = gn_rd(g, RDMA_PROD_INDEX);
        while (g->rx_p != W16(pi)) {
            unsigned s = rx_slot(g, g->rx_p);
            u32 ls = gn_rd(g, GN_RXD + s * GN_DESC + DESC_LEN_STAT);
            unsigned n = (ls >> 16) & 0xfff, want = 60 + (got + bad) * 400;
            int b = g->rxs[s];
            if (b >= 0) gn_cache(g, DRVC_INVAL, g->buf[b], n > GN_BUF ? GN_BUF : n);
            if (b >= 0 && n == want && !(ls & DS_RX_ERRORS) && !memcmp(gn_mem(g->buf[b]), gn_mem(mem + (got + bad) * GN_BUF), n))
                got++;
            else
                bad++;
            g->rx_p = W16(g->rx_p + 1);
        }
        if (got + bad < ST_FRAMES) gn_sleep(1);
    }
    g->link = link;
    gn_stop(g);
    gn_wr(g, UMAC_CMD, 0);
    g->nbuf = save_n;
    memcpy(g->buf, save_buf, sizeof save_buf);
    g->npool = 0;
    for (k = 0; k < GN_NRX; k++) g->rxs[k] = -1;
    g->tx_c = g->tx_p;
    gn_reset(g);
    if (bad) {
        gn_log(g, "self-test: %u of %d frames came back wrong through the loopback", bad, ST_FRAMES);
        return -EIO;
    }
    if (!got) return -ETIMEDOUT;
    if (got < ST_FRAMES) {
        gn_log(g, "self-test: only %u of %d frames came back", got, ST_FRAMES);
        return -EIO;
    }
    return 0;
}
