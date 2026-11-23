/* SPDX-License-Identifier: GPL-2.0-only */
/* plipbox: Ethernet through a plipbox on the parallel port (DESIGN D39, D19 5.5), polled. netd starts it as
 * /sbin/net/plipbox IFACE [mac=XX:XX:XX:XX:XX:XX] [noburst]. The line protocol is plipbox-core.c (shared with the host
 * test against the model of the box, tools/plipbox-model.h); this file is the Zenix side: the claim of the port, the
 * quiesce program, the driver protocol with netd, the box going away and coming back.
 *
 * The kernel gives the port (CIA-A's port B, bits 0-2 of CIA-B's port A) to one driver, turns the ACK line's
 * interrupt on and counts its edges (DRV_PARACK). The driver looks at that count on every doorbell, at once again
 * while frames move, after 10 ms, then every 20 ms when idle. A transfer itself is the kernel's (DRV_PARXFER, DESIGN
 * D60 6.2) with full memory protection: the CIAs mix the parallel port's lines with the floppy's, the serial port's and the
 * keyboard's, so with full memory protection they are never a driver's; without it the driver runs the same loop
 * itself, plipbox-line.c, which the host test runs against the model. */
#include "drv.h"
#include "plipbox.h"

static int claim, kline;                /* kline: the kernel runs the line (full memory protection) */
static u32 pb_ticks(void) { return sys_ticks(); }
static long pb_acks(void) { return sys_drv(DRV_PARACK, (u32)claim, 0); }
static void pb_sleep(u32 t) { sys_sleep(t); }
struct pb;
static void pb_log(struct pb *b, const char *fmt, ...);
/* without full memory protection (no MMU: the stock A1200) the line runs here, as before D60: the same code, no
 * system call a transfer, and the kernel keeps none of it (its DRV_PARXFER is in the protection span) */
static unsigned char pb_rd(u32 a) { return *(volatile unsigned char *)a; }
static void pb_wr(u32 a, unsigned char v) { *(volatile unsigned char *)a = v; }
/* one instruction each: an interrupt cannot fall between the read and the write of CIA-B's port A (bits 3-7 are the
 * serial port's) */
static void pb_bset(u32 a, int bit) { __asm__ volatile("bset %1,(%0)" :: "a"(a), "d"(bit) : "memory", "cc"); }
static void pb_bclr(u32 a, int bit) { __asm__ volatile("bclr %1,(%0)" :: "a"(a), "d"(bit) : "memory", "cc"); }
static void pb_clrbits(u32 a, unsigned m) { __asm__ volatile("and.b %1,(%0)" :: "a"(a), "d"(~m) : "memory", "cc"); }
static int pb_yield(void) { return 0; }     /* user mode: the scheduler takes the CPU when it must */
#define PB_FN
#include "plipbox-line.c"
static int pb_line(struct pb *b, struct udrvparx *x, unsigned char *buf)
{
    (void)b;
    if (!kline)
        return pb_xfer(x, buf);
    x->buf = (u32)buf;
    return sys_drv(DRV_PARXFER, (u32)claim, (u32)x);
}

#include "plipbox-core.c"

/* "probe" in the driver line: netd's card probe (D19 6.3) started this driver for a card nobody configured; until the
 * card is there the driver says nothing (a machine without it hears nothing of it, as a Linux driver that finds no
 * device), then everything as always */
static int quiet;
static void pb_log(struct pb *b, const char *fmt, ...)
{
    if (quiet) return;
    char t[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    dprintf(1, "plipbox: %s: %s\n", b->ifn, t);        /* netd's standard output: the kernel log */
}

static struct pb b;
static struct drvq q;
static struct udrvframe pend[DRV_BATCH];        /* netd's frames not sent yet */
static unsigned npend;

/* netd's buffers: looked at, and only taken when a frame is in one (pb_poll) */
static int sink_take(void *ctx, unsigned char **p)
{
    (void)ctx;
    if (q.x.nrx >= DRV_BATCH || q.at.bufsize < PB_BUF)
        return -1;
    for (u32 i = 0; i < q.at.nbuf; i++)
        if (q.own[i]) { *p = (unsigned char *)q.at.buf[i]; return (int)i; }
    return -1;
}
static void sink_give(void *ctx, int num, unsigned len) { (void)ctx; q.own[num] = 0; drv_rx(&q, num, len); }
static const struct pb_sink sink = { sink_take, sink_give, 0 };

/* netd's frames to the box; each tag goes back when sent or failed (TCP sends a lost one again) */
static int send_pending(void)
{
    unsigned k = 0;
    while (k < npend && q.x.ndone < DRV_BATCH) {
        struct udrvframe *f = &pend[k++];
        if (b.down) {
            b.tx_err++;
        } else {
            int r = pb_send(&b, drv_gather(f, b.tx + 2, PB_MAXFRAME));
            if (r != -EINVAL)                   /* -EINVAL: shorter than a header (netd never sends one) */
                pb_result(&b, r, "sending");
            if (r) b.tx_err++;
            else b.tx_ok++;
        }
        drv_done(&q, f->tag);
    }
    if (k) {
        memmove(pend, pend + k, (npend - k) * sizeof pend[0]);
        npend -= k;
    }
    return k != 0;
}

static int parse_mac(const char *s, unsigned char m[6])
{
    for (int i = 0; i < 6; i++) {
        unsigned v = 0;
        for (int j = 0; j < 2; j++, s++) {
            int c = *s;
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return -1;
            v = v << 4 | (unsigned)d;
        }
        m[i] = (unsigned char)v;
        if (i < 5 && *s++ != ':') return -1;
    }
    return *s ? -1 : 0;
}

int main(int argc, char **argv)
{
    int adopt = 1;
    b.ifn = argc >= 2 ? argv[1] : "?";
    b.burst = 1;
    if (argc < 2) { dprintf(1, "usage: plipbox IFACE [mac=XX:XX:XX:XX:XX:XX] [noburst]\n"); return 2; }
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "mac=", 4)) {
            if (parse_mac(argv[i] + 4, b.mac) || !pb_mac_ok(b.mac)) {
                pb_log(&b, "not a usable MAC address: %s (the driver ends)", argv[i] + 4);
                return 1;
            }
            adopt = 0;
        } else if (!strcmp(argv[i], "noburst"))
            b.burst = 0;
        else if (!strcmp(argv[i], "probe"))
            quiet = 1;
        else
            pb_log(&b, "unknown word in the driver line: %s (ignored)", argv[i]);
    }
    kline = prot_full();                        /* D60 6.2: the CIAs are not ours, the kernel runs the line */
    claim = sys_drv(DRV_CLAIM, DRV_PARPORT, 0);
    if (claim == -EBUSY) { pb_log(&b, "the parallel port is another driver's"); return 1; }
    if (claim < 0) { pb_log(&b, "the claim of the parallel port was refused (%d)", claim); return 1; }
    struct udrvquiesce qz;
    pb_quiesce(&qz);                            /* before the first write to the port: a death leaves it as found */
    int r = sys_drv(DRV_QUIESCE, (u32)claim, (u32)&qz);
    if (r < 0) { pb_log(&b, "the quiesce program was refused (%d)", r); return 1; }
    if (pb_start(&b, adopt)) return 1;
    quiet = 0;                                  /* the box answered */
    pb_log(&b, "plipbox found (%s sends), MAC %02x:%02x:%02x:%02x:%02x:%02x%s", b.burst ? "burst" : "handshaked",
           b.mac[0], b.mac[1], b.mac[2], b.mac[3], b.mac[4], b.mac[5], adopt ? " (the box's own)" : "");
    r = drv_attach(&q, b.ifn, b.mac, 1500, 8, DRVA_LINK, claim, 0, 0);
    if (r < 0) { pb_log(&b, "the attach was refused (%d)", r); pb_stop(&b); return 1; }
    if (q.at.bufsize < PB_BUF) {
        pb_log(&b, "netd's buffers hold %d bytes: too small for a frame", (int)q.at.bufsize);
        pb_stop(&b);
        return 1;
    }
    pb_log(&b, "interface %d", r);
    u32 lastlook = sys_ticks();
    for (int idle = 0;;) {
        int nap = pb_idle_ticks(&b, idle);
        if (drv_wait(nap) < 0) break;
        pb_woke(&b, nap);
        int moved = 0;
        if (b.down && (u32)(sys_ticks() - lastlook) >= PB_RETRY) {
            lastlook = sys_ticks();
            pb_unstick(&b);
            if (!pb_hello(&b, 0)) {             /* the same MAC: netd's interface keeps its address */
                pb_result(&b, 0, "looking for the box");
                moved = 1;
            }
        }
        if ((r = pb_poll(&b, &sink, DRV_BATCH - (int)q.x.nrx)) < 0) {
            pb_log(&b, "the ACK count was refused (%d): the driver ends", r);
            break;
        }
        moved |= r > 0;
        moved |= send_pending();
        pb_keepalive(&b);
        q.x.rx_drop = b.rx_drop; q.x.rx_bad = b.rx_bad; q.x.tx_err = b.tx_err;
        b.rx_drop = b.rx_bad = b.tx_err = 0;
        int nrx = (int)q.x.nrx;
        if ((r = drv_xfer(&q, !b.down)) < 0) {
            if (r != -ECONNRESET && r != -EPERM) pb_log(&b, "the transfer was refused (%d)", r);
            break;                              /* -ECONNRESET, -EPERM: netd is gone */
        }
        for (u32 k = 0; k < q.x.ntx; k++) {
            if (npend < DRV_BATCH) pend[npend++] = q.x.tx[k];
            else drv_done(&q, q.x.tx[k].tag);   /* cannot happen: netd sends at most a batch */
        }
        moved |= send_pending() | (q.x.ntx != 0) | (nrx != 0);
        unsigned char *spare;
        long a = pb_acks();                     /* a request that came during the turn, and a buffer for it */
        int asks = a >= 0 && (u32)a != b.acks && !b.down && sink_take(0, &spare) >= 0;
        idle = moved || q.x.more || npend || q.x.ndone || asks ? 0 : idle + 1;
    }
    pb_report(&b);
    pb_stop(&b);
    pb_log(&b, "the doorbell closed (ifdown, or netd is gone): the box told, the port let go");
    return 0;
}
