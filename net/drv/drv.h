/* SPDX-License-Identifier: GPL-2.0-only */
/* What every network driver shares (DESIGN D19 2.3, 2.4, 5; the protocol in kernel/netproto.h): a driver is a user
 * process netd starts as /sbin/net/NAME IFACE [ARGS], with a pipe as its standard input, the doorbell. It claims its
 * device, attaches (netd lends it receive buffers) and then moves frames in batches with DRV_XFER: it waits on the
 * doorbell and a short timer, never per frame. POLLHUP on the doorbell (or -ECONNRESET from the kernel) means netd
 * has ended: the driver stops its device and ends (the kernel runs its quiesce program anyway). */
#pragma once
#include "../../user/libz.h"
#include "../../kernel/netproto.h"
#include "../../kernel/hwinfo.h"

/* the frames a driver holds between two transfers */
struct drvq {
    struct udrvattach at;               /* netd's answer: the buffers */
    struct udrvxfer x;                  /* the next transfer, being filled */
    unsigned char own[DRV_MAXBUF];      /* buffer i is ours (empty) */
};

static inline void drv_out(const char *name, const char *msg)
{
    dprintf(1, "%s: %s\n", name, msg);      /* netd's standard output: the kernel log */
}

/* the hardware table (DRV_HWINFO); 0 if there is none */
static inline const struct hwinfo *drv_hwinfo(void)
{
    static struct hwinfo h;
    return sys_drv(DRV_HWINFO, (u32)&h, sizeof h) >= HWINFO_FIRST ? &h : 0;
}
/* the first Zorro board of a manufacturer and product that is not memory, or 0 */
static inline const struct hwboard *drv_board(const struct hwinfo *h, unsigned manuf, unsigned prod)
{
    for (u32 i = 0; h && i < h->nboards && i < HW_MAXBOARDS; i++)
        if (h->board[i].manufacturer == manuf && h->board[i].product == prod && !(h->board[i].type & 0x20))
            return &h->board[i];
    return 0;
}

/* attach: netd answers with the interface number and lends the buffers, all ours */
static inline int drv_attach(struct drvq *q, const char *ifname, const unsigned char mac[6], unsigned mtu, unsigned nrx,
                             u32 flags, int claim, u32 dma, u32 dmalen)
{
    memset(q, 0, sizeof *q);
    q->at.size = sizeof q->at;
    strncpy(q->at.name, ifname, sizeof q->at.name);
    memcpy(q->at.mac, mac, 6);
    q->at.mtu = (unsigned short)mtu;
    q->at.flags = flags;
    q->at.nrx = nrx;
    q->at.claim = (u32)claim;
    q->at.dma = dma;
    q->at.dmalen = dmalen;
    int r = sys_drv(DRV_ATTACH, (u32)&q->at, sizeof q->at);
    if (r < 0) return r;
    if (q->at.nbuf > DRV_MAXBUF) return -EINVAL;
    for (u32 i = 0; i < q->at.nbuf; i++) q->own[i] = 1;
    q->x.size = sizeof q->x;
    return r;
}
/* an empty buffer of ours (it stays ours until drv_rx hands it over), or -1 */
static inline int drv_take(struct drvq *q)
{
    for (u32 i = 0; i < q->at.nbuf; i++) if (q->own[i]) { q->own[i] = 0; return (int)i; }
    return -1;
}
/* buffer num now holds a frame of len bytes: it goes to netd with the next transfer (0, or -1 when the batch is full:
 * call drv_xfer first) */
static inline int drv_rx(struct drvq *q, int num, unsigned len)
{
    if (q->x.nrx >= DRV_BATCH) return -1;
    q->x.rx[q->x.nrx].num = (unsigned short)num;
    q->x.rx[q->x.nrx].len = (unsigned short)len;
    q->x.rx[q->x.nrx].flags = 0;
    q->x.nrx++;
    return 0;
}
static inline int drv_done(struct drvq *q, unsigned tag)
{
    if (q->x.ndone >= DRV_BATCH) return -1;
    q->x.done[q->x.ndone++] = (unsigned short)tag;
    return 0;
}
/* one transfer: what was queued goes, the buffers given back are ours again; q->x then holds netd's frames to send
 * (q->x.tx[0..ntx-1], to be answered with drv_done) and q->x.more. -> 0, or -errno (-ECONNRESET: netd is gone) */
static inline int drv_xfer(struct drvq *q, int link)
{
    q->x.size = sizeof q->x;
    q->x.link = (u32)link;
    q->x.nfree = q->x.ntx = q->x.more = 0;
    int r = sys_drv(DRV_XFER, (u32)&q->x, sizeof q->x);
    q->x.nrx = q->x.ndone = 0;
    q->x.rx_bad = q->x.rx_drop = q->x.tx_err = 0;
    if (r < 0) { q->x.nfree = q->x.ntx = 0; return r; }
    if (q->x.nfree > DRV_BATCH) q->x.nfree = DRV_BATCH;
    if (q->x.ntx > DRV_BATCH) q->x.ntx = DRV_BATCH;
    for (u32 i = 0; i < q->x.nfree; i++) if (q->x.freed[i] < q->at.nbuf) q->own[q->x.freed[i]] = 1;
    return 0;
}
/* a sent frame's bytes gathered into one buffer (at most max); its length */
static inline unsigned drv_gather(const struct udrvframe *f, unsigned char *buf, unsigned max)
{
    unsigned n = 0;
    for (u32 k = 0; k < f->npieces && k < 4; k++) {
        u32 l = f->piece[k].len;
        if (l > max - n) l = max - n;
        memcpy(buf + n, (const void *)f->piece[k].addr, l);
        n += l;
    }
    return n;
}
/* wait for the doorbell or ticks (0: do not wait); -1 when netd is gone (the pipe's far end closed) */
static inline int drv_wait(int ticks)
{
    struct pollfd p = { 0, POLLIN, 0 };
    if (sys_evwait(&p, 1, ticks) <= 0) return 0;
    if (p.revents & POLLIN) {
        char b[8];
        int n = sys_read(0, b, sizeof b);
        if (n == 0) return -1;
    }
    return (p.revents & POLLHUP) && !(p.revents & POLLIN) ? -1 : 0;
}
