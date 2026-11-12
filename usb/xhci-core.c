/* SPDX-License-Identifier: GPL-2.0-only */
/* The xHCI back end of usbd (DESIGN D46, docs/d46-usb.md 3.2, 3.3, 4): the controller's start, its rings, its
 * commands, its events, a device's slot and endpoints, control and interrupt transfers, the root ports. Polled, the
 * interrupter's enable left off. Plain C: the includer supplies
 *   u32 rg_r32(struct xhci *, u32 off), void rg_w32(struct xhci *, u32 off, u32 v)   the registers (little-endian)
 *   u32 dm_r32(u32 a), void dm_w32(u32 a, u32 v)            a 32-bit little-endian word of DMA memory (a bus address)
 *   void dm_rd(u32 a, void *p, u32 n), void dm_wr(u32 a, const void *p, u32 n)       bytes of DMA memory
 *   void dm_cache(struct xhci *, unsigned how, u32 a, u32 n)    XC_CLEAN or XC_INVAL (clean and invalidate)
 *   u32 xc_ticks(void), void xc_wait(void)                  the 100 Hz clock, and a pause of at most a tick
 *   void xc_log(struct xhci *, const char *fmt, ...)        the log
 * so the same file runs in usbd on Zenix and against the model on the PC (tools/xhci-model.h). Every wait has a
 * timeout and says what it waited for. Section numbers are xHCI 1.2's. */
#include "xhci.h"
#include "usb.h"

#define TO_HALT         5               /* ticks: 16 ms by 5.4.1, with slack */
#define TO_RESET        100             /* HCRST and CNR: 1 s */
#define TO_CMD          300             /* a command (Address Device waits for the device): 3 s */
#define TO_CTL          500             /* a control transfer: 5 s (USB 2.0 9.2.6.4: 5 s for a request with data) */
#define TO_PORT         50              /* a port reset: 500 ms */
#define TO_BULK         150             /* a bulk transfer: 1.5 s (a stick's 64 KB takes milliseconds; diskd waits 5 s for
                                         * a whole request, docs/d46-usb.md 3.5) */

static u32 op_r(struct xhci *x, u32 off) { return rg_r32(x, x->op + off); }
static void op_w(struct xhci *x, u32 off, u32 v) { rg_w32(x, x->op + off, v); }
static void op_w64(struct xhci *x, u32 off, u32 lo) { rg_w32(x, x->op + off, lo); rg_w32(x, x->op + off + 4, 0); }
static void rt_w64(struct xhci *x, u32 off, u32 lo) { rg_w32(x, x->rt + off, lo); rg_w32(x, x->rt + off + 4, 0); }

static int fail(struct xhci *x, const char *why)
{
    if (!x->dead) {
        x->dead = 1;
        x->why = why;
        xc_log(x, "stopped using the controller: %s", why);
    }
    return -EIO;
}

/* wait until (reg & mask) == want, at most lim ticks -> 0, or -ETIMEDOUT (said) */
static int xc_until(struct xhci *x, u32 off, u32 mask, u32 want, u32 lim, const char *what)
{
    u32 t0 = xc_ticks();
    for (;;) {
        if ((op_r(x, off) & mask) == want)
            return 0;
        if (xc_ticks() - t0 > lim) {
            xc_log(x, "timed out after %u ms waiting for %s", (unsigned)(lim * 10), what);
            return -ETIMEDOUT;
        }
        xc_wait();
    }
}

/* ---- the capabilities (5.3) ---- */

/* x->base and x->size set by the includer -> 0, or -ENODEV (said) */
static int xc_probe(struct xhci *x)
{
    u32 cap = rg_r32(x, XC_CAPLENGTH), ver = cap >> 16;
    if (cap == 0xFFFFFFFFu) {
        xc_log(x, "the registers read all ones: the USB block is not powered (otg_mode=1 in config.txt?)");
        return -ENODEV;
    }
    x->op = cap & 0xFF;
    u32 p1 = rg_r32(x, XC_HCSPARAMS1), p2 = rg_r32(x, XC_HCSPARAMS2), c1 = rg_r32(x, XC_HCCPARAMS1);
    x->db = rg_r32(x, XC_DBOFF) & ~3u;
    x->rt = rg_r32(x, XC_RTSOFF) & ~31u;
    x->maxslots = p1 & 0xFF;
    x->maxports = p1 >> 24;
    x->nscratch = (p2 >> 27) | ((p2 >> 21) & 31) << 5;
    x->csz = c1 & 4 ? 64 : 32;
    xc_log(x, "xHCI %x.%02x at %x: %u slots, %u ports, %u scratchpad pages, %u-byte contexts%s",
           (unsigned)(ver >> 8), (unsigned)(ver & 0xFF), (unsigned)x->base, x->maxslots, x->maxports, x->nscratch,
           x->csz, c1 & 1 ? ", 64-bit addresses" : "");
    if (x->op < 0x20 || ver < 0x0090 || ver > 0x0120 || !x->maxslots || !x->maxports || x->maxports > 31 ||
        x->db < 0x20 || x->rt < 0x20 || x->db + 4 * (XC_MAXDEV + 1) > x->size || x->rt + 0x40 > x->size ||
        x->op + XO_PORTSC(x->maxports) + 16 > x->size) {
        xc_log(x, "these values do not fit an xHCI in a window of %u bytes: not used", (unsigned)x->size);
        return -ENODEV;
    }
    if (x->nscratch > XC_SCRATCH_MAX) {
        xc_log(x, "%u scratchpad pages wanted, %u are kept for it: not used", x->nscratch, XC_SCRATCH_MAX);
        return -ENODEV;
    }
    return 0;
}

/* the DMA memory a controller needs: the structures' pages, then the 64 KB bounce buffer on a 64 KB boundary (so
 * 64 KB of slack for that alignment) */
static u32 xc_dma_size(struct xhci *x)
{
    return (u32)(5 + x->nscratch + 2 * XC_MAXDEV) * XC_PAGE + 2 * XC_BOUNCE;
}

/* ---- rings (4.9) ---- */

static void ring_init(struct xhci *x, struct xring *r, u32 base, unsigned n)
{
    r->base = base;
    r->n = n;
    r->enq = 0;
    r->cycle = 1;
    for (u32 a = base; a < base + n * 16; a += 4)
        dm_w32(a, 0);
    u32 link = base + (n - 1) * 16;     /* the link back to the start, toggling the cycle; its own cycle bit is
                                         * written when the producer reaches it */
    dm_w32(link, base);
    dm_w32(link + 12, TRB_TYPE(TT_LINK) | TRB_TC);
    dm_cache(x, XC_CLEAN, base, n * 16);
}

/* write a TRB at the enqueue point (the cycle bit last), past the link if there -> its bus address */
static u32 ring_put(struct xhci *x, struct xring *r, u32 p0, u32 p1, u32 st, u32 ctl)
{
    u32 a = r->base + r->enq * 16;
    dm_w32(a, p0);
    dm_w32(a + 4, p1);
    dm_w32(a + 8, st);
    dm_w32(a + 12, (ctl & ~TRB_C) | r->cycle);
    dm_cache(x, XC_CLEAN, a, 16);
    if (++r->enq == r->n - 1) {         /* the link: handed over with this cycle, then the cycle toggles */
        u32 l = r->base + r->enq * 16;
        dm_w32(l + 12, TRB_TYPE(TT_LINK) | TRB_TC | (ctl & TRB_CH) | r->cycle);
        dm_cache(x, XC_CLEAN, l, 16);
        r->enq = 0;
        r->cycle ^= 1;
    }
    return a;
}

static void doorbell(struct xhci *x, unsigned slot, unsigned target)
{
    rg_w32(x, x->db + 4 * slot, target);
}

/* ---- events (4.9.4) ---- */

static struct xc_ep *ep_of(struct xc_slot *s, unsigned dci)
{
    for (int i = 0; i < XC_MAXEP; i++)
        if (s->ep[i].dci == dci)
            return &s->ep[i];
    return 0;
}

static void event(struct xhci *x, u32 p0, u32 st, u32 ctl)
{
    unsigned type = TRB_GETTYPE(ctl), cc = st >> 24, sid = ctl >> 24, dci = (ctl >> 16) & 31;
    x->events++;
    if (type == TT_EV_CMD) {
        if (p0 == x->cmd_trb) {
            x->cmd_done = cc ? (int)cc : 255;  /* 0 is "invalid": a failure the callers see as not CC_SUCCESS */
            x->cmd_slot = sid;
        }
    } else if (type == TT_EV_TRANSFER) {
        struct xc_slot *s = sid >= 1 && sid <= XC_MAXDEV ? &x->slot[sid] : 0;
        if (!s || !s->used)
            return;
        if (cc != CC_SUCCESS && cc != CC_SHORT)
            x->errors++;
        if (dci == 1) {
            if (cc == CC_SHORT && p0 != s->ctl_status)
                s->ctl_left = st & 0xFFFFFF;    /* the data stage was short; the status stage follows */
            else if (p0 == s->ctl_status || (cc != CC_SUCCESS && cc != CC_SHORT))
                s->ctl_done = (int)cc;
        } else {
            struct xc_ep *e = ep_of(s, dci);
            if (!e || !e->busy)
                return;
            e->left = st & 0xFFFFFF;
            e->done = (int)cc;
            e->busy = 0;
            if (cc != CC_SUCCESS && cc != CC_SHORT)
                e->halted = 1;
        }
    } else if (type == TT_EV_PORT) {
        unsigned p = p0 >> 24;
        if (p >= 1 && p <= 31)
            x->port_change |= 1u << p;
    } else if (type == TT_EV_HOST)
        xc_log(x, "host controller event %u", cc);
}

/* read every event that is there -> how many */
static int xc_poll(struct xhci *x)
{
    int n = 0;
    if (x->dead)
        return 0;
    for (;;) {
        u32 a = x->ev_deq;
        dm_cache(x, XC_INVAL, a & ~63u, 64);    /* the controller wrote it: never the cached copy */
        u32 ctl = dm_r32(a + 12);
        if ((ctl & TRB_C) != x->ev_cycle)
            break;
        event(x, dm_r32(a), dm_r32(a + 8), ctl);
        n++;
        x->ev_deq += 16;
        if (x->ev_deq == x->ev + EV_TRBS * 16) {
            x->ev_deq = x->ev;
            x->ev_cycle ^= 1;
        }
    }
    if (n)
        rt_w64(x, XR_ERDP, x->ev_deq | 8);      /* EHB written 1: cleared */
    u32 sts = op_r(x, XO_USBSTS);
    if (sts & (STS_HSE | STS_HCE))
        fail(x, sts & STS_HSE ? "host system error (a DMA address the controller could not reach)" : "host controller error");
    return n;
}

/* ---- commands (4.6) ---- */

/* -> the completion code (CC_SUCCESS 1, ...), or -ETIMEDOUT / -EIO */
static int xc_command(struct xhci *x, u32 p0, u32 st, u32 ctl, const char *what)
{
    if (x->dead)
        return -EIO;
    x->cmd_done = 0;
    x->cmd_trb = ring_put(x, &x->cmd, p0, 0, st, ctl);
    x->commands++;
    doorbell(x, 0, 0);
    u32 t0 = xc_ticks();
    for (;;) {
        xc_poll(x);
        if (x->cmd_done)
            return x->cmd_done;
        if (x->dead)
            return -EIO;
        if (xc_ticks() - t0 > TO_CMD) {
            xc_log(x, "the command \"%s\" was not answered in %u ms", what, TO_CMD * 10);
            fail(x, "a command was not answered");
            return -ETIMEDOUT;
        }
        xc_wait();
    }
}

/* The self-test (docs/d46-usb.md 4.8): a No-Op command, with the event ring's line read into the cache before the
 * doorbell, so that a missing clean (the controller reads a stale TRB) or a missing invalidate (the CPU sees its own
 * stale copy) shows as no answer -> 0, or -ETIMEDOUT */
static int xc_selftest(struct xhci *x)
{
    x->cmd_done = 0;
    x->cmd_trb = ring_put(x, &x->cmd, 0, 0, 0, TRB_TYPE(TT_NOOP_CMD));
    (void)dm_r32(x->ev_deq + 12);       /* the stale line now in the CPU's cache */
    doorbell(x, 0, 0);
    u32 t0 = xc_ticks();
    while (xc_ticks() - t0 <= 10) {
        xc_poll(x);
        if (x->cmd_done)
            return x->cmd_done == CC_SUCCESS ? 0 : -EIO;
        xc_wait();
    }
    return -ETIMEDOUT;
}

/* ---- start and stop (4.2) ---- */

/* lays the structures out in [dma, dma + len) (len from xc_dma_size), resets and starts the controller -> 0 or -err */
static int xc_start(struct xhci *x, u32 dma, u32 len)
{
    u32 a = (dma + XC_PAGE - 1) & ~(u32)(XC_PAGE - 1);
    if (len < xc_dma_size(x))
        return -ENOMEM;
    x->dead = 0;
    x->why = 0;
    if (!(op_r(x, XO_USBSTS) & STS_HCH)) {      /* running (left by AmigaOS, by a usbd before, or by a failed self-test
                                                 * whose stale command may still run): halted before anything is
                                                 * written (docs/d46-usb.md 4.8) */
        xc_log(x, "the controller was running: halting it");
        op_w(x, XO_USBCMD, op_r(x, XO_USBCMD) & ~(CMD_RS | CMD_INTE));
        if (xc_until(x, XO_USBSTS, STS_HCH, STS_HCH, TO_HALT, "the controller to halt"))
            return fail(x, "it does not halt (its DMA may still run)");
    }
    x->dma = a;
    x->dmalen = len - (a - dma);
    x->dcbaa = a;                       /* page 0: the DCBAA, the segment table, the control and report buffers */
    x->erst = a + 2048;
    x->ctlbuf = a + 2560;               /* 512 bytes */
    x->cbw = a + 3072;                  /* a command block wrapper, 64 bytes */
    x->csw = a + 3136;                  /* a command status wrapper, 64 bytes */
    x->bounce = (a + (u32)(5 + x->nscratch + 2 * XC_MAXDEV) * XC_PAGE + XC_BOUNCE - 1) & ~(u32)(XC_BOUNCE - 1);
    ring_init(x, &x->cmd, a + XC_PAGE, CMD_TRBS);
    x->ev = x->ev_deq = a + 2 * XC_PAGE;
    x->ev_cycle = 1;
    x->spa = a + 3 * XC_PAGE;
    dm_cache(x, XC_INVAL, a, XC_PAGE);  /* the controller may have written there before (a restart): nothing of
                                         * it may be written back over what the CPU writes now */
    dm_cache(x, XC_INVAL, x->ev, XC_PAGE);
    for (u32 p = a; p < a + XC_PAGE; p += 4)
        dm_w32(p, 0);
    for (u32 p = x->ev; p < x->ev + XC_PAGE; p += 4)
        dm_w32(p, 0);
    dm_cache(x, XC_INVAL, a, XC_PAGE);
    dm_cache(x, XC_INVAL, x->ev, XC_PAGE);

    op_w(x, XO_USBCMD, CMD_HCRST);
    if (xc_until(x, XO_USBCMD, CMD_HCRST, 0, TO_RESET, "the reset to end") ||
        xc_until(x, XO_USBSTS, STS_CNR, 0, TO_RESET, "the controller to be ready"))
        return fail(x, "the reset did not end");
    if (!(op_r(x, XO_PAGESIZE) & 1))
        return fail(x, "its pages are not 4 KB");
    unsigned slots = x->maxslots < XC_MAXDEV ? x->maxslots : XC_MAXDEV;
    op_w(x, XO_CONFIG, slots);
    if (x->nscratch) {                  /* the scratchpad: an array of pages the controller owns (4.20) */
        for (unsigned i = 0; i < x->nscratch; i++) {
            u32 pg = x->spa + (1 + i) * XC_PAGE;
            dm_w32(x->spa + 8 * i, pg);
            dm_w32(x->spa + 8 * i + 4, 0);
            dm_cache(x, XC_INVAL, pg, XC_PAGE);
        }
        dm_cache(x, XC_CLEAN, x->spa, 8 * x->nscratch);
        dm_w32(x->dcbaa, x->spa);
    }
    dm_cache(x, XC_CLEAN, x->dcbaa, 8 * (XC_MAXDEV + 1));
    op_w64(x, XO_DCBAAP, x->dcbaa);
    op_w64(x, XO_CRCR, x->cmd.base | 1);        /* RCS 1 */
    dm_w32(x->erst, x->ev);             /* one segment of 256 TRBs */
    dm_w32(x->erst + 4, 0);
    dm_w32(x->erst + 8, EV_TRBS);
    dm_w32(x->erst + 12, 0);
    dm_cache(x, XC_CLEAN, x->erst, 16);
    rg_w32(x, x->rt + XR_ERSTSZ, 1);
    rt_w64(x, XR_ERDP, x->ev);
    rt_w64(x, XR_ERSTBA, x->erst);
    rg_w32(x, x->rt + XR_IMOD, 0);
    rg_w32(x, x->rt + XR_IMAN, IMAN_IP);        /* a pending interrupt cleared, IE off: polled */
    op_w(x, XO_USBCMD, CMD_RS);
    if (xc_until(x, XO_USBSTS, STS_HCH, 0, TO_HALT, "the controller to run"))
        return fail(x, "it does not start");
    for (unsigned p = 1; p <= x->maxports; p++) {       /* power on */
        u32 v = op_r(x, XO_PORTSC(p));
        if (!(v & PS_PP))
            op_w(x, XO_PORTSC(p), (v & PS_KEEP) | PS_PP);
    }
    x->port_change = 0;
    for (unsigned p = 1; p <= x->maxports; p++)         /* a device already there counts as a change */
        if (op_r(x, XO_PORTSC(p)) & (PS_CCS | PS_CSC))
            x->port_change |= 1u << p;
    return 0;
}

/* halt it (the quiesce program does the same when usbd ends) */
static void xc_stop(struct xhci *x)
{
    op_w(x, XO_USBCMD, 0);
    xc_until(x, XO_USBSTS, STS_HCH, STS_HCH, TO_HALT, "the controller to halt");
}

/* the quiesce program the kernel runs when usbd ends (D19 2.4, docs/d46-usb.md 3.6): Run/Stop and the interrupt enable
 * off, a pending interrupt cleared (IMAN's IP is write-1-to-clear) -> the number of steps, their offsets and values */
static int xc_quiesce(struct xhci *x, u32 *off, u32 *val, u32 *wait)
{
    off[0] = x->op + XO_USBCMD;         /* Run/Stop and INTE off */
    val[0] = 0;
    wait[0] = 0;
    off[1] = x->op + XO_USBSTS;         /* a wait (the kernel's DRVS_WAIT, at most 20 ms) until HCHalted */
    val[1] = STS_HCH;
    wait[1] = STS_HCH;                  /* the mask */
    off[2] = x->rt + XR_IMAN;           /* a pending interrupt cleared, IE off */
    val[2] = IMAN_IP;
    wait[2] = 0;
    return 3;
}

/* ---- root ports (4.19) ---- */

static u32 xc_portsc(struct xhci *x, unsigned p) { return op_r(x, XO_PORTSC(p)); }

/* clear a port's change bits (write 1 to them, keep the rest) */
static void xc_port_ack(struct xhci *x, unsigned p, u32 bits)
{
    u32 v = op_r(x, XO_PORTSC(p));
    op_w(x, XO_PORTSC(p), (v & PS_KEEP) | (bits & PS_CHANGES));
}

/* reset a USB 2.0 port -> its speed (USB_SPEED_*), or -err */
static int xc_port_reset(struct xhci *x, unsigned p)
{
    u32 v = op_r(x, XO_PORTSC(p));
    if (!(v & PS_CCS))
        return -ENODEV;
    op_w(x, XO_PORTSC(p), (v & PS_KEEP) | PS_PR);
    u32 t0 = xc_ticks();
    while (!((v = op_r(x, XO_PORTSC(p))) & PS_PRC)) {
        if (xc_ticks() - t0 > TO_PORT) {
            xc_log(x, "port %u: the reset did not end in %u ms", p, TO_PORT * 10);
            return -ETIMEDOUT;
        }
        xc_wait();
    }
    xc_port_ack(x, p, PS_PRC | PS_CSC | PS_PEC);
    if (!(v & PS_PED))
        return -EIO;
    return (int)PS_SPEED(v);
}

/* ---- slots and contexts (4.3, 6.2) ---- */

static u32 slot_page(struct xhci *x, unsigned sid) { return x->dma + (u32)(5 + x->nscratch + 2 * (sid - 1)) * XC_PAGE; }
static u32 ictx(struct xhci *x, unsigned sid, unsigned i) { return x->slot[sid].in + i * x->csz; }  /* 0 control, 1 slot, 2.. EPs */

static void ictx_clear(struct xhci *x, unsigned sid)
{
    for (u32 a = x->slot[sid].in; a < x->slot[sid].in + 33 * x->csz; a += 4)
        dm_w32(a, 0);
}

/* an EP's interval field from bInterval (6.2.3.6) */
static unsigned ival(unsigned speed, unsigned b)
{
    if (speed == USB_SPEED_HIGH)
        return b ? (b > 16 ? 15 : b - 1) : 0;
    unsigned v = 3;                     /* full/low speed: bInterval in ms, as 2^v x 125 us */
    while (v < 10 && (1u << (v + 1)) <= b * 8u)
        v++;
    return v;
}

/* a new device: Enable Slot, Address Device -> the slot id, or -err */
static int xc_dev_new(struct xhci *x, unsigned speed, unsigned rootport)
{
    int r = xc_command(x, 0, 0, TRB_TYPE(TT_ENABLE_SLOT), "Enable Slot");
    if (r != CC_SUCCESS) {
        if (r == CC_NO_SLOTS)
            xc_log(x, "no free device slot (%u devices at most)", XC_MAXDEV);
        return r < 0 ? r : -ENOSPC;
    }
    unsigned sid = x->cmd_slot;
    if (sid < 1 || sid > XC_MAXDEV || x->slot[sid].used) {
        xc_log(x, "the controller gave slot %u", sid);
        return -EIO;
    }
    struct xc_slot *s = &x->slot[sid];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->speed = (unsigned char)speed;
    s->port = (unsigned char)rootport;
    u32 pg = slot_page(x, sid);
    s->out = pg;
    s->in = pg + XC_PAGE;
    dm_cache(x, XC_INVAL, pg, 2048);    /* the last device's output context, written by the controller: dropped first */
    for (u32 a = pg; a < pg + 2048; a += 4)
        dm_w32(a, 0);
    dm_cache(x, XC_INVAL, pg, 2048);
    ring_init(x, &s->ep0, pg + 2048, RING_TRBS);
    s->ep[0].ring.base = pg + 3072;
    s->ep[1].ring.base = pg + XC_PAGE + 3072;
    dm_w32(x->dcbaa + 8 * sid, s->out);
    dm_w32(x->dcbaa + 8 * sid + 4, 0);
    dm_cache(x, XC_CLEAN, x->dcbaa + 8 * sid, 8);
    unsigned mps = speed == USB_SPEED_HIGH ? 64 : 8;
    ictx_clear(x, sid);
    dm_w32(ictx(x, sid, 0) + 4, 3);                             /* add the slot and EP0 */
    dm_w32(ictx(x, sid, 1), speed << 20 | 1u << 27);            /* route string 0 (the root port), context entries 1 */
    dm_w32(ictx(x, sid, 1) + 4, rootport << 16);
    dm_w32(ictx(x, sid, 2) + 4, 3u << 1 | 4u << 3 | mps << 16);  /* CErr 3, a control endpoint */
    dm_w32(ictx(x, sid, 2) + 8, s->ep0.base | 1);
    dm_w32(ictx(x, sid, 2) + 16, 8);
    dm_cache(x, XC_CLEAN, s->in, 33 * x->csz);
    r = xc_command(x, s->in, 0, TRB_TYPE(TT_ADDRESS_DEV) | sid << 24, "Address Device");
    if (r != CC_SUCCESS) {
        xc_log(x, "slot %u: Address Device failed (code %d)", sid, r);
        xc_command(x, 0, 0, TRB_TYPE(TT_DISABLE_SLOT) | sid << 24, "Disable Slot");
        s->used = 0;
        return r < 0 ? r : -EIO;
    }
    return (int)sid;
}

static void xc_dev_free(struct xhci *x, unsigned sid)
{
    if (sid < 1 || sid > XC_MAXDEV || !x->slot[sid].used)
        return;
    xc_command(x, 0, 0, TRB_TYPE(TT_DISABLE_SLOT) | sid << 24, "Disable Slot");
    dm_w32(x->dcbaa + 8 * sid, 0);
    dm_cache(x, XC_CLEAN, x->dcbaa + 8 * sid, 8);
    x->slot[sid].used = 0;
}

/* EP0's max packet size after the device descriptor said it (Evaluate Context) */
static int xc_set_mps0(struct xhci *x, unsigned sid, unsigned mps)
{
    ictx_clear(x, sid);
    dm_w32(ictx(x, sid, 0) + 4, 2);
    dm_w32(ictx(x, sid, 2) + 4, 3u << 1 | 4u << 3 | mps << 16);
    dm_w32(ictx(x, sid, 2) + 8, x->slot[sid].ep0.base | 1);
    dm_w32(ictx(x, sid, 2) + 16, 8);
    dm_cache(x, XC_CLEAN, x->slot[sid].in, 33 * x->csz);
    int r = xc_command(x, x->slot[sid].in, 0, TRB_TYPE(TT_EVAL_CTX) | sid << 24, "Evaluate Context");
    return r == CC_SUCCESS ? 0 : -EIO;
}

/* an interrupt (type 7: IN, 3: OUT) or bulk (6, 2) endpoint, as endpoint index i of the slot (Configure Endpoint)
 * -> 0 or -err */
static int xc_ep_open(struct xhci *x, unsigned sid, int i, unsigned epaddr, unsigned type, unsigned mps,
                      unsigned binterval)
{
    struct xc_slot *s = &x->slot[sid];
    unsigned dci = (epaddr & 15) * 2 + (epaddr & 0x80 ? 1 : 0);
    if (i < 0 || i >= XC_MAXEP || dci < 2 || !mps)
        return -EINVAL;
    struct xc_ep *e = &s->ep[i];
    int again = e->dci == dci;          /* open already: dropped and added in one command, which resets its data toggle
                                         * (xHCI 4.6.6; after the device's CLEAR_FEATURE(ENDPOINT_HALT), USB 2.0 9.4.5) */
    if (again)
        xc_command(x, 0, 0, TRB_TYPE(TT_STOP_EP) | dci << 16 | sid << 24, "Stop Endpoint");
    e->dci = (unsigned char)dci;
    e->halted = 0;
    e->busy = 0;
    ring_init(x, &e->ring, e->ring.base, RING_TRBS);
    dm_cache(x, XC_INVAL, s->out, 32 * x->csz);
    u32 s0 = dm_r32(s->out), s1 = dm_r32(s->out + 4);
    unsigned entries = s0 >> 27;
    for (int k = 0; k < XC_MAXEP; k++)
        if (s->ep[k].dci > entries)
            entries = s->ep[k].dci;
    ictx_clear(x, sid);
    dm_w32(ictx(x, sid, 0), again ? 1u << dci : 0);
    dm_w32(ictx(x, sid, 0) + 4, 1u | 1u << dci);
    s0 = (s0 & 0x07FFFFFFu) | entries << 27;
    dm_w32(ictx(x, sid, 1), s0);
    dm_w32(ictx(x, sid, 1) + 4, s1);
    u32 e0 = ival(s->speed, binterval) << 16;
    dm_w32(ictx(x, sid, 1 + dci), (type == 7 || type == 3) ? e0 : 0);
    dm_w32(ictx(x, sid, 1 + dci) + 4, 3u << 1 | type << 3 | mps << 16);
    dm_w32(ictx(x, sid, 1 + dci) + 8, e->ring.base | 1);
    dm_w32(ictx(x, sid, 1 + dci) + 16, (type == 7 || type == 3) ? (mps << 16 | mps) : 3072);
    dm_cache(x, XC_CLEAN, s->in, 33 * x->csz);
    int r = xc_command(x, s->in, 0, TRB_TYPE(TT_CONFIG_EP) | sid << 24, "Configure Endpoint");
    if (r != CC_SUCCESS) {
        e->dci = 0;
        xc_log(x, "slot %u: Configure Endpoint failed (code %d)", sid, r);
        return -EIO;
    }
    return 0;
}

/* a halted or stuck endpoint back to work: Reset Endpoint (when halted) or Stop Endpoint, then its dequeue pointer to
 * the ring's enqueue point (4.6.8, 4.6.10) */
static void ep_recover(struct xhci *x, unsigned sid, unsigned dci, struct xring *r, int halted)
{
    if (halted)
        xc_command(x, 0, 0, TRB_TYPE(TT_RESET_EP) | dci << 16 | sid << 24, "Reset Endpoint");
    else
        xc_command(x, 0, 0, TRB_TYPE(TT_STOP_EP) | dci << 16 | sid << 24, "Stop Endpoint");
    xc_command(x, (r->base + r->enq * 16) | r->cycle, 0, TRB_TYPE(TT_SET_DEQ) | dci << 16 | sid << 24, "Set TR Dequeue");
}

/* ---- transfers (4.11) ---- */

/* a control transfer on EP0: setup the 8 bytes, data in or out through the controller's control buffer (at most 512
 * bytes) -> bytes moved, -EPIPE (a stall), -ETIMEDOUT, -EIO */
static int xc_control(struct xhci *x, unsigned sid, const unsigned char *setup, void *data, unsigned len)
{
    struct xc_slot *s = sid >= 1 && sid <= XC_MAXDEV ? &x->slot[sid] : 0;
    if (!s || !s->used || len > 512 || x->dead)
        return -EINVAL;
    int in = setup[0] & 0x80;
    if (len) {
        if (!in)
            dm_wr(x->ctlbuf, data, len);
        dm_cache(x, in ? XC_INVAL : XC_CLEAN, x->ctlbuf, len);
    }
    u32 p0 = setup[0] | setup[1] << 8 | setup[2] << 16 | (u32)setup[3] << 24;
    u32 p1 = setup[4] | setup[5] << 8 | setup[6] << 16 | (u32)setup[7] << 24;
    s->ctl_done = 0;
    s->ctl_left = 0;
    ring_put(x, &s->ep0, p0, p1, 8, TRB_TYPE(TT_SETUP) | TRB_IDT | (u32)(len ? (in ? 3 : 2) : 0) << 16);
    if (len)
        ring_put(x, &s->ep0, x->ctlbuf, 0, len, TRB_TYPE(TT_DATA) | TRB_ISP | (in ? TRB_DIR_IN : 0));
    s->ctl_status = ring_put(x, &s->ep0, 0, 0, 0, TRB_TYPE(TT_STATUS) | TRB_IOC | (len && in ? 0 : TRB_DIR_IN));
    x->transfers++;
    doorbell(x, sid, 1);
    u32 t0 = xc_ticks();
    while (!s->ctl_done) {
        xc_poll(x);
        if (s->ctl_done)
            break;
        if (x->dead)
            return -EIO;
        if (xc_ticks() - t0 > TO_CTL) {
            xc_log(x, "slot %u: request %02x %02x not answered in %u ms", sid, setup[0], setup[1], TO_CTL * 10);
            ep_recover(x, sid, 1, &s->ep0, 0);
            return -ETIMEDOUT;
        }
        xc_wait();
    }
    if (s->ctl_done == CC_STALL) {
        ep_recover(x, sid, 1, &s->ep0, 1);
        return -EPIPE;
    }
    if (s->ctl_done != CC_SUCCESS && s->ctl_done != CC_SHORT) {
        ep_recover(x, sid, 1, &s->ep0, 1);
        return -EIO;
    }
    unsigned got = len > s->ctl_left ? len - s->ctl_left : 0;
    if (in && got) {
        dm_cache(x, XC_INVAL, x->ctlbuf, got);
        dm_rd(x->ctlbuf, data, got);
    }
    return (int)got;
}

/* one bulk transfer of len bytes (at most 64 KB, inside one 64 KB-aligned block) from or to buf, a bus address in the
 * DMA memory, on endpoint index i; waits for it -> the bytes moved, -EPIPE (a stall: the endpoint reset here, the
 * device's halt is the caller's), -ETIMEDOUT (stopped), -EIO */
static int xc_bulk(struct xhci *x, unsigned sid, int i, u32 buf, unsigned len)
{
    struct xc_slot *s = &x->slot[sid];
    struct xc_ep *e = &s->ep[i];
    if (x->dead || !s->used || !e->dci || len > 65536 || ((buf ^ (buf + len - 1)) & ~0xFFFFu))
        return -EINVAL;
    int in = e->dci & 1;
    dm_cache(x, in ? XC_INVAL : XC_CLEAN, buf, len);
    e->done = 0;
    e->busy = 1;
    e->left = 0;
    e->last = ring_put(x, &e->ring, buf, 0, len, TRB_TYPE(TT_NORMAL) | TRB_IOC | TRB_ISP);
    x->transfers++;
    doorbell(x, sid, e->dci);
    u32 t0 = xc_ticks();
    while (e->busy) {
        xc_poll(x);
        if (!e->busy)
            break;
        if (x->dead)
            return -EIO;
        if (xc_ticks() - t0 > TO_BULK) {
            xc_log(x, "slot %u: a bulk transfer of %u bytes not done in %u s", sid, len, TO_BULK / 100);
            e->busy = 0;
            ep_recover(x, sid, e->dci, &e->ring, 0);
            return -ETIMEDOUT;
        }
        xc_wait();
    }
    if (e->done != CC_SUCCESS && e->done != CC_SHORT) {
        int stall = e->done == CC_STALL;
        ep_recover(x, sid, e->dci, &e->ring, 1);
        e->halted = 0;
        return stall ? -EPIPE : -EIO;
    }
    unsigned got = len > e->left ? len - e->left : 0;
    if (in && got)
        dm_cache(x, XC_INVAL, buf, got);
    return (int)got;
}
