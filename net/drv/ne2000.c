/* SPDX-License-Identifier: GPL-2.0-only */
/* ne2000: an NE2000 PC Card in the A600/A1200's slot (the Netgear FA411, an ASIX AX88x90 card, and DP8390 cards),
 * polled (DESIGN D36, D19 5.5). netd starts it as /sbin/net/ne2000 IFACE. The slot, the CIS and the chip are
 * ne2000-core.c (shared with the host test against the model, tools/ne2000-model.h); this file is the Zenix side: the
 * claim of the slot, the quiesce program, the driver protocol with netd, the card leaving and coming back.
 *
 * The kernel claims the slot only on a machine with an A600/A1200 Gayle and no memory in the slot's addresses
 * (0x600000-0xA4FFFF): that memory and a card would clash. Gayle's PC Card interrupt stays off (the kernel keeps its
 * enable register 0): the chip is looked at on every doorbell and every tick while frames move, every 2 ticks when
 * idle, the PHY every second. The chip's frame memory holds the frames; this driver copies them between it and netd's
 * buffers through the remote DMA's data port, a word at a time. */
#include "drv.h"
#include "ne2000.h"

static unsigned char ne_rb(u32 a) { return *(volatile unsigned char *)a; }
static void ne_wb(u32 a, unsigned char v) { *(volatile unsigned char *)a = v; }
static unsigned short ne_rw(u32 a) { return *(volatile unsigned short *)a; }
static void ne_ww(u32 a, unsigned short v) { *(volatile unsigned short *)a = v; }
static u32 ne_ticks(void) { return sys_ticks(); }
static void ne_sleep(u32 t) { sys_sleep(t); }
static void ne_log(struct ne *n, const char *fmt, ...)
{
    char t[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    dprintf(1, "ne2000: %s: %s\n", n->ifn, t);  /* netd's standard output: the kernel log */
}

#include "ne2000-core.c"

static struct ne n;
static struct drvq q;
static int claim;
static struct udrvframe pend[DRV_BATCH];        /* netd's frames waiting for a free send buffer */
static unsigned npend;
static unsigned char txb[NE_BUF];

static int fatal;                               /* the chip does not start again: the driver ends */

static int sink_take(void *ctx, unsigned char **p)
{
    (void)ctx;
    int b = q.x.nrx < DRV_BATCH ? drv_take(&q) : -1;   /* never more than a transfer carries (the core's max) */
    if (b >= 0) *p = (unsigned char *)q.at.buf[b];
    return b;
}
static void sink_give(void *ctx, int b, unsigned len) { (void)ctx; drv_rx(&q, b, len); }
static const struct ne_sink sink = { sink_take, sink_give, 0 };

/* the quiesce program for the card now (none while no card is configured: a card put in later is not ours yet) */
static int set_quiesce(int on)
{
    struct udrvquiesce qz;
    if (on) ne_quiesce(&n, &qz);
    else memset(&qz, 0, sizeof qz);
    int r = sys_drv(DRV_QUIESCE, (u32)claim, (u32)&qz);
    if (r < 0) ne_log(&n, "the quiesce program was refused (%d)", r);
    return r;
}

/* netd's waiting frames into the card's send buffers; each tag goes back once copied (TCP sends a lost one again) */
static int send_pending(void)
{
    unsigned k = 0;
    int moved = 0, r;
    while (k < npend && q.x.ndone < DRV_BATCH) {
        struct udrvframe *f = &pend[k];
        if (n.gone || !n.link) {
            n.tx_err++;                                 /* no card or no link: the frame goes back unsent */
        } else {
            if (!ne_tx_free(&n)) break;
            unsigned len = drv_gather(f, txb, sizeof txb);
            if ((r = ne_tx(&n, txb, len)) == -EIO) {
                drv_done(&q, f->tag);
                k++;
                n.tx_err++;
                if (ne_card_in()) {                     /* a card that left is the next turn's (ne_watch) */
                    n.resets++;
                    ne_log(&n, "the chip's remote DMA failed: starting it again");
                    if (ne_start(&n)) fatal = 1;
                }
                break;
            }
            if (r < 0) n.tx_err++;
        }
        drv_done(&q, f->tag);
        k++;
        moved = 1;
    }
    if (k) {
        memmove(pend, pend + k, (npend - k) * sizeof pend[0]);
        npend -= k;
    }
    return moved;
}

int main(int argc, char **argv)
{
    n.ifn = argc >= 2 ? argv[1] : "?";
    if (argc < 2) { dprintf(1, "usage: ne2000 IFACE\n"); return 2; }
    for (int i = 2; i < argc; i++) ne_log(&n, "unknown word in the driver line: %s (ignored)", argv[i]);
    claim = sys_drv(DRV_CLAIM, DRV_PCMCIA, 0);
    if (claim == -ENODEV) { ne_log(&n, "no PC Card slot (this is not an A600 or A1200)"); return 1; }
    if (claim == -EADDRINUSE) {
        ne_log(&n, "memory lies in the PC Card slot's addresses (0x600000-0xa4ffff): a card cannot be used on this machine");
        return 1;
    }
    if (claim < 0) { ne_log(&n, "the claim of the PC Card slot was refused (%d)", claim); return 1; }
    if (!ne_card_in()) { ne_log(&n, "no card in the PC Card slot"); return 1; }
    if (ne_bring_up(&n)) return 1;
    if (set_quiesce(1) < 0) { ne_card_off(&n); return 1; }
    ne_link_poll(&n, 1);
    int r = drv_attach(&q, n.ifn, n.mac, 1500, 8, n.link ? DRVA_LINK : 0, claim, 0, 0);
    if (r < 0) { ne_log(&n, "the attach was refused (%d)", r); ne_card_off(&n); return 1; }
    if (q.at.bufsize < 1514) {
        ne_log(&n, "netd's buffers hold %d bytes: too small for a frame", (int)q.at.bufsize);
        ne_card_off(&n);
        return 1;
    }
    ne_log(&n, "interface %d, %02x:%02x:%02x:%02x:%02x:%02x", r, n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
    for (int idle = 0;;) {
        if (drv_wait(n.gone ? 10 : idle > 2 ? 2 : idle ? 1 : 0) < 0) break;
        int moved = 0;
        switch (ne_watch(&n)) {
        case NE_LEFT:                                   /* its quiesce steps must not write into the next card */
            set_quiesce(0);
            break;
        case NE_BACK:
            if (set_quiesce(1) < 0) { ne_card_off(&n); return 1; }
            moved = 1;
            break;
        case NE_OTHER:
            ne_log(&n, "the driver ends: netd starts it again for the new card");
            return 1;
        case NE_BAD:
            ne_log(&n, "the card put in cannot be used: the driver ends");
            return 1;
        }
        if (!n.gone) {
            moved |= ne_link_poll(&n, 0);
            if ((r = ne_poll(&n, &sink, DRV_BATCH - (int)q.x.nrx)) < 0) {
                if (!ne_card_in()) continue;            /* it left: the next turn's ne_watch says so */
                n.resets++;
                ne_log(&n, "starting the chip again (%d times so far)", (int)n.resets);
                if (ne_start(&n)) fatal = 1;
                r = 1;
            }
            moved |= r > 0;
        }
        moved |= send_pending();
        if (fatal) { ne_log(&n, "the chip does not start again: the driver ends"); ne_card_off(&n); return 1; }
        q.x.rx_drop = n.rx_drop; q.x.rx_bad = n.rx_bad; q.x.tx_err = n.tx_err;
        n.rx_drop = n.rx_bad = n.tx_err = 0;
        int nrx = (int)q.x.nrx;
        if ((r = drv_xfer(&q, !n.gone && n.link)) < 0) {
            if (r != -ECONNRESET && r != -EPERM) ne_log(&n, "the transfer was refused (%d)", r);
            break;                                      /* -ECONNRESET, -EPERM: netd is gone */
        }
        for (u32 k = 0; k < q.x.ntx; k++) {
            if (npend < DRV_BATCH) pend[npend++] = q.x.tx[k];
            else drv_done(&q, q.x.tx[k].tag);           /* cannot happen: netd sends at most a batch */
        }
        moved |= send_pending() | (q.x.ntx != 0) | (nrx != 0);
        idle = moved || q.x.more || npend || q.x.ndone ? 0 : idle + 1;
    }
    if (!n.gone) ne_card_off(&n);
    ne_log(&n, "the doorbell closed (ifdown, or netd is gone): the card stopped");
    return 0;
}
