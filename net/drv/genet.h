/* SPDX-License-Identifier: GPL-2.0-only */
/* The CM4's (and the Pi 4's) Ethernet: Broadcom GENET v5 with a BCM54213PE PHY on RGMII (DESIGN D19 5.2, step 9).
 * The register numbers and bits below are hardware facts, read from Linux's drivers/net/ethernet/broadcom/genet/
 * (bcmgenet.h, bcmmii.c; GPL-2.0), U-Boot's drivers/net/bcmgenet.c (GPL-2.0+) and the BCM54xx PHY's from Linux's
 * include/linux/brcmphy.h and drivers/net/phy/broadcom.c; rondoval's emu68-genet-driver (genet.device, GPL-2.0+ /
 * MPL-2.0) was read for what Emu68 needs (no code taken). The code in genet-core.c is this project's own.
 *
 * Registers are little-endian 32-bit words in a 64 KB window (hwinfo.genet). The DMA descriptors live in the window
 * too (12 bytes each: length and status, address low, address high). Every frame buffer lies in the claim's DMA
 * region; the 68k address is the DMA address under Emu68 (DESIGN D19 5.2). */
#pragma once

#define GN_WINDOW               0x10000

#define SYS_REV_CTRL            0x0000          /* bits 27-24: 6 = GENET v5 */
#define SYS_PORT_CTRL           0x0004
#define  PORT_MODE_EXT_GPHY     3
#define SYS_RBUF_FLUSH_CTRL     0x0008
#define EXT_RGMII_OOB_CTRL      0x008c
#define  RGMII_LINK             (1u << 4)
#define  OOB_DISABLE            (1u << 5)
#define  RGMII_MODE_EN          (1u << 6)
#define  ID_MODE_DIS            (1u << 16)      /* set: the MAC adds no TX clock delay (only for plain "rgmii") */
#define INTRL2_0                0x0200
#define INTRL2_1                0x0240
#define  INTRL2_CPU_CLEAR       0x08
#define  INTRL2_CPU_MASK_SET    0x10
#define RBUF_CTRL               0x0300
#define  RBUF_64B_EN            (1u << 0)       /* a 64-byte status block before every frame: off (D19 14, step 9) */
#define  RBUF_ALIGN_2B          (1u << 1)
#define RBUF_CHK_CTRL           0x0314
#define RBUF_ENERGY_CTRL        0x039c
#define  ENERGY_EEE_EN          (1u << 0)       /* EEE breaks reception: off */
#define  ENERGY_PM_EN           (1u << 1)
#define RBUF_TBUF_SIZE_CTRL     0x03b4
#define TBUF_CTRL               0x0600
#define TBUF_ENERGY_CTRL        0x0614
#define UMAC_CMD                0x0808
#define  CMD_TX_EN              (1u << 0)
#define  CMD_RX_EN              (1u << 1)
#define  CMD_SPEED_SHIFT        2               /* 0 10, 1 100, 2 1000 Mbit/s */
#define  CMD_PROMISC            (1u << 4)
#define  CMD_CRC_FWD            (1u << 6)
#define  CMD_RX_PAUSE_IGNORE    (1u << 8)
#define  CMD_HD_EN              (1u << 10)
#define  CMD_SW_RESET           (1u << 13)
#define  CMD_LCL_LOOP_EN        (1u << 15)
#define  CMD_TX_PAUSE_IGNORE    (1u << 28)
#define UMAC_MAC0               0x080c
#define UMAC_MAC1               0x0810
#define UMAC_MAX_FRAME_LEN      0x0814
#define UMAC_TX_FLUSH           0x0b34
#define UMAC_MIB_CTRL           0x0d80
#define UMAC_MDIO_CMD           0x0e14
#define  MDIO_START_BUSY        (1u << 29)
#define  MDIO_READ_FAIL         (1u << 28)
#define  MDIO_RD                (2u << 26)
#define  MDIO_WR                (1u << 26)
#define  MDIO_PMD_SHIFT         21
#define  MDIO_REG_SHIFT         16
#define UMAC_MDF_CTRL           0x0e50
#define UMAC_MDF_ADDR           0x0e54          /* 17 filters of two words each; filter k is MDF_CTRL bit 16 - k */

/* the descriptors and the DMA engines (one ring each way: ring 16, the default queue) */
#define GN_RXD                  0x2000          /* 256 receive descriptors */
#define GN_TXD                  0x4000          /* 256 send descriptors */
#define GN_DESC                 12
#define  DESC_LEN_STAT          0
#define  DESC_ADDR_LO           4
#define  DESC_ADDR_HI           8
#define GN_RDMA                 (GN_RXD + 256 * GN_DESC)        /* 0x2c00: the receive DMA's registers */
#define GN_TDMA                 (GN_TXD + 256 * GN_DESC)        /* 0x4c00 */
#define GN_RING16               (16 * 0x40)                     /* ring 16's registers, from the engine's base */
#define RDMA_WRITE_PTR          (GN_RDMA + GN_RING16 + 0x00)
#define RDMA_PROD_INDEX         (GN_RDMA + GN_RING16 + 0x08)    /* bits 31-16: frames discarded (no descriptor) */
#define RDMA_CONS_INDEX         (GN_RDMA + GN_RING16 + 0x0c)
#define RDMA_RING_BUF_SIZE      (GN_RDMA + GN_RING16 + 0x10)    /* descriptors << 16 | buffer bytes */
#define RDMA_START_ADDR         (GN_RDMA + GN_RING16 + 0x14)    /* in 32-bit words */
#define RDMA_END_ADDR           (GN_RDMA + GN_RING16 + 0x1c)
#define RDMA_XON_XOFF_THRESH    (GN_RDMA + GN_RING16 + 0x28)
#define RDMA_READ_PTR           (GN_RDMA + GN_RING16 + 0x2c)
#define TDMA_READ_PTR           (GN_TDMA + GN_RING16 + 0x00)
#define TDMA_CONS_INDEX         (GN_TDMA + GN_RING16 + 0x08)
#define TDMA_PROD_INDEX         (GN_TDMA + GN_RING16 + 0x0c)
#define TDMA_RING_BUF_SIZE      (GN_TDMA + GN_RING16 + 0x10)
#define TDMA_START_ADDR         (GN_TDMA + GN_RING16 + 0x14)
#define TDMA_END_ADDR           (GN_TDMA + GN_RING16 + 0x1c)
#define TDMA_MBUF_DONE_THRESH   (GN_TDMA + GN_RING16 + 0x24)
#define TDMA_FLOW_PERIOD        (GN_TDMA + GN_RING16 + 0x28)
#define TDMA_WRITE_PTR          (GN_TDMA + GN_RING16 + 0x2c)
#define GN_RINGS                (17 * 0x40)                     /* the engine's own registers after the 17 rings */
#define RDMA_RING_CFG           (GN_RDMA + GN_RINGS + 0x00)
#define RDMA_CTRL               (GN_RDMA + GN_RINGS + 0x04)
#define RDMA_STATUS             (GN_RDMA + GN_RINGS + 0x08)     /* bit 0: the engine is stopped */
#define RDMA_BURST              (GN_RDMA + GN_RINGS + 0x0c)
#define TDMA_RING_CFG           (GN_TDMA + GN_RINGS + 0x00)
#define TDMA_CTRL               (GN_TDMA + GN_RINGS + 0x04)
#define TDMA_STATUS             (GN_TDMA + GN_RINGS + 0x08)
#define TDMA_BURST              (GN_TDMA + GN_RINGS + 0x0c)
#define  DMA_EN                 (1u << 0)
#define  DMA_RING16_EN          (1u << 17)      /* DMA_CTRL: ring 16's buffer enable */
#define  DMA_DISABLED           (1u << 0)       /* DMA_STATUS */
/* descriptor status (the low 16 bits of the length and status word; the length in bits 27-16) */
#define DS_OWN                  0x8000
#define DS_EOP                  0x4000
#define DS_SOP                  0x2000
#define DS_RX_ERRORS            0x001f          /* overrun, CRC, receive error, no octet, too long */
#define DS_TX_APPEND_CRC        0x0040
#define DS_TX_QTAG              (0x3f << 7)

/* the PHY (IEEE 802.3 clause 22, and the BCM54xx's own windows) */
#define MII_BMCR                0x00
#define  BMCR_RESET             0x8000
#define  BMCR_ANENABLE          0x1000
#define  BMCR_ANRESTART         0x0200
#define MII_BMSR                0x01
#define  BMSR_LSTATUS           0x0004          /* latched low: read twice for the state now */
#define  BMSR_ANEGCOMPLETE      0x0020
#define MII_PHYSID1             0x02
#define MII_PHYSID2             0x03
#define MII_ADVERTISE           0x04
#define MII_LPA                 0x05
#define  ADV_CSMA               0x0001
#define  ADV_10HALF             0x0020
#define  ADV_10FULL             0x0040
#define  ADV_100HALF            0x0080
#define  ADV_100FULL            0x0100
#define MII_CTRL1000            0x09
#define  ADV_1000FULL           0x0200
#define MII_STAT1000            0x0a
#define  LPA_1000FULL           0x0800
#define BCM_EXP_DATA            0x15
#define BCM_EXP_SEL             0x17
#define BCM_AUX_CTL             0x18
#define  AUXCTL_SHDWSEL_MISC    0x0007
#define  AUXCTL_READ_SHIFT      12
#define  AUXCTL_MISC_WREN       0x8000
#define  AUXCTL_MISC_RGMII_SKEW 0x0100          /* the PHY delays RXC (the "rxid" in rgmii-rxid) */
#define BCM_SHD                 0x1c
#define  SHD_WRITE              0x8000
#define  SHD_CLK_CTL            0x03
#define  SHD_CLK_GTXCLK_EN      0x0200          /* the PHY delays TXC: off for rgmii-rxid (the MAC does it) */
#define BCM_EXP_MII_BUF_CNTL0   0x0d00
#define  AUTOGREEEN_EN          0x0001
#define BCM54213PE_ID           0x600d84a2
#define BCM_OUI_MASK            0xfffffc00

/* what the driver keeps */
#define GN_NRX      32          /* receive descriptors (a power of two; at most DRV_MAXBUF buffers posted) */
#define GN_NTX      16          /* send descriptors: netd's DMA send buffers (DRV_BATCH) */
#define GN_BUF      1536        /* netd's buffer size: a frame of up to 1536 bytes (UMAC_MAX_FRAME_LEN) */
#define GN_TXSTUCK  100         /* ticks (1 s) without progress while frames are out: the send DMA is stuck */

struct genet {
    const char *ifn;            /* the interface, for the log */
    u32 base;                   /* the register window */
    unsigned char mac[6];
    int phy;                    /* the PHY's MDIO address */
    u32 phy_id;
    int link, speed, full;      /* the link now: up, 10/100/1000, full duplex */
    u32 link_at;                /* the tick of the last look at the PHY */
    int bcm;                    /* the PHY is a BCM54xx: its RGMII delays are set */
    /* receive: slot k of the ring holds buffer rxs[k] (-1: empty, waiting for one); counters are the chip's 16-bit
     * indexes: rx_c the consumer index written (the chip may fill up to GN_NRX past it), rx_p the frames taken */
    u32 buf[GN_NRX];            /* netd's receive buffers by number */
    unsigned nbuf;
    signed char rxs[GN_NRX];
    unsigned char pool[GN_NRX]; /* buffers that are ours and in no slot */
    unsigned npool;
    unsigned short rx_base, rx_c, rx_p, rx_disc;
    /* send: slot k holds netd's tag txtag[k] */
    unsigned short tx_base, tx_p, tx_c;
    unsigned short txtag[GN_NTX];
    u32 tx_moved;               /* the tick the send ring last moved (or was empty) */
    /* counts since the last report to netd */
    u32 rx_bad, rx_drop, tx_err;
    unsigned resets;
};

/* the frame tags and buffers the core hands back (the driver's transfer block, or the host test's netd) */
struct gn_sink {
    int (*rx)(void *ctx, int buf, unsigned len);        /* a received frame: 0 taken, -1 no room this turn */
    void (*done)(void *ctx, unsigned tag);              /* a frame sent (or dropped: counted in tx_err) */
    void *ctx;
};

int gn_probe(struct genet *g);
int gn_reset(struct genet *g);
int gn_rings(struct genet *g);
void gn_stop(struct genet *g);
int gn_phy_start(struct genet *g, int hwaddr);
int gn_link_poll(struct genet *g, int now);
int gn_rx(struct genet *g, const struct gn_sink *s);
void gn_give(struct genet *g, int buf);
void gn_refill(struct genet *g);
int gn_tx(struct genet *g, unsigned tag, u32 addr, unsigned len);
int gn_tx_done(struct genet *g, const struct gn_sink *s);
int gn_restart(struct genet *g, const struct gn_sink *s);
int gn_selftest(struct genet *g, u32 mem, u32 len, u32 seed);
void gn_quiesce(const struct genet *g, struct udrvquiesce *q);
