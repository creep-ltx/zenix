/* SPDX-License-Identifier: GPL-2.0-only */
/* The xHCI USB host controller (DESIGN D46): registers, TRBs, contexts, and the driver's state. Written from Intel's
 * xHCI 1.2 specification (the section numbers below are its); U-Boot's drivers/usb/host/xhci*.c (GPL-2.0+) read for
 * the order of operations, no code taken. Everything the controller sees is little-endian; the driver never touches
 * the registers or the DMA memory through a C pointer, only through the includer's rg_* and dm_* functions
 * (docs/d46-usb.md 4.2). */
#pragma once

/* capability registers (5.3) */
#define XC_CAPLENGTH    0x00            /* byte 0: the operational registers' offset; bytes 2-3 HCIVERSION */
#define XC_HCSPARAMS1   0x04            /* MaxSlots 7:0, MaxIntrs 18:8, MaxPorts 31:24 */
#define XC_HCSPARAMS2   0x08            /* ERST Max 7:4, scratchpads hi 25:21, lo 31:27 */
#define XC_HCCPARAMS1   0x10            /* AC64 bit 0, CSZ bit 2 */
#define XC_DBOFF        0x14
#define XC_RTSOFF       0x18

/* operational registers (5.4), from the operational base */
#define XO_USBCMD       0x00
#define XO_USBSTS       0x04
#define XO_PAGESIZE     0x08
#define XO_CRCR         0x18            /* 64 bits */
#define XO_DCBAAP       0x30            /* 64 bits */
#define XO_CONFIG       0x38
#define XO_PORTSC(n)    (0x400 + 0x10 * ((n) - 1))
#define CMD_RS          0x01
#define CMD_HCRST       0x02
#define CMD_INTE        0x04
#define STS_HCH         0x01
#define STS_HSE         0x04
#define STS_EINT        0x08
#define STS_CNR         0x800
#define STS_HCE         0x1000

/* PORTSC (5.4.8) */
#define PS_CCS          (1u << 0)
#define PS_PED          (1u << 1)
#define PS_OCA          (1u << 3)
#define PS_PR           (1u << 4)
#define PS_PP           (1u << 9)
#define PS_SPEED(v)     (((v) >> 10) & 15)
#define PS_CSC          (1u << 17)
#define PS_PEC          (1u << 18)
#define PS_WRC          (1u << 19)
#define PS_OCC          (1u << 20)
#define PS_PRC          (1u << 21)
#define PS_PLC          (1u << 22)
#define PS_CEC          (1u << 23)
#define PS_CHANGES      (PS_CSC | PS_PEC | PS_WRC | PS_OCC | PS_PRC | PS_PLC | PS_CEC)
/* what a write must carry over unchanged: the read-only and read-write bits, never PED (writing 1 disables the port)
 * nor a change bit (writing 1 clears it) */
#define PS_KEEP         ((1u << 0) | (1u << 3) | (15u << 10) | (1u << 30) | (15u << 5) | (1u << 9) | (3u << 14) | (7u << 25))

/* runtime registers (5.5): interrupter 0 */
#define XR_IMAN         0x20
#define XR_IMOD         0x24
#define XR_ERSTSZ       0x28
#define XR_ERSTBA       0x30            /* 64 bits */
#define XR_ERDP         0x38            /* 64 bits; EHB bit 3 */
#define IMAN_IP         0x01
#define IMAN_IE         0x02

/* TRBs (6.4): four little-endian words; control word: cycle bit 0, type 15:10 */
#define TRB_C           (1u << 0)
#define TRB_TC          (1u << 1)       /* link: toggle cycle */
#define TRB_ENT         (1u << 1)
#define TRB_ISP         (1u << 2)
#define TRB_CH          (1u << 4)
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_BSR         (1u << 9)       /* Address Device: block SET_ADDRESS */
#define TRB_DIR_IN      (1u << 16)
#define TRB_TYPE(t)     ((u32)(t) << 10)
#define TRB_GETTYPE(c)  (((c) >> 10) & 63)
#define TT_NORMAL       1
#define TT_SETUP        2
#define TT_DATA         3
#define TT_STATUS       4
#define TT_LINK         6
#define TT_ENABLE_SLOT  9
#define TT_DISABLE_SLOT 10
#define TT_ADDRESS_DEV  11
#define TT_CONFIG_EP    12
#define TT_EVAL_CTX     13
#define TT_RESET_EP     14
#define TT_STOP_EP      15
#define TT_SET_DEQ      16
#define TT_NOOP_CMD     23
#define TT_EV_TRANSFER  32
#define TT_EV_CMD       33
#define TT_EV_PORT      34
#define TT_EV_HOST      37
/* completion codes (6.4.5) */
#define CC_SUCCESS      1
#define CC_DATA_BUF     2
#define CC_BABBLE       3
#define CC_TRANSACTION  4
#define CC_TRB          5
#define CC_STALL        6
#define CC_NO_SLOTS     9
#define CC_SHORT        13
#define CC_STOPPED      26

/* the ring sizes: one page each (a link TRB at the end of every ring: never across a page, 4.1) */
#define XC_PAGE         4096
#define RING_TRBS       64              /* a transfer ring: 1 KB, 63 usable */
#define CMD_TRBS        256             /* the command ring and the event ring: a page each */
#define EV_TRBS         256
#define XC_MAXDEV       2               /* slots used (CONFIG.MaxSlotsEn): one socket, no hubs yet (docs/d46-usb.md 3.1) */
#define XC_MAXEP        2               /* endpoints with a ring besides EP0, a device (bulk IN and OUT; two pages a device) */
#define XC_BOUNCE       65536           /* the bulk bounce buffer, 64 KB aligned: one TRB never crosses 64 KB (4.11.7.1) */
#define XC_SCRATCH_MAX  32              /* scratchpad pages accepted (HCSPARAMS2) */

struct xring {
    u32 base;                           /* bus address of TRB 0 */
    unsigned n;                         /* TRBs including the link */
    unsigned enq;                       /* next TRB to write */
    u32 cycle;                          /* the producer's cycle bit */
};

struct xc_ep {
    unsigned char dci;                  /* 0: unused */
    unsigned char halted;
    struct xring ring;
    u32 last;                           /* bus address of the last TRB queued (its completion ends the transfer) */
    int busy;                           /* a transfer is queued */
    int done;                           /* its completion code, 0 while it runs */
    u32 left;                           /* bytes not transferred (the event's residue) */
};

struct xc_slot {
    unsigned char used, speed, port;
    u32 out, in;                        /* output and input device contexts (bus addresses) */
    struct xring ep0;
    struct xc_ep ep[XC_MAXEP];
    int ctl_done;                       /* EP0: completion code of the running control transfer */
    u32 ctl_left, ctl_status;           /* its data stage's residue, its status TRB */
};

struct xhci {
    u32 base, size;                     /* the register window (the includer's) */
    u32 op, rt, db;                     /* operational, runtime and doorbell offsets in it */
    unsigned maxslots, maxports, nscratch, csz;     /* csz: 32 or 64 bytes a context */
    u32 dma, dmalen;                    /* the claim's DMA memory, page aligned here */
    u32 dcbaa, erst, ctlbuf, cbw, csw, spa, bounce;
    struct xring cmd;
    u32 ev, ev_deq, ev_cycle;           /* the event ring: base, the next TRB to read, its cycle state */
    int cmd_done;                       /* the running command's completion code (0: none yet) */
    u32 cmd_slot, cmd_trb;              /* its slot id; the command TRB it answers */
    u32 port_change;                    /* root ports with a change (bit n: port n, 1-31) */
    struct xc_slot slot[XC_MAXDEV + 1]; /* by slot id */
    unsigned events, commands, transfers, errors;
    int dead;                           /* a fatal error was seen: nothing more is sent */
    const char *why;                    /* why it is dead */
};

/* the cache calls the includer makes (as DRV_CACHE's) */
#define XC_CLEAN        1
#define XC_INVAL        2               /* always clean and invalidate: nothing is dropped unwritten */
