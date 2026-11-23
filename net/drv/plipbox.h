/* SPDX-License-Identifier: GPL-2.0-only */
/* The plipbox (DESIGN D39): Christian Vogelgsang's Ethernet bridge on the Amiga's parallel port, an AVR (an Arduino
 * Nano) with an ENC28J60. The numbers and the protocol below are facts read from plipbox 0.6 (2015, the last release;
 * GPL-2.0-or-later, github.com/cnvogelg/plipbox): avr/src/pb_proto.c (the box's side of the line), avr/src/bridge.c
 * (its magic frames), amiga/src/plipbox/hwpar.asm and hw.c (the AmigaOS driver's side, plipbox.device). The code in
 * plipbox-core.c is this project's own.
 *
 * The line: the eight data lines are CIA-A's port B; the handshake is CIA-B's port A bits 0-2: BUSY (in: the box's
 * "RAK"), POUT (out: the Amiga's "REQ") and SEL (out: the Amiga starts every transfer). The box asks for a transfer
 * by a pulse on ACK, CIA-A's FLAG (the kernel counts its edges, DRV_PARACK). No CRC on the line (the ENC28J60 checks
 * the Ethernet's own): a damaged byte reaches IP's and TCP's checksums only.
 *
 * Every address here is the 68k's: the line (plipbox-line.c) reaches the port through pb_rd/pb_wr/pb_bset/pb_bclr,
 * which the kernel maps to the CIAs (DRV_PARXFER) and the host test to its model (tools/plipbox-model.h). */
#pragma once

#define PB_DATA         0xBFE101u       /* CIA-A port B: the data lines (DRV_PARPORT) */
#define PB_DDATA        0xBFE301u       /* its direction: 0xFF the Amiga drives them, 0x00 the box may */
#define PB_CTRL         0xBFD000u       /* CIA-B port A: bit 0 BUSY, 1 POUT, 2 SEL; 3-7 the serial port's (never ours) */
#define PB_DCTRL        0xBFD200u       /* its direction */
#define PB_BUSY         0               /* bit numbers in PB_CTRL */
#define PB_POUT         1
#define PB_SEL          2
#define PB_LINES        0x07            /* the three bits that are ours; the other five are left as they are */

/* the commands (the byte on the data lines when SEL rises) */
#define PB_CMD_SEND     0x11            /* the Amiga sends a frame, every byte answered */
#define PB_CMD_RECV     0x22            /* the Amiga takes the box's frame, every byte answered */
#define PB_CMD_SENDB    0x33            /* the Amiga sends a frame in a burst (0.6) */
#define PB_CMD_RECVB    0x44            /* the box sends a frame in a burst (0.6): not used, section 4 of D39 */

/* the magic frames: an Ethernet header of 14 bytes with one of these types */
#define PB_MAGIC_ONLINE   0xFFFF        /* Amiga -> box: online, the source is the MAC; box -> Amiga: send it (again) */
#define PB_MAGIC_OFFLINE  0xFFFE        /* Amiga -> box: offline (the box drops what comes from the network) */
#define PB_MAGIC_LOOPBACK 0xFFFD        /* Amiga -> box: answer with an online request; box -> Amiga: send it back */

#define PB_HDR          14
#define PB_MAXFRAME     1514            /* the box's buffer (avr/src/pkt_buf.h) */
#define PB_BUF          1518            /* the size word, a frame, the odd byte's pad, rounded */
#define PB_WAIT         50              /* ticks (the kernel's 100 Hz, on PAL and NTSC): a transfer's deadline,
                                         * 500 ms, as plipbox.device's and the box's own */
#define PB_PROBE        5               /* ticks: 50 ms, a look for a box that stopped answering */
#define PB_POLLCHECK    32              /* line reads between two looks at the clock (each read is an E-clock cycle) */
#define PB_KEEPALIVE    500             /* ticks (5 s) without a transfer before an online frame is sent anyway */
#define PB_DOWNAFTER    3               /* transfers failed in a row before the link is down */
#define PB_RETRY        200             /* ticks (2 s) between looks for a box that stopped answering */
#define PB_GAP          30              /* ticks: away this long between two looks at the clock inside a frame, the
                                         * box may have given up (its own 500 ms per wait): the frame is not trusted */
#define PB_GAPWORDS     64              /* words of a frame between two looks at the clock */

/* plipbox.device's own address (amiga/src/device.c), used when the box gives none */
#define PB_DEFMAC       { 0x1a, 0x11, 0xaf, 0xa0, 0x47, 0x11 }

struct pb {
    const char *ifn;
    unsigned char mac[6];
    int burst;                          /* frames go out in bursts (0x33); 0: "noburst", every byte answered (0x11) */
    int down;                           /* the box stopped answering: looked for every PB_RETRY ticks */
    unsigned fails;                     /* transfers failed in a row */
    u32 acks;                           /* the ACK count served (DRV_PARACK's value when the last request was taken) */
    u32 last_ok;                        /* the tick of the last transfer that went through */
    int wedged;                         /* a receive was cut between its size bytes (section 5 of D39) */
    const char *stage;                  /* where the last transfer failed */
    unsigned nlog;                      /* error lines logged (the first 8, then every 256th) */
    u32 tx_ok, tx_err, rx_ok, rx_bad, rx_drop, rx_empty, online_asks, loops;
    u32 rx_n, rx_ticks, tx_n, tx_ticks;  /* frames of more than a header each way and their ticks on the line (the
                                         * ticks' sum over many frames is their time, whatever a frame's own) */
    u32 naps, late;                     /* the driver's sleeps; those after which a request was waiting */
    unsigned char tx[PB_BUF];           /* a frame to send: its size word, then the frame (pb_send's buffer) */
};

/* where netd's receive buffers come from (the driver: netd's; the test: its own) */
struct pb_sink {
    int (*take)(void *ctx, unsigned char **buf);        /* a buffer of at least PB_BUF bytes, its number; -1: none */
    void (*give)(void *ctx, int num, unsigned len);     /* buffer num holds a frame of len bytes */
    void *ctx;
};
