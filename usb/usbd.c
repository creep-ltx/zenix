/* SPDX-License-Identifier: GPL-2.0-only */
/* usbd: the USB server (DESIGN D46, docs/d46-usb.md; docs/man/usbd.txt). The D11 service of port "usb" (/etc/svctab),
 * kept running by `lsusb --hold` (/etc/daemons). It drives the BCM2711's xHCI USB 2.0 controller on the CM4's USB
 * port under Emu68 (the hardware table's xhci, written by stage0), polled, and serves a USB stick on it to the disk
 * server: diskd lists it as usb0 and its partitions (read-only), and asks for sectors by posts into usbd's window;
 * usbd reads them into its bounce buffer and posts the answer back (kernel/diskproto.h, struct urec). usbd never
 * waits for diskd, diskd waits for usbd at most 5 s a request.
 *
 * The kernel gives it what user mode cannot do safely (SYS_drv, D19 2.4; for the "usb" service only, and only the
 * xHCI's claim): the claim, DMA memory that belongs to the claim, a quiesce program (Run/Stop off, a wait for the
 * halt, the interrupt cleared) run when usbd ends, the cache calls. The ARM's data cache is not coherent with the
 * controller's DMA: usb/xhci-core.c cleans what it hands over and invalidates what it reads, with Emu68's range
 * opcodes from this process when a child survived them, else the kernel's DRV_CACHE; a No-Op command with a stale
 * cache line is the self-test of whichever way is chosen.
 *
 * With no controller (every machine but a CM4 or Pi 4 under Emu68 with otg_mode=1) it says so once in the kernel log,
 * answers what waits -ENODEV and ends with status 0: init and the service manager take that as "stopped". */
#include "../user/libz.h"
#include "../kernel/netproto.h"
#include "../kernel/hwinfo.h"
#include "../kernel/diskproto.h"
#include "usbproto.h"
#include "xhci.h"
#include "usb.h"

#define PROBE_ARG "--probe-rangeops"

static int cache_user, cache_fails, hold_n;
static struct umsg *holds[4];           /* lsusb --hold's calls, answered when usbd ends */

static u32 rg_r32(struct xhci *x, u32 off)
{
    u32 v = __builtin_bswap32(*(volatile u32 *)(x->base + off));
    __asm__ volatile("nop" ::: "memory");       /* Emu68 turns NOP into a barrier */
    return v;
}
static void rg_w32(struct xhci *x, u32 off, u32 v)
{
    *(volatile u32 *)(x->base + off) = __builtin_bswap32(v);
    __asm__ volatile("nop" ::: "memory");
}
static u32 dm_r32(u32 a) { return __builtin_bswap32(*(volatile u32 *)a); }
static void dm_w32(u32 a, u32 v) { *(volatile u32 *)a = __builtin_bswap32(v); }
static void dm_rd(u32 a, void *p, u32 n) { memcpy(p, (const void *)a, n); }
static void dm_wr(u32 a, const void *p, u32 n) { memcpy((void *)a, p, n); }
static u32 xc_ticks(void) { return sys_ticks(); }
static void xc_wait(void) { sys_sleep(1); }
static void xc_log(struct xhci *x, const char *fmt, ...)
{
    (void)x;
    char t[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    dprintf(1, "usbd: %s\n", t);                /* a service's output: the kernel log */
}
/* Emu68's range opcodes (base A0, length D1, whole 64-byte lines): clean, or clean and invalidate (never a bare
 * invalidate); the words net/drv/genet.c and the kernel's DRV_CACHE use */
static void range_op(unsigned how, u32 a, u32 n)
{
    u32 lo = a & ~63u, len = ((a + n + 63) & ~63u) - lo;
    if (how == XC_CLEAN)
        __asm__ volatile("move.l %0,%%a0\n\tmove.l %1,%%d1\n\t.word 0xF460,0x1C00" :: "r"(lo), "r"(len) : "a0", "d1", "memory");
    else
        __asm__ volatile("move.l %0,%%a0\n\tmove.l %1,%%d1\n\t.word 0xF460,0x1800" :: "r"(lo), "r"(len) : "a0", "d1", "memory");
}
static void dm_cache(struct xhci *x, unsigned how, u32 a, u32 n)
{
    if (cache_user) {
        range_op(how, a, n);
        return;
    }
    int r = sys_drv(DRV_CACHE | (how == XC_CLEAN ? DRVC_CLEAN : DRVC_INVAL) << 8, a, n);
    if (r < 0 && cache_fails++ < 4)
        xc_log(x, "the kernel refused a cache call at %x (%u bytes): %d", (unsigned)a, (unsigned)n, r);
}

#include "xhci-core.c"
#include "core.c"

/* ---- diskd (DESIGN D46 3.5): our window of its requests, posts into its window ---- */
static struct xhci X;
static struct usb U;
static int port = -1, dport = -1, dhello;   /* our port, diskd's, whether diskd took our hello */
static struct urec win[UREC_N];         /* diskd's requests (our window) */
static u32 din, dout;                   /* the last request taken; the last record posted */

/* the hello: diskd opens its window for us and forgets what an earlier usbd told it -> 0, or -err (said once) */
static int hello(void)
{
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = DISK_USBHELLO;
    m.a[0] = (u32)port;
    din = dout = 0;
    memset(win, 0, sizeof win);
    int r = sys_call(dport, &m);
    if (r < 0 && dhello >= 0)
        xc_log(&X, "the disk server did not take the hello (%d): sticks are not served", r);
    dhello = r < 0 ? -1 : 1;
    return r;
}

static int post1(struct urec *q)
{
    q->seq = ++dout;
    struct upost w = { (q->seq % UREC_N) * sizeof *q, (u32)q, sizeof *q };
    return sys_post(POST_WRITE, dport, &w);
}

static void announce(struct usb_dev *d, int add)
{
    struct urec q = { add ? UR_ADD : UR_DEL, 0, 0, add ? d->sectors : 0, add ? MSC_MAXSECT : 0, 0,
                      add ? X.bounce : 0, add ? (u32)d->label : 0 };
    post1(&q);
}

static void requests(void);

/* diskd reads posts only when it wakes for a request: after news, a request that answers at once (a DISK_LIST past
 * the end) wakes it. diskd may be waiting for one of our answers meanwhile: what it asked is answered first */
static void wake(void)
{
    requests();
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = DISK_LIST;
    m.a[0] = 0xFFFFFFFFu;
    sys_call(dport, &m);
}

/* a record to diskd; a diskd that started again (its window gone) gets the hello and what it must know first */
static void post_diskd(struct urec *q)
{
    if (dhello > 0 && post1(q) >= 0)
        return;
    if (hello() < 0)
        return;
    for (int i = 0; i < USB_MAXDEV; i++)
        if (U.dev[i].used && U.dev[i].ready)
            announce(&U.dev[i], 1);
    if (q->kind == UR_DONE)
        post1(q);
    wake();
}

/* core.c's news: a stick came or went */
static void usb_news(struct usb_dev *d, int add)
{
    struct urec q = { add ? UR_ADD : UR_DEL, 0, 0, add ? d->sectors : 0, add ? MSC_MAXSECT : 0, 0,
                      add ? X.bounce : 0, add ? (u32)d->label : 0 };
    if (dhello <= 0 && hello() < 0)
        return;
    post_diskd(&q);
    wake();
}

/* diskd's requests: each read into the bounce buffer, then its answer */
static void requests(void)
{
    for (u32 k = 0; k < UREC_N; k++) {
        struct urec *q = &win[(din + 1) % UREC_N];
        if (q->seq != din + 1)
            return;
        din++;
        int r = -EINVAL;
        struct usb_dev *d = 0;
        for (int i = 0; i < USB_MAXDEV; i++)
            if (U.dev[i].used && U.dev[i].ready)
                d = &U.dev[i];
        u32 t0 = xc_ticks();
        if (q->kind == UR_READ)
            r = !d || q->unit ? -ENODEV : msc_read(&U, d, q->lba, q->count);
        struct urec a = { UR_DONE, 0, 0, 0, 0, r, q->seq, 0 };
        post_diskd(&a);
        if (d && d->ready && xc_ticks() - t0 > 450) {       /* past diskd's 5 s: it has dropped the stick, so tell it
                                                             * again (a mount of it is stale) */
            xc_log(&X, "storage: a read took %u ms: the stick is told to the disk server again", (unsigned)((xc_ticks() - t0) * 10));
            announce(d, 0);
            announce(d, 1);
            wake();
        }
    }
}

/* the child that tries the range opcodes in user mode: it ends 0, or the kernel kills it (vector 11) */
static int probe(void)
{
    static unsigned char b[128] __attribute__((aligned(64)));
    memset(b, 0x5a, sizeof b);
    range_op(XC_CLEAN, (u32)b, 64);
    range_op(XC_INVAL, (u32)b, 64);
    return b[0] == 0x5a && b[63] == 0x5a ? 0 : 1;
}

static int peer_ok(struct umsg *m, int op, u32 a, u32 n)
{
    struct upeermem pm = { a, n };
    return n && sys_peer(m, op, &pm) == 0;
}

static struct usb_dev *nth(u32 i)
{
    for (int k = 0; k < USB_MAXDEV; k++)
        if (U.dev[k].used && !i--)
            return &U.dev[k];
    return 0;
}

/* a request -> its answer; 1 when it is kept (USB_HOLD) */
static int request(struct umsg *m, int *ans, int nodev)
{
    u32 a1 = m->a[1], a2 = m->a[2];
    switch (m->type) {
    case USB_HOLD:
        if (m->uid != 0) { *ans = -EPERM; return 0; }
        if (nodev || hold_n >= 4) { *ans = nodev ? -ENODEV : -EBUSY; return 0; }
        holds[hold_n++] = m;
        return 1;
    case USB_LIST: {
        struct usb_dev *d = nodev ? 0 : nth(m->a[0]);
        if (nodev) { *ans = -ENODEV; return 0; }
        if (!d) { *ans = -ENOENT; return 0; }
        if (a2 < USBDEVINFO_FIRST || !peer_ok(m, PEER_WR, a1, a2)) { *ans = -EFAULT; return 0; }
        struct usbdevinfo i;
        memset(&i, 0, sizeof i);
        i.size = sizeof i;
        i.slot = d->slot; i.speed = d->speed; i.port = d->port; i.parent = 0xFF;
        i.kind = d->kind; i.sectors = d->sectors;
        memcpy(i.label, d->label, sizeof i.label);
        memcpy(i.dd, d->dd, 18);
        i.cfglen = (unsigned short)d->cfglen;
        memcpy((void *)a1, &i, a2 < sizeof i ? a2 : sizeof i);
        *ans = 0;
        return 0;
    }
    case USB_DESC: {
        struct usb_dev *d = nodev ? 0 : nth(m->a[0]);
        if (nodev) { *ans = -ENODEV; return 0; }
        if (!d) { *ans = -ENOENT; return 0; }
        u32 n = a2 < d->cfglen ? a2 : d->cfglen;
        if (n && !peer_ok(m, PEER_WR, a1, n)) { *ans = -EFAULT; return 0; }
        memcpy((void *)a1, d->cfg, n);
        *ans = (int)n;
        return 0;
    }
    case USB_STAT: {
        if (a2 < USBSTAT_FIRST || !peer_ok(m, PEER_WR, a1, a2)) { *ans = -EFAULT; return 0; }
        struct usbstat s;
        memset(&s, 0, sizeof s);
        s.size = sizeof s;
        s.events = X.events; s.commands = X.commands; s.transfers = X.transfers; s.errors = X.errors;
        s.refused = U.nrefused;
        for (int k = 0; k < USB_MAXDEV; k++) s.devices += U.dev[k].used;
        s.cache_user = (u32)cache_user;
        memcpy((void *)a1, &s, a2 < sizeof s ? a2 : sizeof s);
        *ans = nodev ? -ENODEV : 0;
        return 0;
    }
    }
    *ans = -EINVAL;
    return 0;
}

/* the requests waiting now (wait: ticks to wait for the first: the call that started us may queue just after the port
 * was taken) */
static void serve(int nodev, int wait)
{
    for (;;) {
        struct umsg *m = sys_recv(port, wait ? wait + 1 : 0);
        if (!m || (int)m < 0)
            return;
        int ans = 0;
        if (!request(m, &ans, nodev))
            sys_reply(m, ans);
    }
}

/* nothing to drive: what waits is answered -ENODEV, then a stop (status 0) */
static int nothing(const char *why)
{
    dprintf(1, "usbd: %s: stopped until the next boot\n", why);
    if (port >= 0)
        serve(1, 50);
    return 0;
}

/* the cache calls: the range opcodes when this Emu68 has them and a child survived them, else the kernel's */
static void choose_cache(const struct hwinfo *h, int force)
{
    cache_user = 0;
    if (force == 1 || !(h->flags & HW_RANGEOPS)) {
        xc_log(&X, "cache calls: the kernel's (%s)", force == 1 ? "cache=kernel" : "this Emu68 has no range opcodes");
        return;
    }
    if (force == 2) { cache_user = 1; xc_log(&X, "cache calls: Emu68's range opcodes (cache=user)"); return; }
    char *argv[3] = { "/sbin/usbd", PROBE_ARG, 0 };
    int io[3] = { -1, 1, 2 };
    int pid = sys_spawn(argv[0], argv, io), st = pid < 0 ? pid : sys_wait(pid);
    cache_user = st == 0;
    xc_log(&X, st == 0 ? "Emu68's cache range opcodes work in user mode" :
                         "the range opcodes failed in user mode (status %d): the kernel's cache calls", st);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], PROBE_ARG)) return probe();
    int force = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "cache=kernel")) force = 1;
        else if (!strcmp(argv[i], "cache=user")) force = 2;
        else dprintf(1, "usbd: unknown argument %s (ignored)\n", argv[i]);
    }
    port = sys_port_create(USB_PORT);           /* first: the kernel knows the service's process by its port */
    if (port < 0) { dprintf(1, "usbd: port usb busy (%d)\n", port); return 1; }
    static struct hwinfo h;
    int r = sys_drv(DRV_HWINFO, (u32)&h, sizeof h);
    if (r == -EPERM || r == -ECONNRESET) { dprintf(1, "usbd: not started by the service manager (the kernel's device calls are refused)\n"); return 1; }
    if (r < HWINFO_FIRST || !(h.flags & HW_DT) || !h.xhci)
        return nothing(r >= HWINFO_FIRST && (h.flags & HW_DT) ?
                       "no xHCI in the device tree, or it is disabled (otg_mode=1 in config.txt)" :
                       "no USB controller (not a Pi 4 or CM4 under Emu68)");
    if (h.xhci_stop & XSTOP_SLOW)
        return nothing("the controller AmigaOS left running did not halt at the boot (its DMA may still run)");
    int claim = sys_drv(DRV_CLAIM, h.xhci, 0);
    if (claim < 0) { dprintf(1, "usbd: the claim of the xHCI at %x was refused (%d)\n", (unsigned)h.xhci, claim); return 1; }
    X.base = h.xhci;
    X.size = h.xhci_size;
    if (xc_probe(&X))
        return nothing("the controller cannot be used");
    struct udrvquiesce q;
    memset(&q, 0, sizeof q);
    u32 off[4], val[4], wait[4];
    q.n = (u32)xc_quiesce(&X, off, val, wait);
    for (u32 i = 0; i < q.n; i++) {             /* the registers are little-endian: values and masks as the 68k sees them */
        q.step[i].op = wait[i] ? DRVS_WAIT : DRVS_WRITE;
        q.step[i].size = 4;
        q.step[i].addr = X.base + off[i];
        q.step[i].value = __builtin_bswap32(val[i]);
        q.step[i].mask = __builtin_bswap32(wait[i]);
    }
    if ((r = sys_drv(DRV_QUIESCE, (u32)claim, (u32)&q)) < 0) { dprintf(1, "usbd: the quiesce program was refused (%d)\n", r); return 1; }
    u32 len = xc_dma_size(&X);
    r = sys_drv(DRV_DMAMEM, (u32)claim, len);
    if (r < 0 || (r & 63)) { dprintf(1, "usbd: no DMA memory (%d bytes: %d)\n", (int)len, r); return 1; }
    u32 dma = (u32)r;
    choose_cache(&h, force);
    if ((r = xc_start(&X, dma, len)))
        return nothing("the controller did not start");
    r = xc_selftest(&X);
    if (r && cache_user) {
        xc_log(&X, "self-test: no answer with the range opcodes from user mode: the kernel's cache calls");
        cache_user = 0;
        if (xc_start(&X, dma, len) == 0)
            r = xc_selftest(&X);
    }
    if (r) {
        xc_stop(&X);
        return nothing("self-test: the controller did not answer a No-Op command (the DMA or the cache calls do not work)");
    }
    xc_log(&X, "self-test: a No-Op command answered; cache calls: %s",
           cache_user ? "Emu68's range opcodes from usbd" : "the kernel's (DRV_CACHE)");
    U.x = &X;
    dport = sys_port_find("disk");
    struct upostbuf pb = { (u32)win, sizeof win, dport };
    if (dport < 0 || sys_post(POST_BUF, port, &pb) < 0)
        xc_log(&X, "no window for the disk server's requests: sticks are not served");
    else
        hello();                        /* a diskd that knew an earlier usbd's stick forgets it */
    for (u32 beat = xc_ticks();;) {
        struct pollfd pf = { port, POLLPORT | POLLIN | POLLRDBAND, 0 };
        if (sys_evwait(&pf, 1, 10) < 0)
            break;                      /* killed */
        serve(0, 0);
        requests();
        usb_poll(&U);
        if (X.dead)
            break;
        if (dhello > 0 && xc_ticks() - beat >= 100) {   /* once a second: a diskd that started again has no window for
                                                         * us (a post of nothing fails): the hello, the stick again */
            beat = xc_ticks();
            struct upost w = { 0, 0, 0 };
            if (sys_post(POST_WRITE, dport, &w) < 0) {
                struct urec q = { UR_DEL, 0, 1, 0, 0, 0, 0, 0 };    /* unit 1: nothing; only the hello's side effects */
                dhello = 0;
                post_diskd(&q);
            }
        }
    }
    usb_stop(&U);
    for (int i = 0; i < hold_n; i++)
        sys_reply(holds[i], -ECONNRESET);
    dprintf(1, "usbd: stopped (%s)\n", X.dead ? X.why : "killed");
    return X.dead ? 1 : 0;
}
