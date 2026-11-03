/* SPDX-License-Identifier: GPL-2.0-only */
/* NE2000 PC Cards in the A600/A1200's PC Card slot (DESIGN D36): Gayle's slot, the card's CIS, and the chip, a
 * DP8390 core (National's, or a clone) or ASIX's AX88190 / AX88790 (the Netgear FA411 is an AX88x90 card).
 *
 * The numbers below are hardware facts, read from: Linux's arch/m68k/include/asm/amigayle.h and
 * arch/m68k/amiga/pcmcia.c (Gayle's registers, the slot's addresses and reset), drivers/net/ethernet/8390/apne.c (an
 * NE2000 card through Gayle: the odd-register window, the CIS's FUNCID, CONFIG and CFTABLE_ENTRY), 8390.h and ne.c
 * (the DP8390's registers and the PROM), axnet_cs.c (the AX88190's quirks, the FA411's identity strings); FreeBSD's
 * dev/ed/if_ed_pccard.c (the AX88x90 probe, the IOBASE registers, "fa411 has ax88790"); NetBSD's
 * dev/pcmcia/if_ne_pcmcia.c; the PC Card standard's tuple layout as those drivers parse it. All GPL-2.0 or BSD. The
 * code in ne2000-core.c is this project's own.
 *
 * Every address here is the 68k's: the core reaches the slot through ne_rb/ne_wb/ne_rw/ne_ww, which the driver maps
 * to the bus and the host test to its model (tools/ne2000-model.h). */
#pragma once

/* Gayle (A600/A1200): its registers, one byte each */
#define GAYLE_CS        0xDA8000u       /* card status (read) and control (write) */
#define  CS_CCDET       0x40            /* a card is in the slot */
#define  CS_WR          0x08            /* writes to the card enabled */
#define  CS_DA          0x10
#define GAYLE_INTREQ    0xDA9000u       /* never written by the driver: the kernel's (kernel/ide.c) */
#define GAYLE_INTEN     0xDAA000u       /* never written by the driver: the kernel keeps it 0 (no PCMCIA interrupt) */
#define GAYLE_CONFIG    0xDAB000u       /* bits 1-0 programming voltage (0: 0 V), bits 3-2 access time (0: 250 ns) */
#define  CFG_0V_250NS   0x00
/* the slot */
#define PC_ATTR         0xA00000u       /* attribute memory: the CIS, one byte at every even address */
#define PC_ATTR_SIZE    0x20000u
#define PC_IO           0xA20000u       /* I/O: words, and bytes at even ports */
#define PC_IO_ODD       0xA30000u       /* bytes at odd ports: port p at PC_IO_ODD + p - 1 (apne.c's GAYLE_ODD) */
#define PC_RESET        0xA40000u       /* write: the card's reset line on; read: off */

/* the CIS (PC Card standard, as Linux's and the BSDs' parsers read it) */
#define CISTPL_NULL     0x00
#define CISTPL_VERS_1   0x15
#define CISTPL_CONFIG   0x1A
#define CISTPL_CFTABLE  0x1B
#define CISTPL_MANFID   0x20
#define CISTPL_FUNCID   0x21
#define CISTPL_END      0xFF
#define FUNCID_NETWORK  6
#define CIS_MAXBYTES    0x2000          /* at most this many CIS bytes are walked (a chain of 8 KB is far beyond any card) */
/* the card's configuration registers, at the CONFIG tuple's base (attribute bytes from it) */
#define CCR_COR         0x00            /* configuration option: the entry's index; 0 = not configured */
#define  COR_LEVEL      0x40
#define CCR_CCSR        0x02            /* configuration and status */
#define  CCSR_PWRDWN    0x04
#define CCR_IOBASE0     0x0A            /* the I/O base the card decodes (AX88x90: written, as Linux and FreeBSD do) */
#define CCR_IOBASE1     0x0C
#define NE_IOBASE       0x300           /* the I/O base this driver gives a card that takes any (apne.c's IOBASE) */

/* the DP8390, page 0 unless said */
#define E_CR            0x00
#define  CR_STP         0x01
#define  CR_STA         0x02
#define  CR_TXP         0x04
#define  CR_RD_READ     0x08
#define  CR_RD_WRITE    0x10
#define  CR_NODMA       0x20
#define  CR_PAGE1       0x40
#define E_PSTART        0x01
#define E_PSTOP         0x02
#define E_BNRY          0x03
#define E_TPSR          0x04            /* write; read: TSR */
#define E_TSR           0x04
#define  TSR_PTX        0x01
#define E_TBCR0         0x05
#define E_TBCR1         0x06
#define E_ISR           0x07
#define  ISR_PRX        0x01
#define  ISR_PTX        0x02
#define  ISR_RXE        0x04
#define  ISR_TXE        0x08
#define  ISR_OVW        0x10
#define  ISR_CNT        0x20
#define  ISR_RDC        0x40
#define  ISR_RST        0x80
#define E_RSAR0         0x08
#define E_RSAR1         0x09
#define E_RBCR0         0x0A
#define E_RBCR1         0x0B
#define E_RCR           0x0C
#define  RCR_AB         0x04            /* broadcasts */
#define  RCR_MON        0x20
#define  RCR_AX_INTT    0x40            /* AX88x90: always set (Linux's axnet_cs) */
#define E_TCR           0x0D
#define  TCR_LB         0x02            /* internal loopback (the "stopped" state of the NS procedure) */
#define  TCR_AX_FDU     0x80            /* AX88x90: full duplex */
#define E_DCR           0x0E
#define  DCR_WTS        0x01            /* word transfers */
#define  DCR_BYTE       0x48            /* FT1, LS: byte transfers */
#define  DCR_WORD       0x49
#define E_IMR           0x0F
#define E_CNTR0         0x0D            /* read (page 0): the frame-alignment, CRC and missed counters */
#define E_CNTR1         0x0E
#define E_CNTR2         0x0F
#define E1_PAR0         0x01            /* page 1: the station address, 6 bytes */
#define E1_CURR         0x07
#define E1_MAR0         0x08            /* page 1: the multicast filter, 8 bytes */
#define NE_DATA         0x10            /* the remote DMA's data port */
#define NE_RESET        0x1F            /* read: the chip's reset; write: back */
#define AX_MII          0x14            /* AX88x90: the MII's bit-banged port */
#define  MII_MDC        0x01
#define  MII_DIR_IN     0x02
#define  MII_MDI        0x04
#define  MII_MDO        0x08
#define AX_TEST         0x15            /* AX88x90: 0 on an AX88190, else an AX88790 */
#define AX_GPIO         0x17            /* AX88790: 0x10 selects the internal PHY */
#define MII_BMSR        1
#define  BMSR_LINK      0x0004          /* latched low: read twice for the state now */
#define MII_PHYID1      2
#define MII_LPA         5
#define  LPA_FULL       0x0140          /* 100 or 10 full duplex */
#define  LPA_100        0x0180

/* the card's 16 KB of frame memory, in 256-byte pages: two send buffers, then the receive ring (both chips) */
#define NE_MEM0         0x40            /* the first page (0x4000) */
#define NE_MEMEND       0x80            /* one past the last (0x8000) */
#define NE_TXPAGES      6               /* one send buffer: 1536 bytes */
#define NE_RXSTART      (NE_MEM0 + 2 * NE_TXPAGES)
#define NE_BUF          1536
#define NE_LINKEVERY    100             /* ticks between looks at the PHY (1 s) */
#define NE_TXSTUCK      100             /* ticks (1 s) a send may take before the chip is reset */
#define NE_SETTLE       50              /* ticks (0.5 s) a card put in must stay in before it is used */
enum { NE_HERE, NE_AWAY, NE_LEFT, NE_BACK, NE_OTHER, NE_BAD };

struct ne_cis {
    unsigned funcid;                    /* 0xFFFF: no FUNCID tuple */
    unsigned manf, card;                /* MANFID (0 if none) */
    char vers[2][40];                   /* VERS_1's manufacturer and product, printable characters only */
    u32 cor;                            /* the configuration registers' base (attribute offset); 0xFFFFFFFF: none */
    unsigned rmask;                     /* which registers are present (bit k: register 2k) */
    unsigned index;                     /* the chosen configuration entry (0: none fits) */
    u32 iobase;                         /* its I/O base (NE_IOBASE when the card takes any) */
    unsigned iolines;
    int anybase;                        /* the card decodes any base: the driver tells it which (IOBASE registers) */
    unsigned ntuples;
};

struct ne {
    const char *ifn;                    /* the interface, for the log */
    struct ne_cis cis;
    u32 io;                             /* the card's I/O base (ISA port numbers) */
    int ax;                             /* 0 a DP8390, 1 an AX88190, 2 an AX88790 */
    int phy;                            /* the MII address of the AX88x90's PHY, -1 none */
    unsigned char mac[6];
    int link, full, speed;
    u32 link_at;
    unsigned next;                      /* the receive ring's next page to read */
    int tx_len[2];                      /* bytes waiting in each send buffer (0: free) */
    int txing;                          /* which buffer the chip sends (-1: none) */
    unsigned tx_order;                  /* the buffer to send next */
    u32 tx_since;
    int gone;                           /* the card left the slot */
    u32 away_at;                        /* the tick it was last seen away */
    int on;                             /* the card is configured (its COR written) */
    unsigned odd_next;                  /* frames whose next page was not where their length put it */
    u32 rx_bad, rx_drop, tx_err;        /* since the last report */
    u32 overruns, resets;
};

/* the received frames' way out (the driver's transfer block, or the host test's netd) */
struct ne_sink {
    int (*take)(void *ctx, unsigned char **p);         /* a free buffer: its number and where, or -1 */
    void (*give)(void *ctx, int buf, unsigned len);    /* buffer buf holds a frame of len bytes */
    void *ctx;
};

int ne_card_in(void);
void ne_slot_reset(void);
int ne_read_cis(struct ne *n);
int ne_card_on(struct ne *n);
void ne_card_off(struct ne *n);
int ne_probe(struct ne *n);
int ne_start(struct ne *n);
void ne_stop(struct ne *n);
int ne_link_poll(struct ne *n, int now);
int ne_poll(struct ne *n, const struct ne_sink *s, int max);
int ne_tx(struct ne *n, const unsigned char *f, unsigned len);
int ne_tx_free(const struct ne *n);
int ne_bring_up(struct ne *n);
int ne_watch(struct ne *n);
void ne_quiesce(const struct ne *n, struct udrvquiesce *q);
