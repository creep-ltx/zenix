/* SPDX-License-Identifier: GPL-2.0-only */
/* The USB core of usbd (DESIGN D46, docs/d46-usb.md 3.1, 3.5): the root ports, enumeration, and USB sticks (mass
 * storage, bulk-only transport, SCSI's transparent command set), on top of the host controller's calls
 * (usb/xhci-core.c: xc_dev_new, xc_control, xc_ep_open, xc_bulk, the root ports; a clock-port chip's back end would
 * give the same calls). Everything a device says is untrusted: every length is checked against what was read, a
 * device that says something impossible is logged and left alone (and not tried again until it is plugged in again).
 * From USB 2.0 chapter 9, the Mass Storage Class Bulk-Only Transport 1.0 and SCSI's SBC/SPC (INQUIRY, TEST UNIT READY,
 * REQUEST SENSE, READ CAPACITY (10) and (16), READ (10)). The includer supplies xhci-core.c's functions and
 * usb_news(), which tells diskd a stick came or went. */
#include "usb.h"
#ifndef EPROTO
#define EPROTO 71                       /* (Linux's number) the transport broke: kept inside usbd, never in a message */
#endif

struct usb {
    struct xhci *x;
    struct usb_dev dev[USB_MAXDEV];
    u32 refused;                        /* root ports whose device was left alone, until it is pulled (bit p) */
    u32 settle[32];                     /* a root port's connect time + 100 ms (USB 2.0 7.1.7.3), 0: none waiting */
    unsigned nrefused;                  /* devices left alone (USB_STAT) */
};

static unsigned char cfgbuf[USB_MAXDEV][USB_CFGMAX];

static void usb_news(struct usb_dev *d, int add);       /* the includer's: tell diskd */

static int ctl(struct usb *u, struct usb_dev *d, unsigned rt, unsigned req, unsigned val, unsigned idx, void *buf,
               unsigned len)
{
    unsigned char s[8] = { (unsigned char)rt, (unsigned char)req, (unsigned char)val, (unsigned char)(val >> 8),
                           (unsigned char)idx, (unsigned char)(idx >> 8), (unsigned char)len, (unsigned char)(len >> 8) };
    return xc_control(u->x, d->slot, s, buf, len);
}

static u32 be32(const unsigned char *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 le32(const unsigned char *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* the configuration's mass-storage interface (class 8, SCSI transparent 6, bulk-only 0x50), alternate setting 0, and
 * its bulk endpoints -> UK_STORAGE, or UK_OTHER */
static int pick(struct usb_dev *d)
{
    const unsigned char *p = d->cfg, *end = d->cfg + d->cfglen;
    int ours = 0, found = 0;
    while (p + 2 <= end) {
        unsigned l = p[0];
        if (l < 2 || p + l > end)
            break;                      /* a descriptor running past what the device sent: the rest ignored */
        if (p[1] == DT_IFACE && l >= 9) {
            if (found)
                break;
            ours = p[3] == 0 && p[5] == 8 && p[6] == 6 && p[7] == 0x50;
            if (ours) {
                d->iface = p[2];
                d->ep_in = d->ep_out = 0;
            }
        } else if (p[1] == DT_ENDPOINT && l >= 7 && ours && (p[3] & 3) == 2) {
            unsigned mps = (p[4] | p[5] << 8) & 0x7FF;
            if (mps >= 8) {
                if (p[2] & 0x80) { d->ep_in = p[2]; d->mps_in = (unsigned short)mps; }
                else { d->ep_out = p[2]; d->mps_out = (unsigned short)mps; }
                found = d->ep_in && d->ep_out;
            }
        }
        p += l;
    }
    return found ? UK_STORAGE : UK_OTHER;
}

/* ---- bulk-only transport (BOT 1.0) ---- */

/* a bulk endpoint's halt cleared on both sides: CLEAR_FEATURE(ENDPOINT_HALT) sets the device's data toggle to DATA0
 * (USB 2.0 9.4.5), so the host's endpoint is dropped and added again, which sets its toggle to DATA0 too (xHCI 4.6.6;
 * otherwise every second CBW is taken as a repeat and lost) */
static void clear_halt(struct usb *u, struct usb_dev *d, int in)
{
    ctl(u, d, RT_EP, REQ_CLEAR_FEAT, 0, in ? d->ep_in : d->ep_out, 0, 0);
    xc_ep_open(u->x, d->slot, in ? 0 : 1, in ? d->ep_in : d->ep_out, in ? 6 : 2, in ? d->mps_in : d->mps_out, 0);
}

/* the reset recovery (BOT 5.3.4): Bulk-Only Mass Storage Reset, then both endpoints' halts cleared */
static void bot_reset(struct usb *u, struct usb_dev *d)
{
    ctl(u, d, RT_CLASS | RT_IFACE, MSC_RESET, 0, d->iface, 0, 0);
    clear_halt(u, d, 1);
    clear_halt(u, d, 0);
}

/* is the device still plugged in (a stick pulled in the middle of a command is noticed between its stages) */
static int there(struct usb *u, struct usb_dev *d) { return (xc_portsc(u->x, d->port) & PS_CCS) != 0; }

/* one SCSI command: the CDB, len bytes in from the device into the bounce buffer (0: no data) -> the bytes the device
 * sent (the CSW's residue taken off), -EIO (the command failed: the caller may ask REQUEST SENSE), -EPROTO (the
 * transport broke: reset recovery done) */
static int bot(struct usb *u, struct usb_dev *d, const unsigned char *cdb, unsigned cdblen, unsigned len)
{
    struct xhci *x = u->x;
    unsigned char w[31];
    memset(w, 0, sizeof w);
    u32 tag = ++d->tag;
    w[0] = 'U'; w[1] = 'S'; w[2] = 'B'; w[3] = 'C';
    for (int i = 0; i < 4; i++) {
        w[4 + i] = (unsigned char)(tag >> (8 * i));
        w[8 + i] = (unsigned char)(len >> (8 * i));
    }
    w[12] = len ? 0x80 : 0;             /* data in */
    w[14] = (unsigned char)cdblen;
    memcpy(w + 15, cdb, cdblen);
    dm_wr(x->cbw, w, 31);
    if (!there(u, d))
        return -ENODEV;
    int r = xc_bulk(x, d->slot, 1, x->cbw, 31);
    if (r != 31) {
        if (!there(u, d))
            return -ENODEV;
        xc_log(x, "storage: the command %02x was not taken (%d)", cdb[0], r);
        bot_reset(u, d);
        return -EPROTO;
    }
    int got = 0;
    if (len) {
        got = xc_bulk(x, d->slot, 0, x->bounce, len);
        if (got == -EPIPE) {            /* a stall in the data stage: the halt cleared, then the CSW (BOT 6.7.2) */
            clear_halt(u, d, 1);
            got = 0;
        } else if (got < 0) {
            if (!there(u, d))
                return -ENODEV;
            bot_reset(u, d);
            return -EPROTO;
        }
    }
    unsigned char c[13];
    r = xc_bulk(x, d->slot, 0, x->csw, 13);
    if (r == -EPIPE) {                  /* the CSW stalled: the halt cleared and the CSW asked once more (BOT 6.7) */
        clear_halt(u, d, 1);
        r = xc_bulk(x, d->slot, 0, x->csw, 13);
    }
    if (r != 13) {
        if (!there(u, d))
            return -ENODEV;
        xc_log(x, "storage: no status for the command %02x (%d)", cdb[0], r);
        bot_reset(u, d);
        return -EPROTO;
    }
    dm_rd(x->csw, c, 13);
    u32 sig = le32(c), ctag = le32(c + 4), residue = le32(c + 8);
    if (sig != CSW_SIG || ctag != tag || c[12] > 2) {
        xc_log(x, "storage: a status that is not the command's (signature %08x, tag %u of %u, status %u)",
               (unsigned)sig, (unsigned)ctag, (unsigned)tag, c[12]);
        bot_reset(u, d);
        return -EPROTO;
    }
    if (c[12] == 2) {                   /* phase error */
        xc_log(x, "storage: phase error on the command %02x", cdb[0]);
        bot_reset(u, d);
        return -EPROTO;
    }
    if (c[12] == 1)
        return -EIO;
    if (residue > len)
        return -EPROTO;
    return (int)(len - residue) < got ? (int)(len - residue) : got;
}

/* REQUEST SENSE -> the sense key << 16 | ASC << 8 | ASCQ, or -err */
static int sense(struct usb *u, struct usb_dev *d)
{
    unsigned char cdb[6] = { 0x03, 0, 0, 0, 18, 0 }, s[18];
    int r = bot(u, d, cdb, 6, 18);
    if (r < 14)
        return r < 0 ? r : -EIO;
    dm_rd(u->x->bounce, s, 14);
    return (s[2] & 15) << 16 | s[12] << 8 | s[13];
}

/* a stick: INQUIRY, ready (up to 5 s), its capacity in 512-byte sectors -> 0, or -err (said) */
static int msc_start(struct usb *u, struct usb_dev *d)
{
    struct xhci *x = u->x;
    unsigned char lun = 0;
    if (ctl(u, d, RT_IN | RT_CLASS | RT_IFACE, MSC_GET_MAX_LUN, 0, d->iface, &lun, 1) == 1 && lun)
        xc_log(x, "storage: %u logical units: the first is served", lun + 1u);
    if (xc_ep_open(x, d->slot, 0, d->ep_in, 6, d->mps_in, 0) || xc_ep_open(x, d->slot, 1, d->ep_out, 2, d->mps_out, 0))
        return -EIO;
    unsigned char inq[6] = { 0x12, 0, 0, 0, 36, 0 }, b[36];
    int r = bot(u, d, inq, 6, 36);
    if (r < 0 && r != -EIO)
        return r;
    if (r >= 32) {                      /* vendor (8 bytes) and product (16), spaces folded */
        dm_rd(x->bounce, b, 32);
        if (b[0] & 0x1F) {
            xc_log(x, "storage: device type %u (not a disk): left alone", b[0] & 0x1F);
            return -ENODEV;
        }
        unsigned char raw[25];
        memcpy(raw, b + 8, 8);
        raw[8] = ' ';
        memcpy(raw + 9, b + 16, 16);
        int n = 0;
        for (int i = 0; i < 25 && n < 30; i++) {
            unsigned char ch = raw[i];
            if (ch < 32 || ch > 126 || (ch == ' ' && (!n || d->label[n - 1] == ' ')))
                continue;
            d->label[n++] = (char)ch;
        }
        while (n && d->label[n - 1] == ' ') n--;
        d->label[n] = 0;
    }
    u32 t0 = xc_ticks();
    for (;;) {                          /* TEST UNIT READY until ready, at most 5 s (a stick waking up, a unit attention) */
        unsigned char tur[6] = { 0 };
        r = bot(u, d, tur, 6, 0);
        if (r == 0)
            break;
        if (r != -EIO)
            return r;
        int k = sense(u, d);
        if (k >> 16 == 2 && (k >> 8 & 0xFF) == 0x3A) {
            xc_log(x, "storage: no medium (an empty card reader?)");
            return -ENODEV;
        }
        if (xc_ticks() - t0 > 500) {
            xc_log(x, "storage: not ready after 5 s (sense %06x)", k < 0 ? 0 : k);
            return -ETIMEDOUT;
        }
        xc_wait();
        xc_wait();
    }
    unsigned char rc[10] = { 0x25 }, cap[32];
    for (int tries = 0; ; tries++) {    /* READ CAPACITY (10); some sticks fail the first one */
        r = bot(u, d, rc, 10, 8);
        if (r == 8)
            break;
        if (tries == 2 || r != -EIO)
            return r < 0 ? r : -EIO;
        sense(u, d);
    }
    dm_rd(x->bounce, cap, 8);
    u32 last = be32(cap), bs = be32(cap + 4);
    unsigned long long n = (unsigned long long)last + 1;
    if (last == 0xFFFFFFFFu) {          /* past 2 TB: READ CAPACITY (16) */
        unsigned char rc16[16] = { 0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0 };
        if (bot(u, d, rc16, 16, 32) < 12)
            return -EIO;
        dm_rd(x->bounce, cap, 12);
        n = ((unsigned long long)be32(cap) << 32 | be32(cap + 4)) + 1;
        bs = be32(cap + 8);
    }
    if (bs != 512) {
        xc_log(x, "storage: %u-byte blocks: only 512-byte ones are served", (unsigned)bs);
        return -ENODEV;
    }
    if (!n) {
        xc_log(x, "storage: no sectors");
        return -ENODEV;
    }
    if (n > 0xFFFFFFFFull) {
        xc_log(x, "storage: past 2 TB: only the first 2 TB are served");
        n = 0xFFFFFFFFull;
    }
    d->sectors = (u32)n;
    xc_log(x, "storage: \"%s\", %u sectors (%u MB), read-only", d->label, (unsigned)d->sectors,
           (unsigned)(d->sectors >> 11));
    return 0;
}

/* n sectors (at most MSC_MAXSECT) from lba into the bounce buffer -> 0, -EIO, -EINVAL, -ENODEV (pulled out). Tried
 * again while 4 s have not passed (diskd waits 5 s for the answer) */
static int msc_read(struct usb *u, struct usb_dev *d, u32 lba, u32 n)
{
    if (!d->ready || !n || n > MSC_MAXSECT || lba + n < lba || lba + n > d->sectors)
        return -EINVAL;
    u32 t0 = xc_ticks();
    for (int tries = 0; tries < 3 && xc_ticks() - t0 < 400; tries++) {
        unsigned char cdb[10] = { 0x28, 0, (unsigned char)(lba >> 24), (unsigned char)(lba >> 16), (unsigned char)(lba >> 8),
                                  (unsigned char)lba, 0, (unsigned char)(n >> 8), (unsigned char)n, 0 };
        int r = bot(u, d, cdb, 10, n * 512);
        if (r == (int)(n * 512))
            return 0;
        if (r == -ENODEV || u->x->dead)
            return -ENODEV;
        if (r == -EIO)
            sense(u, d);
        d->errors++;
    }
    xc_log(u->x, "storage: sectors %u-%u could not be read", (unsigned)lba, (unsigned)(lba + n - 1));
    return -EIO;
}

/* a device on a root port: address, descriptors, configuration, a stick's start -> its table entry, or 0 */
static struct usb_dev *enumerate(struct usb *u, unsigned port, unsigned speed)
{
    struct usb_dev *d = 0;
    for (int i = 0; i < USB_MAXDEV; i++)
        if (!u->dev[i].used) { d = &u->dev[i]; break; }
    if (!d) {
        xc_log(u->x, "port %u: no room for another device: left alone", port);
        return 0;
    }
    int sid = xc_dev_new(u->x, speed, port);
    if (sid < 0)
        return 0;
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->slot = (unsigned char)sid;
    d->speed = (unsigned char)speed;
    d->port = (unsigned char)port;
    d->cfg = cfgbuf[d - u->dev];
    const char *why = 0;
    int r = ctl(u, d, RT_IN, REQ_GET_DESC, DT_DEVICE << 8, 0, d->dd, 8);
    unsigned m = d->dd[7];
    if (r < 8 || d->dd[0] < 8 || d->dd[1] != DT_DEVICE)
        why = "no device descriptor";
    else if (m != 8 && m != 16 && m != 32 && m != 64)
        why = "an impossible packet size for its control endpoint";
    else if (m != (speed == USB_SPEED_HIGH ? 64u : 8u) && xc_set_mps0(u->x, d->slot, m))
        why = "its control endpoint's packet size could not be set";
    else if (ctl(u, d, RT_IN, REQ_GET_DESC, DT_DEVICE << 8, 0, d->dd, 18) < 18 || d->dd[0] < 18)
        why = "a short device descriptor";
    else if ((r = ctl(u, d, RT_IN, REQ_GET_DESC, DT_CONFIG << 8, 0, d->cfg, 9)) < 9 || d->cfg[1] != DT_CONFIG ||
             d->cfg[0] < 9)
        why = "no configuration descriptor";
    else {
        unsigned total = d->cfg[2] | d->cfg[3] << 8;
        if (total < 9)
            why = "a configuration shorter than its own header";
        else {
            if (total > USB_CFGMAX)
                total = USB_CFGMAX;     /* what is past it is not looked at */
            r = ctl(u, d, RT_IN, REQ_GET_DESC, DT_CONFIG << 8, 0, d->cfg, total);
            if (r < 9)
                why = "the configuration could not be read";
            else
                d->cfglen = (unsigned)r;
        }
    }
    if (!why) {
        d->cfgval = d->cfg[5];
        d->kind = (unsigned char)pick(d);
        if (ctl(u, d, 0, REQ_SET_CONFIG, d->cfgval, 0, 0, 0) < 0)
            why = "it refused its configuration";
    }
    xc_log(u->x, "port %u: %s-speed device %04x:%04x, class %02x%s%s", port,
           speed == USB_SPEED_LOW ? "low" : speed == USB_SPEED_HIGH ? "high" : "full", d->dd[8] | d->dd[9] << 8,
           d->dd[10] | d->dd[11] << 8, d->dd[4], why ? ": " : "", why ? why : "");
    if (!why && d->kind == UK_STORAGE && msc_start(u, d))
        why = "the stick could not be started";
    if (why) {
        xc_log(u->x, "port %u: %s: left alone until it is plugged in again", port, why);
        xc_dev_free(u->x, d->slot);
        d->used = 0;
        return 0;
    }
    if (d->kind == UK_OTHER)
        xc_log(u->x, "port %u: not a USB stick (no driver yet): listed only", port);
    else {
        d->ready = 1;
        usb_news(d, 1);
    }
    return d;
}

static void detach(struct usb *u, struct usb_dev *d)
{
    if (d->ready)
        usb_news(d, 0);
    xc_log(u->x, "port %u: device %04x:%04x gone", d->port, d->dd[8] | d->dd[9] << 8, d->dd[10] | d->dd[11] << 8);
    xc_dev_free(u->x, d->slot);
    d->used = 0;
    d->ready = 0;
}

/* the root ports' changes: a device that came (after 100 ms of settling) or went -> 1 when something happened */
static int usb_poll(struct usb *u)
{
    struct xhci *x = u->x;
    int any = xc_poll(x) > 0;
    while (x->port_change && !x->dead) {
        unsigned p = 1;
        while (!(x->port_change >> p & 1))
            p++;
        x->port_change &= ~(1u << p);
        any = 1;
        u32 v = xc_portsc(x, p);
        xc_port_ack(x, p, v & PS_CHANGES);
        struct usb_dev *d = 0;
        for (int i = 0; i < USB_MAXDEV; i++)
            if (u->dev[i].used && u->dev[i].port == p)
                d = &u->dev[i];
        if ((v & PS_CSC) || (d && !(v & PS_CCS))) {     /* a connect or a disconnect: what was there is gone, and a
                                                         * refusal forgotten (a device plugged in again is tried) */
            if (d)
                detach(u, d);
            u->refused &= ~(1u << p);
            u->settle[p] = 0;
        }
        if ((v & PS_CCS) && !d && !(u->refused >> p & 1) && !u->settle[p])
            u->settle[p] = (xc_ticks() + 10) | 1;     /* never 0: 0 is "nothing waits" */
    }
    for (unsigned p = 1; p <= x->maxports && p < 32 && !x->dead; p++) {
        if (!u->settle[p] || (int)(xc_ticks() - u->settle[p]) < 0)
            continue;
        u->settle[p] = 0;
        any = 1;
        int sp = xc_port_reset(x, p);
        xc_wait();
        xc_wait();                      /* reset recovery, 10 ms (USB 2.0 7.1.7.5) */
        if ((sp == USB_SPEED_LOW || sp == USB_SPEED_FULL || sp == USB_SPEED_HIGH) && enumerate(u, p, (unsigned)sp))
            ;
        else {
            if (sp < 0 || sp > USB_SPEED_HIGH)
                xc_log(x, "root port %u: %s: left alone until it is plugged in again", p,
                       sp < 0 ? "the reset failed" : "not a USB 2.0 device");
            u->refused |= 1u << p;      /* never reset again in a loop (the reset's own change is no news) */
            u->nrefused++;
        }
    }
    return any;
}

/* usbd ends: the sticks said gone, the controller halted */
static void usb_stop(struct usb *u)
{
    for (int i = 0; i < USB_MAXDEV; i++)
        if (u->dev[i].used && u->dev[i].ready)
            usb_news(&u->dev[i], 0);
    xc_stop(u->x);
}
