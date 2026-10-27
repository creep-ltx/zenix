/* SPDX-License-Identifier: GPL-2.0-only */
/* a2065: the Commodore A2065 Ethernet board (Zorro II, an AMD Am7990 LANCE with 32 KB of its own RAM), polled
 * (DESIGN D19 5.1, 5.5, step 8). netd starts it as /sbin/net/a2065 IFACE. FS-UAE emulates the board, so every
 * `make check` tests the Ethernet path with it.
 *
 * The board: the LANCE's data and address ports at +0x4000 and +0x4002, its RAM at +0x8000 (32 KB), which the LANCE
 * itself sees at 0x8000-0xFFFF. In that RAM: the initialisation block, 16 receive and 4 send descriptors and their
 * 1536-byte buffers. The LANCE moves frames between the wire and its RAM; this driver copies them between that RAM
 * and netd's buffers (the board's DMA never reaches the Amiga's memory, so no DMA region is needed: the claim's
 * quiesce program stops the LANCE when the driver or netd ends). The byte order: CSR3's BSWP, so frames read in the
 * 68k's order; the descriptors and the initialisation block are 16-bit words, which puts the MAC address in the
 * block with each pair of bytes swapped. The MAC: Commodore's 00:80:10, then the board's serial number's low 24 bits
 * (as Linux's driver makes it).
 *
 * Polled: no interrupt (the LANCE's INEA stays off). The descriptors are looked at on every doorbell and every tick
 * (10 ms) while frames move, every 2 ticks when idle; every wait on the chip has a time limit and says what it waited
 * for. Read from the Am7990 data sheet's register and descriptor layout and Linux's drivers/net/ethernet/amd/a2065.c
 * (GPL-2.0) for the board's addresses, the byte order and the MAC; the code is this file's own. */
#include "drv.h"

#define LANCE_RDP 0x4000
#define LANCE_RAP 0x4002
#define BOARD_RAM 0x8000                /* also the LANCE's own address of it */
#define NRX 16                          /* the LANCE's rings: 2^4 receive, 2^2 send descriptors */
#define LOG_NRX 4
#define NTX 4
#define LOG_NTX 2
#define BUFSZ 1536
/* the board RAM's layout (offsets from BOARD_RAM) */
#define IB 0                            /* the initialisation block, 24 bytes */
#define RXD 24                          /* 16 receive descriptors of 8 bytes (8-byte aligned) */
#define TXD (RXD + NRX * 8)             /* 4 send descriptors */
#define RXB (TXD + NTX * 8)             /* the buffers */
#define TXB (RXB + NRX * BUFSZ)
_Static_assert(TXB + NTX * BUFSZ <= 0x8000, "the rings fit the board's 32 KB");
/* CSR0 */
#define C0_ERR  0x8000
#define C0_BABL 0x4000
#define C0_CERR 0x2000
#define C0_MISS 0x1000
#define C0_MERR 0x0800
#define C0_RINT 0x0400
#define C0_TINT 0x0200
#define C0_IDON 0x0100
#define C0_TDMD 0x0008
#define C0_STOP 0x0004
#define C0_STRT 0x0002
#define C0_INIT 0x0001
#define C3_BSWP 0x0004
/* descriptor bits (the high byte of the second word) */
#define D_OWN 0x80
#define D_ERR 0x40
#define D_STP 0x02
#define D_ENP 0x01

static const char *ifn = "a2065";
static volatile unsigned short *rdp, *rap;
static unsigned char *ram;              /* the board's RAM as the 68k sees it */
static struct drvq q;
static unsigned rx_next, tx_next, tx_old, tx_busy;
static unsigned long missed, rxerr, txerr;
/* netd's frames waiting for a free send descriptor (they stay netd's until copied: their tags go back then) */
static struct udrvframe pend[DRV_BATCH];
static unsigned npend;

static void csr_w(unsigned r, unsigned short v) { *rap = (unsigned short)r; *rdp = v; }
static unsigned short csr_r(unsigned r) { *rap = (unsigned short)r; return *rdp; }
static volatile unsigned short *w16(unsigned off) { return (volatile unsigned short *)(ram + off); }
static void say(const char *fmt, u32 a, u32 b)
{
    char t[160], l[200];
    snprintf(t, sizeof t, fmt, a, b);
    snprintf(l, sizeof l, "a2065: %s: %s", ifn, t);
    printf("%s\n", l);
}

/* the rings and the block in the board's RAM; then INIT and STRT (the LANCE stopped first). 0, or -1 */
static int lance_start(const unsigned char mac[6])
{
    csr_w(0, C0_STOP);
    memset(ram, 0, TXB + NTX * BUFSZ);
    *w16(IB + 0) = 0;                                   /* mode: normal */
    for (int i = 0; i < 6; i += 2) {                    /* the address, each pair swapped (16-bit words) */
        ram[IB + 2 + i] = mac[i + 1];
        ram[IB + 3 + i] = mac[i];
    }
    /* the multicast filter stays 0: none */
    *w16(IB + 16) = (unsigned short)(BOARD_RAM + RXD);
    *w16(IB + 18) = (unsigned short)(LOG_NRX << 13 | (BOARD_RAM + RXD) >> 16);
    *w16(IB + 20) = (unsigned short)(BOARD_RAM + TXD);
    *w16(IB + 22) = (unsigned short)(LOG_NTX << 13 | (BOARD_RAM + TXD) >> 16);
    for (int i = 0; i < NRX; i++) {
        u32 a = BOARD_RAM + RXB + (u32)i * BUFSZ;
        *w16(RXD + i * 8 + 0) = (unsigned short)a;
        *w16(RXD + i * 8 + 2) = (unsigned short)(D_OWN << 8 | a >> 16);
        *w16(RXD + i * 8 + 4) = (unsigned short)(-BUFSZ | 0xF000);
        *w16(RXD + i * 8 + 6) = 0;
    }
    for (int i = 0; i < NTX; i++) {
        u32 a = BOARD_RAM + TXB + (u32)i * BUFSZ;
        *w16(TXD + i * 8 + 0) = (unsigned short)a;
        *w16(TXD + i * 8 + 2) = (unsigned short)(a >> 16);
        *w16(TXD + i * 8 + 4) = 0xF000;
        *w16(TXD + i * 8 + 6) = 0;
    }
    rx_next = tx_next = tx_old = tx_busy = 0;
    csr_w(1, (unsigned short)(BOARD_RAM + IB));
    csr_w(2, (unsigned short)((BOARD_RAM + IB) >> 16));
    csr_w(3, C3_BSWP);
    csr_w(0, C0_INIT);
    u32 t0 = sys_ticks();
    unsigned short c0;
    while (!((c0 = csr_r(0)) & (C0_IDON | C0_ERR))) {
        if (sys_ticks() - t0 > 20) {                    /* 200 ms: the LANCE needs microseconds */
            say("the LANCE did not finish its initialisation in 200 ms (CSR0 %x)", c0, 0);
            return -1;
        }
        sys_sleep(1);
    }
    if (c0 & C0_ERR) {
        say("the LANCE's initialisation failed (CSR0 %x)", c0, 0);
        return -1;
    }
    csr_w(0, C0_IDON);                                  /* written as 1: cleared */
    csr_w(0, C0_STRT);                                  /* no INEA: polled */
    return 0;
}

/* received frames into netd's buffers (as many as this batch takes) */
static int rx_poll(void)
{
    int moved = 0;
    while (q.x.nrx < DRV_BATCH) {
        volatile unsigned short *d = w16(RXD + rx_next * 8);
        unsigned bits = d[1] >> 8;
        if (bits & D_OWN) break;
        if ((bits & (D_STP | D_ENP)) != (D_STP | D_ENP) || (bits & D_ERR)) rxerr++;   /* a chained or bad frame */
        else {
            unsigned len = (d[3] & 0xFFF), b;
            len = len >= 4 ? len - 4 : 0;               /* without the CRC */
            if (len < 14 || len > BUFSZ) rxerr++;
            else if ((int)(b = (unsigned)drv_take(&q)) < 0) {
                missed++;                               /* netd holds every buffer: dropped */
            } else {
                memcpy((void *)q.at.buf[b], ram + RXB + rx_next * BUFSZ, len);
                drv_rx(&q, (int)b, len);
            }
        }
        d[3] = 0;
        d[1] = (unsigned short)(D_OWN << 8 | (d[1] & 0xFF));   /* the LANCE's again */
        rx_next = (rx_next + 1) % NRX;
        moved = 1;
    }
    return moved;
}

/* send descriptors the LANCE is done with; netd's waiting frames into free ones */
static int tx_poll(void)
{
    int moved = 0;
    while (tx_busy) {
        volatile unsigned short *d = w16(TXD + tx_old * 8);
        if ((d[1] >> 8) & D_OWN) break;
        if ((d[1] >> 8) & D_ERR) txerr++;
        tx_old = (tx_old + 1) % NTX;
        tx_busy--;
        moved = 1;
    }
    unsigned k = 0;
    while (k < npend && tx_busy < NTX && q.x.ndone < DRV_BATCH) {
        struct udrvframe *f = &pend[k++];
        unsigned char *b = ram + TXB + tx_next * BUFSZ;
        unsigned n = drv_gather(f, b, BUFSZ);
        if (n < 60) { memset(b + n, 0, 60 - n); n = 60; }   /* Ethernet's shortest frame */
        volatile unsigned short *d = w16(TXD + tx_next * 8);
        d[2] = (unsigned short)(-(int)n | 0xF000);
        d[3] = 0;
        d[1] = (unsigned short)((D_OWN | D_STP | D_ENP) << 8 | (d[1] & 0xFF));
        drv_done(&q, f->tag);                           /* copied: netd may reuse the frame now */
        tx_next = (tx_next + 1) % NTX;
        tx_busy++;
        moved = 1;
    }
    if (k) {
        memmove(pend, pend + k, (npend - k) * sizeof pend[0]);
        npend -= k;
        csr_w(0, C0_TDMD);                              /* send now */
    }
    return moved;
}

int main(int argc, char **argv)
{
    unsigned char mac[6];
    if (argc < 2) { printf("usage: a2065 IFACE\n"); return 2; }
    ifn = argv[1];
    const struct hwboard *bd = drv_board(drv_hwinfo(), 0x202, 112);    /* Commodore A2065 */
    if (!bd) { say("no A2065 board in the hardware table", 0, 0); return 1; }
    int claim = sys_drv(DRV_CLAIM, bd->addr, 0);
    if (claim < 0) { say("the claim of the board at %x was refused (%d)", bd->addr, (u32)claim); return 1; }
    rdp = (volatile unsigned short *)(bd->addr + LANCE_RDP);
    rap = (volatile unsigned short *)(bd->addr + LANCE_RAP);
    ram = (unsigned char *)(bd->addr + BOARD_RAM);
    struct udrvquiesce qz;                              /* the LANCE stopped when this driver or netd ends */
    memset(&qz, 0, sizeof qz);
    qz.n = 2;
    qz.step[0].op = qz.step[1].op = DRVS_WRITE;
    qz.step[0].size = qz.step[1].size = 2;
    qz.step[0].addr = bd->addr + LANCE_RAP;
    qz.step[1].addr = bd->addr + LANCE_RDP;
    qz.step[1].value = C0_STOP;
    if (sys_drv(DRV_QUIESCE, (u32)claim, (u32)&qz) < 0) { say("the quiesce program was refused", 0, 0); return 1; }
    mac[0] = 0x00;
    mac[1] = 0x80;
    mac[2] = 0x10;
    mac[3] = (unsigned char)(bd->serial >> 16);
    mac[4] = (unsigned char)(bd->serial >> 8);
    mac[5] = (unsigned char)bd->serial;
    if (lance_start(mac)) { csr_w(0, C0_STOP); return 1; }
    int r = drv_attach(&q, ifn, mac, 1500, 8, DRVA_LINK, claim, 0, 0);
    if (r < 0) {
        say("the attach was refused (%d)", (u32)r, 0);
        csr_w(0, C0_STOP);
        return 1;
    }
    say("the board at %x, interface %d", bd->addr, (u32)r);
    for (int idle = 0;;) {
        if (drv_wait(idle > 2 ? 2 : idle ? 1 : 0) < 0) break;
        unsigned short c0 = csr_r(0);
        if (c0 & (C0_MISS | C0_BABL | C0_CERR | C0_MERR | C0_RINT | C0_TINT)) {
            if (c0 & C0_MISS) missed++;
            csr_w(0, c0 & (C0_MISS | C0_BABL | C0_CERR | C0_MERR | C0_RINT | C0_TINT));  /* written as 1: cleared */
        }
        if ((c0 & C0_STOP) || (c0 & C0_MERR)) {         /* the chip stopped (a memory error): start it again */
            say("the LANCE stopped (CSR0 %x): starting it again", c0, 0);
            if (lance_start(mac)) break;
            for (unsigned k = 0; k < npend; k++) drv_done(&q, pend[k].tag);   /* lost: TCP sends them again */
            npend = 0;
        }
        int moved = rx_poll() | tx_poll();
        q.x.rx_drop = missed; q.x.rx_bad = rxerr; q.x.tx_err = txerr;
        missed = rxerr = txerr = 0;
        if ((r = drv_xfer(&q, 1)) < 0) {
            if (r == -ECONNRESET || r == -EPERM) break;  /* netd is gone */
            say("the transfer was refused (%d)", (u32)r, 0);
            break;
        }
        for (u32 k = 0; k < q.x.ntx; k++) {
            if (npend < DRV_BATCH) pend[npend++] = q.x.tx[k];
            else drv_done(&q, q.x.tx[k].tag);           /* cannot happen: netd sends at most a batch */
        }
        moved |= tx_poll() | (q.x.ntx != 0);
        idle = moved || q.x.more || npend || q.x.nrx || q.x.ndone ? 0 : idle + 1;
    }
    csr_w(0, C0_STOP);
    say("the doorbell closed (ifdown, or netd is gone): the LANCE stopped", 0, 0);
    return 0;
}
