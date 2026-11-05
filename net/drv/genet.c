/* SPDX-License-Identifier: GPL-2.0-only */
/* genet: the Ethernet port of the Raspberry Pi CM4 (and Pi 4) under Emu68, a Broadcom GENET v5 with a BCM54213PE PHY,
 * polled (DESIGN D19 5.2, step 9). netd starts it as /sbin/net/genet IFACE [cache=kernel|cache=user] [noselftest].
 * The chip and the PHY are genet-core.c (shared with the host test against the model, tools/genet-model.h); this file
 * is the Zenix side: the register access, the cache calls, the driver protocol with netd.
 *
 * The chip moves frames by DMA straight into netd's buffers, which netd makes in the claim's DMA region (DRVA_DMA:
 * netd copies each received frame out at once and each frame to send into one of the region's 16 send buffers), so
 * no frame is copied twice. The ARM's data cache is not coherent with the chip's DMA: a buffer is cleaned and
 * invalidated before the chip may fill it and invalidated again before netd reads it; a frame to send is cleaned
 * before its descriptor. Two ways to do that, chosen at the start and logged: Emu68's cache range opcodes from this
 * process (only when the hardware table says the Emu68 has them, after a child process tried them: an Emu68 that
 * refuses them in user mode kills the child, not the driver), or the kernel's DRV_CACHE call (always correct; one
 * system call a buffer). A self-test then sends frames to itself through the chip's loopback with the chosen way and
 * checks them byte by byte: frames that come back wrong mean the cache calls do not work, and the driver falls back to
 * the kernel's, or refuses. The owner's checklist (docs/a1200-checklist-network.md) reads these lines.
 *
 * Polled: no sleep while frames move, then one tick, then two when idle; the PHY looked at every 100 ms; netd's
 * doorbell wakes it for a frame to send. A send DMA stuck for a second, or an index that cannot be right, restarts the
 * chip (logged). When netd closes the doorbell (ifdown, or netd ended) the driver stops the chip and ends; the kernel
 * runs the quiesce program anyway when it ends. */
#include "drv.h"
#include "genet.h"

#define PROBE_ARG "--probe-rangeops"
#define REGION ((GN_NRX + DRV_BATCH) * GN_BUF)  /* netd's 32 receive and 16 send buffers: 72 KB */

static int cache_user;          /* 1: Emu68's range opcodes from this process; 0: the kernel's DRV_CACHE */
static int cache_fails;

static u32 gn_rd(struct genet *g, u32 off)
{
    u32 v = __builtin_bswap32(*(volatile u32 *)(g->base + off));
    __asm__ volatile("nop" ::: "memory");       /* Emu68 turns NOP into a barrier */
    return v;
}
static void gn_wr(struct genet *g, u32 off, u32 v)
{
    *(volatile u32 *)(g->base + off) = __builtin_bswap32(v);
    __asm__ volatile("nop" ::: "memory");
}
static u32 gn_ticks(void) { return sys_ticks(); }
static void gn_sleep(u32 t) { sys_sleep(t); }
static unsigned char *gn_mem(u32 a) { return (unsigned char *)a; }
/* "probe" in the driver line: netd's card probe (D19 6.3) started this driver for a card nobody configured; until the
 * card is there the driver says nothing (a machine without it hears nothing of it, as a Linux driver that finds no
 * device), then everything as always */
static int quiet;
static void gn_log(struct genet *g, const char *fmt, ...)
{
    if (quiet) return;
    char t[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    dprintf(1, "genet: %s: %s\n", g->ifn, t);   /* netd's standard output: the kernel log */
}
/* Emu68's range opcodes (base A0, length D1, whole 64-byte lines): clean, or clean and invalidate; never a bare
 * invalidate (CLAUDE.md: nothing dropped unwritten). The same words the kernel's DRV_CACHE uses */
static void range_op(unsigned how, u32 a, u32 n)
{
    u32 lo = a & ~63u, len = ((a + n + 63) & ~63u) - lo;
    if (how == DRVC_CLEAN)
        __asm__ volatile("move.l %0,%%a0\n\tmove.l %1,%%d1\n\t.word 0xF460,0x1C00" :: "r"(lo), "r"(len) : "a0", "d1", "memory");
    else
        __asm__ volatile("move.l %0,%%a0\n\tmove.l %1,%%d1\n\t.word 0xF460,0x1800" :: "r"(lo), "r"(len) : "a0", "d1", "memory");
}
static void gn_cache(struct genet *g, unsigned how, u32 a, u32 n)
{
    if (cache_user) {
        range_op(how, a, n);
        return;
    }
    int r = sys_drv(DRV_CACHE | how << 8, a, n);
    if (r < 0 && cache_fails++ < 4) gn_log(g, "the kernel refused a cache call at %x (%d bytes): %d", a, (int)n, r);
}

#include "genet-core.c"

/* the child that tries the range opcodes in user mode: it ends 0, or the kernel kills it (vector 11) */
static int probe(void)
{
    static unsigned char b[128] __attribute__((aligned(64)));
    memset(b, 0x5a, sizeof b);
    range_op(DRVC_CLEAN, (u32)b, 64);
    range_op(DRVC_INVAL, (u32)b, 64);
    return b[0] == 0x5a && b[63] == 0x5a ? 0 : 1;
}

static struct genet g;
static struct drvq q;
static u32 dma;
static struct udrvframe pend[DRV_BATCH];        /* netd's frames waiting for room on the send ring */
static unsigned npend;
static unsigned short late[DRV_BATCH * 2];      /* tags to give back that did not fit this transfer */
static unsigned nlate;

static int sink_rx(void *ctx, int b, unsigned len) { (void)ctx; return drv_rx(&q, b, len); }
static void sink_done(void *ctx, unsigned tag)
{
    (void)ctx;
    if (drv_done(&q, tag) < 0 && nlate < sizeof late / sizeof late[0]) late[nlate++] = (unsigned short)tag;
}
static const struct gn_sink sink = { sink_rx, sink_done, 0 };

/* netd's frames onto the send ring (each one piece in a send buffer of our region); without a link they go back at
 * once, counted as errors (TCP sends them again) */
static int send_pending(void)
{
    unsigned k = 0;
    int moved = 0;
    while (k < npend) {
        struct udrvframe *f = &pend[k];
        u32 a = f->piece[0].addr, n = f->piece[0].len;
        int r = f->npieces != 1 || a < dma || a + n > dma + REGION || n > GN_BUF ? -EINVAL : gn_tx(&g, f->tag, a, n);
        if (r == -ENOSPC) break;
        if (r < 0) { sink_done(0, f->tag); g.tx_err++; }
        k++;
        moved = 1;
    }
    if (k) {
        memmove(pend, pend + k, (npend - k) * sizeof pend[0]);
        npend -= k;
    }
    return moved;
}

/* the cache calls: the range opcodes when this Emu68 has them and a child survived them, else the kernel's */
static void choose_cache(const struct hwinfo *h, int force)
{
    cache_user = 0;
    if (force == 1) { gn_log(&g, "cache calls: the kernel's (cache=kernel)"); return; }
    if (!(h->flags & HW_RANGEOPS)) {
        gn_log(&g, "this Emu68 has no cache range opcodes: the kernel's cache calls");
        return;
    }
    if (force == 2) { cache_user = 1; gn_log(&g, "cache calls: Emu68's range opcodes from user mode (cache=user)"); return; }
    gn_log(&g, "trying Emu68's cache range opcodes in user mode in a child (if it is killed, vector 11: they need supervisor mode)");
    char *argv[3] = { "/sbin/net/genet", PROBE_ARG, 0 };
    int io[3] = { -1, 1, 2 };
    int pid = sys_spawn(argv[0], argv, io), st = pid < 0 ? pid : sys_wait(pid);
    if (st == 0) {
        cache_user = 1;
        gn_log(&g, "Emu68's cache range opcodes work in user mode");
    } else
        gn_log(&g, "the range opcodes failed in user mode (status %d): the kernel's cache calls", st);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], PROBE_ARG)) return probe();
    if (argc < 2) { dprintf(1, "usage: genet IFACE [cache=kernel|cache=user] [noselftest]\n"); return 2; }
    int force = 0, selftest = 1, r;
    g.ifn = argv[1];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "cache=kernel")) force = 1;
        else if (!strcmp(argv[i], "cache=user")) force = 2;
        else if (!strcmp(argv[i], "noselftest")) selftest = 0;
        else if (!strcmp(argv[i], "probe")) quiet = 1;
        else gn_log(&g, "unknown word in the driver line: %s (ignored)", argv[i]);
    }
    const struct hwinfo *h = drv_hwinfo();
    if (!h || !(h->flags & HW_DT) || !h->genet) {
        gn_log(&g, "no GENET in the hardware table (this is not a Pi 4 or CM4 under Emu68)");
        return 1;
    }
    quiet = 0;                                  /* the hardware table has one: from here everything is said */
    int claim = sys_drv(DRV_CLAIM, h->genet, 0);
    if (claim < 0) { gn_log(&g, "the claim of the GENET at %x was refused (%d)", h->genet, claim); return 1; }
    g.base = h->genet;
    if (gn_probe(&g)) return 1;
    struct udrvquiesce qz;                      /* the chip stopped when this driver or netd ends */
    gn_quiesce(&g, &qz);
    if ((r = sys_drv(DRV_QUIESCE, (u32)claim, (u32)&qz)) < 0) { gn_log(&g, "the quiesce program was refused (%d)", r); return 1; }
    memcpy(g.mac, h->mac, 6);
    if (!(g.mac[0] | g.mac[1] | g.mac[2] | g.mac[3] | g.mac[4] | g.mac[5])) {   /* none in the device tree: the chip's */
        u32 m0 = gn_rd(&g, UMAC_MAC0), m1 = gn_rd(&g, UMAC_MAC1);
        for (int i = 0; i < 4; i++) g.mac[i] = (unsigned char)(m0 >> (24 - 8 * i));
        g.mac[4] = (unsigned char)(m1 >> 8);
        g.mac[5] = (unsigned char)m1;
        if (!(m0 | (m1 & 0xffff)) || (g.mac[0] & 1)) { gn_log(&g, "no MAC address (neither the device tree nor the chip has one)"); return 1; }
        gn_log(&g, "the MAC address from the chip (the device tree had none)");
    }
    r = sys_drv(DRV_DMAMEM, (u32)claim, REGION);
    if (r < 0 || (r & 63)) { gn_log(&g, "no DMA memory (%d)", r); return 1; }
    dma = (u32)r;
    gn_reset(&g);                               /* whatever ran before is stopped first */
    if (gn_phy_start(&g, h->phy_addr == 0xff ? -1 : (int)h->phy_addr)) { gn_stop(&g); return 1; }
    choose_cache(h, force);
    if (selftest) {
        r = gn_selftest(&g, dma, REGION, sys_ticks() * 2654435761u);
        if (r == -EIO && cache_user) {
            gn_log(&g, "with the range opcodes from user mode the frames came back wrong: the kernel's cache calls");
            cache_user = 0;
            r = gn_selftest(&g, dma, REGION, sys_ticks() * 2654435761u + 1);
        }
        if (r == -EIO) {
            gn_log(&g, "the frames came back wrong with the kernel's cache calls too: the DMA cannot be trusted (noselftest skips this test)");
            gn_stop(&g);
            return 1;
        }
        if (r == -ETIMEDOUT) {
            gn_log(&g, "self-test: nothing came back through the loopback, nothing shown either way%s",
                   cache_user ? ": the kernel's cache calls, to be safe" : "");
            cache_user = 0;
        } else
            gn_log(&g, "self-test: %d frames through the loopback, checked byte by byte", ST_FRAMES);
    }
    gn_log(&g, "cache calls: %s", cache_user ? "Emu68's range opcodes from this process" : "the kernel's (DRV_CACHE)");
    r = drv_attach(&q, g.ifn, g.mac, 1500, GN_NRX, DRVA_DMA, claim, dma, REGION);
    if (r < 0) { gn_log(&g, "the attach was refused (%d)", r); gn_stop(&g); return 1; }
    if (q.at.bufsize != GN_BUF || q.at.nbuf < 1 || q.at.nbuf > GN_NRX) {
        gn_log(&g, "netd lent %d buffers of %d bytes: not what the chip is set for", (int)q.at.nbuf, (int)q.at.bufsize);
        gn_stop(&g);
        return 1;
    }
    g.nbuf = q.at.nbuf;
    for (u32 i = 0; i < g.nbuf; i++) {
        g.buf[i] = q.at.buf[i];
        if ((g.buf[i] & 63) || g.buf[i] < dma || g.buf[i] + GN_BUF > dma + REGION) {
            gn_log(&g, "netd's buffer %d is not in the DMA memory", (int)i);
            gn_stop(&g);
            return 1;
        }
        gn_give(&g, (int)i);
    }
    gn_rings(&g);
    gn_log(&g, "interface %d, %02x:%02x:%02x:%02x:%02x:%02x, %d receive buffers", r, g.mac[0], g.mac[1], g.mac[2],
           g.mac[3], g.mac[4], g.mac[5], (int)g.nbuf);
    gn_link_poll(&g, 1);
    for (int idle = 0;;) {
        if (drv_wait(idle > 2 ? 2 : idle ? 1 : 0) < 0) break;
        int moved = gn_link_poll(&g, 0);
        while (nlate && drv_done(&q, late[nlate - 1]) == 0) nlate--;
        if ((r = gn_tx_done(&g, &sink)) < 0 || (r = gn_rx(&g, &sink)) < 0) {
            gn_restart(&g, &sink);
            r = 1;
        }
        moved |= r > 0;
        moved |= send_pending();
        q.x.rx_drop = g.rx_drop; q.x.rx_bad = g.rx_bad; q.x.tx_err = g.tx_err;
        g.rx_drop = g.rx_bad = g.tx_err = 0;
        int nrx = (int)q.x.nrx;
        if ((r = drv_xfer(&q, g.link)) < 0) {
            if (r != -ECONNRESET && r != -EPERM) gn_log(&g, "the transfer was refused (%d)", r);
            break;                              /* -ECONNRESET, -EPERM: netd is gone */
        }
        for (u32 k = 0; k < q.x.nfree; k++) gn_give(&g, q.x.freed[k]);
        gn_refill(&g);
        for (u32 k = 0; k < q.x.ntx; k++) {
            if (npend < DRV_BATCH) pend[npend++] = q.x.tx[k];
            else { sink_done(0, q.x.tx[k].tag); g.tx_err++; }  /* cannot happen: netd lends at most 16 */
        }
        moved |= send_pending() | (q.x.ntx != 0) | (nrx != 0);
        idle = moved || q.x.more || npend || nlate ? 0 : idle + 1;
    }
    gn_stop(&g);
    gn_log(&g, "the doorbell closed (ifdown, or netd is gone): the chip stopped");
    return 0;
}
