/* SPDX-License-Identifier: GPL-2.0-only */
/* fake: the network driver protocol's test driver (DESIGN D19 8.2, "the driver protocol before any card"; test images
 * only: tools/drv-test.sh puts it in /sbin/net). It has no wire: every frame netd sends it answers itself as if a
 * host were there, an ARP request for any address but the interface's own (with the MAC 02:00:00:00:00:fe) and an
 * ICMP echo request (the reply). So ping through it walks the whole path: netd's ARP and IP, the doorbell, the
 * batches both ways, the buffers lent and given back.
 *   /sbin/net/fake IFACE [dma] [bad]
 *     dma   claims the A2065 board of the hardware table (FS-UAE's, so the claim has a device; it never touches it but
 *           for the quiesce program), takes DMA memory and attaches with DRVA_DMA (netd's buffers in that memory);
 *           first it checks the kernel's refusals and prints one line: "fake: IFACE: kernel checks PASS (...)"
 *     bad   its first transfer carries a buffer that is not its own, a wrong length and a tag it does not hold:
 *           netd must refuse each (and log it), and go on */
#include "drv.h"

static const char *ifn = "fake";
static const unsigned char me[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const unsigned char peer[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xfe };
static struct drvq q;
static unsigned char frame[1600];
static unsigned long answered;

static unsigned short sum16(const unsigned char *p, unsigned n, u32 acc)
{
    for (; n > 1; n -= 2, p += 2) acc += (u32)p[0] << 8 | p[1];
    if (n) acc += (u32)p[0] << 8;
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (unsigned short)acc;
}

/* the answer to a frame netd sent, into one of our buffers; 0 if it needs none */
static int answer(const unsigned char *f, unsigned n)
{
    int b;
    unsigned char *o;
    unsigned len = 0;
    if (n < 14 || q.x.nrx >= DRV_BATCH) return 0;              /* no room in this batch: dropped, as a wire would */
    if (f[12] == 0x08 && f[13] == 0x06 && n >= 42 && f[20] == 0 && f[21] == 1) {    /* an ARP request */
        if (!memcmp(f + 28, f + 38, 4)) return 0;               /* its own address (a gratuitous ARP) */
        if ((b = drv_take(&q)) < 0) return 0;
        o = (unsigned char *)q.at.buf[b];
        memcpy(o, f + 6, 6);
        memcpy(o + 6, peer, 6);
        memcpy(o + 12, f + 12, 8);
        o[20] = 0; o[21] = 2;
        memcpy(o + 22, peer, 6);
        memcpy(o + 28, f + 38, 4);
        memcpy(o + 32, f + 22, 10);
        len = 42;
    } else if (f[12] == 0x08 && f[13] == 0x00 && n >= 34 + 8 && f[23] == 1 && f[34] == 8) {  /* an ICMP echo request */
        unsigned hl = (f[14] & 15) * 4u, tl = (unsigned)f[16] << 8 | f[17];
        if (tl + 14 > n || hl < 20 || tl < hl + 8) return 0;
        if ((b = drv_take(&q)) < 0) return 0;
        o = (unsigned char *)q.at.buf[b];
        memcpy(o, f + 6, 6);
        memcpy(o + 6, peer, 6);
        memcpy(o + 12, f + 12, 2 + tl);
        memcpy(o + 26, f + 30, 4);                              /* the addresses swapped: the header sum stays */
        memcpy(o + 30, f + 26, 4);
        unsigned char *ic = o + 14 + hl;
        ic[0] = 0;                                              /* echo reply */
        ic[2] = ic[3] = 0;
        unsigned short s = (unsigned short)~sum16(ic, tl - hl, 0);
        ic[2] = (unsigned char)(s >> 8);
        ic[3] = (unsigned char)s;
        len = 14 + tl;
    } else
        return 0;
    if (q.at.flags & DRVA_DMA)
        sys_drv(DRV_CACHE | DRVC_INVAL << 8, q.at.buf[b], len);   /* as a device that wrote it would (Emu68's cache) */
    drv_rx(&q, b, len < 60 ? 60 : len);                         /* Ethernet's shortest frame */
    answered++;
    return 1;
}

/* the kernel's refusals (dma: the caller holds a claim) */
static void checks(int claim, u32 base, u32 dma, u32 dmalen)
{
    char line[260];
    struct udrvquiesce bad, wrap;
    memset(&bad, 0, sizeof bad);
    bad.n = 1;
    bad.step[0].op = DRVS_WRITE;
    bad.step[0].size = 2;
    bad.step[0].addr = base - 2;                                /* outside the claimed window */
    int again = sys_drv(DRV_CLAIM, base, 0);
    int dma2 = sys_drv(DRV_DMAMEM, (u32)claim, 4096);
    int qout = sys_drv(DRV_QUIESCE, (u32)claim, (u32)&bad);
    wrap = bad;                                                 /* audit 2026-10-28 S5: addr + size wraps to 0 */
    wrap.step[0].size = 4;
    wrap.step[0].addr = 0xFFFFFFFCu;
    int qwrap = sys_drv(DRV_QUIESCE, (u32)claim, (u32)&wrap);
    int cin = sys_drv(DRV_CACHE | DRVC_CLEAN << 8, dma + 100, 200);
    int cout = sys_drv(DRV_CACHE | DRVC_CLEAN << 8, dma + dmalen - 8, 64);
    int other = sys_drv(DRV_DMAMEM, (u32)claim + 1, 4096);
    struct hwinfo h;
    int hsmall = sys_drv(DRV_HWINFO, (u32)&h, 8);
    struct udrvxfer x;
    memset(&x, 0, sizeof x);
    x.size = sizeof x;
    int early = sys_drv(DRV_XFER, (u32)&x, sizeof x);
    int ok = again == -EBUSY && dma2 == -EBUSY && qout == -EINVAL && qwrap == -EINVAL && !cin && cout == -EFAULT && other == -EPERM &&
             hsmall == -EINVAL && early == -ENOTCONN;
    snprintf(line, sizeof line, "kernel checks %s (a second claim %d, a second DMA region %d, a quiesce step outside "
             "the window %d, one that wraps %d, cache inside %d, outside %d, a claim not ours %d, a short hardware table %d, "
             "a transfer before the attach %d)", ok ? "PASS" : "FAIL", again, dma2, qout, qwrap, cin, cout, other, hsmall,
             early);
    drv_out(ifn, line);
}

int main(int argc, char **argv)
{
    int dma = 0, bad = 0, claim = 0;
    u32 region = 0, rlen = 0;
    char line[160];
    if (argc < 2) { dprintf(1, "usage: fake IFACE [dma] [bad]\n"); return 2; }
    ifn = argv[1];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "dma")) dma = 1;
        else if (!strcmp(argv[i], "bad")) bad = 1;
    }
    if (dma) {
        const struct hwboard *bd = drv_board(drv_hwinfo(), 0x202, 112);
        if (!bd) { drv_out(ifn, "no A2065 board to claim (dma needs a device)"); return 1; }
        if ((claim = sys_drv(DRV_CLAIM, bd->addr, 0)) < 0) { drv_out(ifn, "the claim was refused"); return 1; }
        struct udrvquiesce qz;                                  /* the LANCE stopped: RAP 0, then STOP in CSR0 */
        memset(&qz, 0, sizeof qz);
        qz.n = 2;
        qz.step[0].op = qz.step[1].op = DRVS_WRITE;
        qz.step[0].size = qz.step[1].size = 2;
        qz.step[0].addr = bd->addr + 0x4002;
        qz.step[0].value = 0;
        qz.step[1].addr = bd->addr + 0x4000;
        qz.step[1].value = 4;
        rlen = (8 + DRV_BATCH) * 1536;
        int r = sys_drv(DRV_DMAMEM, (u32)claim, rlen);
        if (r < 0 || sys_drv(DRV_QUIESCE, (u32)claim, (u32)&qz) < 0) { drv_out(ifn, "no DMA memory"); return 1; }
        region = (u32)r;
        checks(claim, bd->addr, region, rlen);
    }
    int r = drv_attach(&q, ifn, me, 1500, 8, DRVA_LINK | (dma ? DRVA_DMA : 0), claim, region, rlen);
    if (r < 0) {
        snprintf(line, sizeof line, "the attach was refused: %s", zstrerror(r));
        drv_out(ifn, line);
        return 1;
    }
    snprintf(line, sizeof line, "attached as interface %d, %u buffers of %u bytes%s", r, q.at.nbuf, q.at.bufsize,
             dma ? " in its DMA memory" : "");
    drv_out(ifn, line);
    if (bad) {                                                  /* three wrong numbers, then the work as usual */
        q.x.rx[0].num = 40; q.x.rx[0].len = 60;                 /* not a buffer */
        int b = drv_take(&q);
        q.x.rx[1].num = (unsigned short)b; q.x.rx[1].len = 9000; /* too long */
        q.x.nrx = 2;
        q.x.done[0] = 77;                                       /* a tag netd never gave */
        q.x.ndone = 1;
    }
    for (int wait = 0;;) {
        if (drv_wait(wait) < 0) break;
        if ((r = drv_xfer(&q, 1)) < 0) {
            if (r == -ECONNRESET || r == -EPERM) break;         /* netd is gone (a new one is not our parent) */
            snprintf(line, sizeof line, "the transfer was refused: %s", zstrerror(r));
            drv_out(ifn, line);
            return 1;
        }
        for (u32 k = 0; k < q.x.ntx; k++) {
            const struct udrvframe *f = &q.x.tx[k];
            if (dma)
                for (u32 j = 0; j < f->npieces && j < 4; j++)
                    sys_drv(DRV_CACHE | DRVC_CLEAN << 8, f->piece[j].addr, f->piece[j].len);
            unsigned n = drv_gather(f, frame, sizeof frame);
            answer(frame, n);
            drv_done(&q, f->tag);
        }
        wait = q.x.more || q.x.nrx || q.x.ndone ? 0 : 10;
    }
    snprintf(line, sizeof line, "netd is gone: stopping (%lu frames answered)", answered);
    drv_out(ifn, line);
    return 0;
}
