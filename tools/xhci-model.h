/* SPDX-License-Identifier: GPL-2.0-only */
/* A model of the BCM2711's xHCI USB 2.0 controller and of USB devices behind it, for testing usbd's code
 * (usb/xhci-core.c, usb/core.c) on the PC (DESIGN D46, docs/d46-usb.md 6.1). Written from xHCI 1.2, USB 2.0 chapters 9
 * and 11 and HID 1.11, as tools/genet-model.h is written for the Ethernet chip.
 *
 * The controller: capability, operational, port, runtime and doorbell registers; reset, run and halt; the command ring
 * (Enable/Disable Slot, Address Device with the device found by its root port and route string, Configure Endpoint
 * with the hub fields, Evaluate Context, Reset/Stop Endpoint, Set TR Dequeue, No-Op); the event ring with its segment
 * table and dequeue pointer; transfer rings with link TRBs and cycle bits, control transfers in three stages, interrupt
 * IN transfers that wait for data, short packets, stalls, a device that never answers, one that goes away; the root
 * port's connect, reset and change bits (write 1 to clear). The memory is bytes, little-endian, at the model's own
 * addresses (M_BASE ...), never host pointers, with the ARM's data cache between the CPU and the controller: the CPU's
 * view, RAM, the lines the CPU wrote and has not cleaned, the lines the controller wrote since the CPU last
 * invalidated them.
 *
 * Strict: what the real controller would do wrong with, or what would corrupt memory, is counted in M.faults with a
 * line saying what: the controller reading a line the CPU wrote and never cleaned, a dirty line written back over what
 * the controller wrote, an address outside the memory, a ring, context or array crossing a page, a TRB's buffer
 * crossing 64 KB, a doorbell while halted, a slot never enabled, Run/Stop without the DCBAA or the rings, a full event
 * ring, a device behind a hub whose slot was not made a hub's, wrong transaction-translator fields, a route string that
 * leads nowhere. It shows the driver's order and arithmetic agree with the specification; only the real controller
 * can show the BCM2711 behaves the same. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define M_BASE  0x10000000u
#define M_SIZE  (256 * 1024)
#define M_LINE  64
#define M_WIN   0x10000u
#define M_CAPLEN 0x20u
#define M_RTS   0x600u
#define M_DB    0x800u
#define M_SLOTS 16
#define M_PORTS 1

struct mdev;
struct mhubport { struct mdev *dev; uint16_t st, chg; int powered; };
struct mdev {
    const char *name;
    int kind;                           /* MD_* */
    int speed;                          /* 1 full, 2 low, 3 high (xHCI's IDs) */
    unsigned char dd[18];
    unsigned char cfg[600];
    unsigned cfglen;                    /* what GET_DESCRIPTOR(config) answers from (wTotalLength may say otherwise) */
    int addressed, configured, proto, idle, nostall_proto;
    unsigned char leds;
    int setreports;
    /* reports waiting to be read on its interrupt endpoint */
    unsigned char rq[32][8];
    unsigned rqlen[32];
    unsigned rqh, rqt;
    /* a hub */
    int nports, hubdesc_ports, ttt;
    struct mhubport port[15];
    /* how it misbehaves */
    int stall_dev_desc, never_answer, gone;
    /* a USB stick (MD_MSC): bulk-only transport over SCSI, its sectors from an image file */
    int imgfd;                          /* -1: every sector reads as its number */
    uint32_t sectors, blocksize;
    int bot;                            /* 0 waiting for a CBW, 1 data to send, 2 the CSW to send */
    unsigned char data[65536 + 64], csw[13], sense[3];
    uint32_t dlen, doff;
    int halt_in, halt_out;              /* the device's endpoints halted (CLEAR_FEATURE(ENDPOINT_HALT) clears them) */
    int tog_in, tog_out;                /* the device's data toggles (USB 2.0 8.6): CLEAR_FEATURE sets them to DATA0 */
    int dropped;                        /* packets thrown away for a wrong toggle */
    /* how it misbehaves */
    int not_ready, no_medium, cap16, bad_sig, bad_tag, phase_err, stall_data, never_csw, read_fail, babble, devtype;
    int reads, resets;
    struct mdev *parent;
    int parent_port;
};
#define MD_OTHER 0
#define MD_HUB   1
#define MD_KBD   2
#define MD_MOUSE 3
#define MD_MSC   4

struct mslot {
    int on;
    uint32_t out;                       /* output context */
    struct mdev *dev;
    int hub, nports, ttt;               /* from Configure Endpoint */
    unsigned mps0;
    struct { uint32_t deq; int cyc, state, type, mps, tog; } ep[32];  /* by DCI; state 0 disabled 1 running 2 halted 3
                                         * stopped; tog the host's data toggle (Reset Endpoint and a drop+add set DATA0) */
};

static struct {
    unsigned char cpu[M_SIZE], ram[M_SIZE];
    unsigned char dirty[M_SIZE / M_LINE], dmaw[M_SIZE / M_LINE];
    uint32_t usbcmd, usbsts, config, crcr_lo, dcbaap, erstsz, erstba, erdp, iman, imod;
    uint32_t cmd_deq; int cmd_ccs; int cmd_set;
    uint32_t ev_base, ev_n, ev_enq; int ev_pcs;
    uint32_t portsc[M_PORTS + 1];
    struct mdev *root[M_PORTS + 1];
    struct mslot slot[M_SLOTS + 1];
    unsigned maxslots, nscratch, csz;
    int faults, expect_faults, quiet;
    unsigned time;
    int cnr_ticks;                      /* the reset's CNR stays this many ticks */
    int allones;                        /* every register reads all ones (an unpowered block) */
    unsigned events, cmds;
} M;

static void mfault(const char *what, uint32_t a)
{
    M.faults++;
    if (!M.expect_faults && !M.quiet)
        printf("  model fault: %s (%08x)\n", what, (unsigned)a);
}

static int mok(uint32_t a, uint32_t n)
{
    if (a < M_BASE || a - M_BASE > M_SIZE - n || n > M_SIZE) { mfault("an address outside the memory", a); return 0; }
    return 1;
}
static unsigned line(uint32_t a) { return (a - M_BASE) / M_LINE; }

/* ---- the CPU's side (the includer's dm_* call these) ---- */
static uint32_t cpu_r32(uint32_t a)
{
    if (!mok(a, 4)) return 0;
    unsigned char *p = M.cpu + (a - M_BASE);
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
static void cpu_w32(uint32_t a, uint32_t v)
{
    if (!mok(a, 4)) return;
    unsigned char *p = M.cpu + (a - M_BASE);
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
    M.dirty[line(a)] = 1;
}
static void cpu_rd(uint32_t a, void *b, uint32_t n) { if (n && mok(a, n)) memcpy(b, M.cpu + (a - M_BASE), n); }
static void cpu_wr(uint32_t a, const void *b, uint32_t n)
{
    if (!n || !mok(a, n)) return;
    memcpy(M.cpu + (a - M_BASE), b, n);
    for (uint32_t l = line(a); l <= line(a + n - 1); l++) M.dirty[l] = 1;
}
/* clean (how 1) or clean and invalidate (how 2) whole lines */
static void cpu_cache(unsigned how, uint32_t a, uint32_t n)
{
    if (!n || !mok(a, n)) return;
    for (uint32_t l = line(a); l <= line(a + n - 1); l++) {
        if (M.dirty[l]) {
            if (M.dmaw[l]) mfault("a dirty line written back over what the controller wrote", M_BASE + l * M_LINE);
            memcpy(M.ram + l * M_LINE, M.cpu + l * M_LINE, M_LINE);
            M.dirty[l] = 0;
        }
        if (how == 2) {
            memcpy(M.cpu + l * M_LINE, M.ram + l * M_LINE, M_LINE);
            M.dmaw[l] = 0;
        }
    }
}

/* ---- the controller's side ---- */
static uint32_t mr32(uint32_t a)
{
    if (!mok(a, 4)) return 0;
    if (M.dirty[line(a)]) mfault("the controller read a line the CPU wrote and never cleaned", a);
    unsigned char *p = M.ram + (a - M_BASE);
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
static void mw32(uint32_t a, uint32_t v)
{
    if (!mok(a, 4)) return;
    unsigned char *p = M.ram + (a - M_BASE);
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
    M.dmaw[line(a)] = 1;
}
static void mwr(uint32_t a, const unsigned char *b, uint32_t n)
{
    if (!n || !mok(a, n)) return;
    if ((a ^ (a + n - 1)) & ~0xFFFFu) mfault("a TRB's buffer crossing 64 KB", a);
    memcpy(M.ram + (a - M_BASE), b, n);
    for (uint32_t l = line(a); l <= line(a + n - 1); l++) M.dmaw[l] = 1;
}
static void mrd(uint32_t a, unsigned char *b, uint32_t n)
{
    if (!n || !mok(a, n)) return;
    if ((a ^ (a + n - 1)) & ~0xFFFFu) mfault("a TRB's buffer crossing 64 KB", a);
    for (uint32_t l = line(a); l <= line(a + n - 1); l++)
        if (M.dirty[l]) mfault("the controller read a line the CPU wrote and never cleaned", a);
    memcpy(b, M.ram + (a - M_BASE), n);
}
static void in_page(uint32_t a, uint32_t n, const char *what)
{
    if ((a ^ (a + n - 1)) & ~0xFFFu) mfault(what, a);
}

static void post(uint32_t p0, uint32_t st, uint32_t ctl)
{
    uint32_t deq = (M.erdp & ~15u);
    uint32_t nxt = M.ev_enq + 16 == M.ev_base + M.ev_n * 16 ? M.ev_base : M.ev_enq + 16;
    if (!M.ev_base) { mfault("an event with no event ring", 0); return; }
    if (nxt == deq) { mfault("the event ring is full (ERDP not moved)", M.ev_enq); return; }
    mw32(M.ev_enq, p0); mw32(M.ev_enq + 4, 0); mw32(M.ev_enq + 8, st);
    mw32(M.ev_enq + 12, (ctl & ~1u) | (uint32_t)M.ev_pcs);
    M.ev_enq = nxt;
    if (nxt == M.ev_base) M.ev_pcs ^= 1;
    M.usbsts |= 8;                      /* EINT */
    M.iman |= 1;
    M.events++;
}
#define EV(type) ((uint32_t)(type) << 10)

/* ---- devices ---- */
static int rq_pop(struct mdev *d, unsigned char *b, unsigned *len)
{
    if (d->rqh == d->rqt) return 0;
    memcpy(b, d->rq[d->rqt], 8);
    *len = d->rqlen[d->rqt];
    d->rqt = (d->rqt + 1) % 32;
    return 1;
}
static void rq_push(struct mdev *d, const unsigned char *b, unsigned len)
{
    memcpy(d->rq[d->rqh], b, len);
    d->rqlen[d->rqh] = len;
    d->rqh = (d->rqh + 1) % 32;
}

/* a control request -> bytes of answer (IN) or 0, -1 for a stall, -2 for no answer */
static int mdev_control(struct mdev *d, const unsigned char *s, unsigned char *buf, unsigned len, const unsigned char *out)
{
    unsigned rt = s[0], req = s[1], val = s[2] | s[3] << 8, idx = s[4] | s[5] << 8;
    (void)idx;
    if (d->never_answer) return -2;
    if (rt == 0x80 && req == 6) {
        unsigned t = val >> 8, n = 0;
        const unsigned char *src = 0;
        if (t == 1) { if (d->stall_dev_desc) return -1; src = d->dd; n = 18; }
        else if (t == 2) { src = d->cfg; n = d->cfglen; }
        else return -1;
        if (n > len) n = len;
        memcpy(buf, src, n);
        return (int)n;
    }
    if (rt == 0 && req == 9) { d->configured = val; d->tog_in = d->tog_out = 0; d->halt_in = d->halt_out = 0; d->bot = 0; return 0; }   /* SET_CONFIGURATION: every endpoint at DATA0 (USB 2.0 9.1.1.5) */
    if (rt == 0x02 && req == 1) {                               /* CLEAR_FEATURE(ENDPOINT_HALT): DATA0 too */
        if (idx & 0x80) { d->halt_in = 0; d->tog_in = 0; } else { d->halt_out = 0; d->tog_out = 0; }
        return 0;
    }
    if (d->kind == MD_MSC) {
        if (rt == 0xA1 && req == 0xFE) { if (len) buf[0] = 0; return len ? 1 : 0; }     /* GET MAX LUN: one */
        if (rt == 0x21 && req == 0xFF) { d->bot = 0; d->resets++; return 0; }        /* Bulk-Only Mass Storage Reset */
        return -1;
    }
    if (d->kind == MD_KBD || d->kind == MD_MOUSE) {
        if (rt == 0x21 && req == 0x0B) { if (d->nostall_proto < 0) return -1; d->proto = (int)val; return 0; }
        if (rt == 0x21 && req == 0x0A) { d->idle = 1; return 0; }
        if (rt == 0x21 && req == 0x09) { d->leds = out && len ? out[0] : 0; d->setreports++; return 0; }
        return -1;
    }
    if (d->kind == MD_HUB) {
        if (rt == 0xA0 && req == 6 && (val >> 8) == 0x29) {
            unsigned char h[9] = { 9, 0x29, (unsigned char)d->hubdesc_ports, (unsigned char)(d->ttt << 5), 0, 50, 0, 0, 0xFF };
            unsigned n = len < 9 ? len : 9;
            memcpy(buf, h, n);
            return (int)n;
        }
        if (idx < 1 || idx > (unsigned)d->nports) return -1;
        struct mhubport *p = &d->port[idx - 1];
        if (rt == 0xA3 && req == 0) {
            unsigned char st[4] = { (unsigned char)p->st, (unsigned char)(p->st >> 8), (unsigned char)p->chg, (unsigned char)(p->chg >> 8) };
            unsigned n = len < 4 ? len : 4;
            memcpy(buf, st, n);
            return (int)n;
        }
        if (rt == 0x23 && req == 3) {
            if (val == 8) { p->powered = 1; p->st |= 0x100; return 0; }
            if (val == 4 && (p->st & 1)) {
                p->st |= 2;             /* enabled, the speed bits from the device */
                p->st &= ~(0x200 | 0x400);
                if (p->dev && p->dev->speed == 2) p->st |= 0x200;
                if (p->dev && p->dev->speed == 3) p->st |= 0x400;
                p->chg |= 0x10;
                return 0;
            }
            return 0;
        }
        if (rt == 0x23 && req == 1) {
            if (val == 16) p->chg &= ~1;
            if (val == 17) p->chg &= ~2;
            if (val == 20) p->chg &= ~0x10;
            return 0;
        }
        return -1;
    }
    return -1;
}

/* the device a slot context names: its root port, then the route string's hub ports */
static struct mdev *route_dev(unsigned rootport, uint32_t route, int *tier)
{
    if (rootport < 1 || rootport > M_PORTS) return 0;
    struct mdev *d = M.root[rootport];
    *tier = 0;
    for (int t = 0; d && t < 5; t++) {
        unsigned p = route >> (4 * t) & 15;
        if (!p) break;
        if (d->kind != MD_HUB || (int)p > d->nports) return 0;
        d = d->port[p - 1].dev;
        if (d && !(d->parent && (d->parent->port[p - 1].st & 2))) return 0;     /* its hub port is not enabled */
        (*tier)++;
    }
    return d;
}
static int slot_of(struct mdev *d)
{
    for (int i = 1; i <= M_SLOTS; i++) if (M.slot[i].on && M.slot[i].dev == d) return i;
    return 0;
}

/* ---- commands ---- */
static uint32_t ctxa(uint32_t base, unsigned i) { return base + i * M.csz; }

static int cmd_address(uint32_t in, unsigned sid, int bsr)
{
    struct mslot *s = &M.slot[sid];
    if ((in & 63) || (M.slot[sid].out & 63)) mfault("a context not 64-byte aligned", in);
    in_page(in, 33 * M.csz, "an input context crossing a page");
    in_page(s->out, 32 * M.csz, "an output context crossing a page");
    uint32_t add = mr32(in + 4), s0 = mr32(ctxa(in, 1)), s1 = mr32(ctxa(in, 1) + 4), s2 = mr32(ctxa(in, 1) + 8);
    uint32_t e1 = mr32(ctxa(in, 2) + 4), e2 = mr32(ctxa(in, 2) + 8);
    if ((add & 3) != 3) { mfault("Address Device without the slot and EP0 added", in); return 5; }
    int tier;
    struct mdev *d = route_dev(s1 >> 16 & 0xFF, s0 & 0xFFFFF, &tier);
    if (!d || d->gone) return 4;        /* nothing answers there: a transaction error */
    if ((int)(s0 >> 20 & 15) != d->speed) mfault("the slot's speed is not the device's", s0);
    struct mdev *h = d->parent;
    if (h) {
        int hs = slot_of(h);
        if (!hs || !M.slot[hs].hub) mfault("a device behind a hub whose slot was not made a hub's", s0);
        if (d->speed != 3 && h->speed == 3 &&
            ((s2 & 0xFF) != (unsigned)hs || (s2 >> 8 & 0xFF) != (unsigned)d->parent_port))
            mfault("wrong transaction-translator fields", s2);
    } else if (s2 & 0xFFFF)
        mfault("translator fields on a device on the root port", s2);
    if ((e1 >> 3 & 7) != 4) mfault("EP0 not a control endpoint", e1);
    in_page(e2 & ~15u, 16, "EP0's ring");
    s->dev = d;
    s->mps0 = e1 >> 16;
    s->ep[1].deq = e2 & ~15u; s->ep[1].cyc = (int)(e2 & 1); s->ep[1].state = 1; s->ep[1].type = 4; s->ep[1].mps = (int)(e1 >> 16);
    if (!bsr) d->addressed = 1;
    for (unsigned i = 0; i < 2 * M.csz / 4; i++)                /* the slot and EP0 into the output context */
        mw32(s->out + 4 * i, mr32(ctxa(in, 1) + 4 * i));
    mw32(s->out + 12, (2u << 27) | (uint32_t)sid);              /* addressed */
    mw32(ctxa(s->out, 1), (mr32(ctxa(in, 2)) & ~7u) | 1);       /* EP0 running */
    return 1;
}

static int cmd_config(uint32_t in, unsigned sid)
{
    struct mslot *s = &M.slot[sid];
    in_page(in, 33 * M.csz, "an input context crossing a page");
    uint32_t drop = mr32(in), add = mr32(in + 4), s0 = mr32(ctxa(in, 1)), s1 = mr32(ctxa(in, 1) + 4), s2 = mr32(ctxa(in, 1) + 8);
    if (!(add & 1)) mfault("Configure Endpoint without the slot context added", add);
    for (unsigned dci = 2; dci < 32; dci++) {
        if (drop >> dci & 1) s->ep[dci].state = 0;
        if (!(add >> dci & 1)) continue;
        if (s->ep[dci].state) mfault("an endpoint added that is enabled already (no drop)", dci);
        s->ep[dci].tog = 0;
        uint32_t c = ctxa(in, 1 + dci), e0 = mr32(c), e1 = mr32(c + 4), e2 = mr32(c + 8);
        if ((s0 >> 27) < dci) mfault("context entries below an endpoint added", s0);
        if (!(e1 >> 16)) mfault("an endpoint with max packet 0", e1);
        if ((e2 & 15u & ~1u) || !(e2 & ~15u)) mfault("an endpoint ring pointer that is not a TRB's", e2);
        in_page(e2 & ~15u, 16, "an endpoint ring");
        (void)e0;
        s->ep[dci].deq = e2 & ~15u; s->ep[dci].cyc = (int)(e2 & 1); s->ep[dci].state = 1;
        s->ep[dci].type = (int)(e1 >> 3 & 7); s->ep[dci].mps = (int)(e1 >> 16);
        mw32(ctxa(s->out, dci), (e0 & ~7u) | 1);
    }
    s->hub = (int)(s0 >> 26 & 1);
    s->nports = (int)(s1 >> 24);
    s->ttt = (int)(s2 >> 16 & 3);
    if (s->hub && s->dev && s->nports != s->dev->nports) mfault("a hub's slot with the wrong number of ports", s1);
    mw32(s->out, s0); mw32(s->out + 4, s1); mw32(s->out + 8, s2);
    return 1;
}

static void do_command(uint32_t a, uint32_t c)
{
    unsigned type = c >> 10 & 63, sid = c >> 24, dci = c >> 16 & 31;
    uint32_t p0 = mr32(a);
    int cc = 1, rsid = 0;
    struct mslot *s = sid >= 1 && sid <= M_SLOTS ? &M.slot[sid] : 0;
    M.cmds++;
    if (type != 9 && type != 23 && (!s || !s->on)) { mfault("a command for a slot never enabled", c); cc = 11; goto done; }
    switch (type) {
    case 9:
        cc = 9;
        for (unsigned i = 1; i <= M.config && i <= M_SLOTS; i++)
            if (!M.slot[i].on) {
                memset(&M.slot[i], 0, sizeof M.slot[i]);
                M.slot[i].on = 1;
                rsid = (int)i;
                cc = 1;
                break;
            }
        break;
    case 10: s->on = 0; if (s->dev) s->dev->addressed = 0; break;
    case 11:
        s->out = mr32(M.dcbaap + 8 * sid);
        if (!s->out) { mfault("Address Device with no output context in the DCBAA", sid); cc = 5; break; }
        cc = cmd_address(p0 & ~15u, sid, (c >> 9) & 1);
        break;
    case 12: cc = cmd_config(p0 & ~15u, sid); break;
    case 13: {
        uint32_t in = p0 & ~15u, e1 = mr32(ctxa(in, 2) + 4);
        s->mps0 = e1 >> 16; s->ep[1].mps = (int)(e1 >> 16);
        break;
    }
    case 14: if (s->ep[dci].state != 2) cc = 19; else { s->ep[dci].state = 3; s->ep[dci].tog = 0; } break;
    case 15: if (s->ep[dci].state == 1) s->ep[dci].state = 3; break;
    case 16:
        if (s->ep[dci].state != 3) { cc = 19; break; }
        s->ep[dci].deq = p0 & ~15u; s->ep[dci].cyc = (int)(p0 & 1); s->ep[dci].state = 1;
        break;
    case 23: break;
    default: cc = 5;
    }
done:
    post(a, (uint32_t)cc << 24, EV(33) | (uint32_t)rsid << 24 | (type != 9 ? (uint32_t)sid << 24 : 0));
}

static void run_commands(void)
{
    if (M.usbsts & 1) { mfault("a doorbell while halted", 0); return; }
    for (int guard = 0; guard < 1000; guard++) {
        uint32_t a = M.cmd_deq, c = mr32(a + 12);
        if ((int)(c & 1) != M.cmd_ccs) return;
        if ((c >> 10 & 63) == 6) {      /* a link */
            M.cmd_deq = mr32(a) & ~15u;
            if (c & 2) M.cmd_ccs ^= 1;
            continue;
        }
        M.cmd_deq += 16;
        do_command(a, c);
    }
}

/* ---- transfers ---- */
static void ring_next(struct mslot *s, unsigned dci, uint32_t *a)
{
    *a += 16;
    uint32_t c = mr32(*a + 12);
    if ((c >> 10 & 63) == 6 && (int)(c & 1) == s->ep[dci].cyc) {
        *a = mr32(*a) & ~15u;
        if (c & 2) s->ep[dci].cyc ^= 1;
    }
}

/* EP0: one TD (setup, data, status) if it is there and the device answers */
static int run_control(unsigned sid)
{
    struct mslot *s = &M.slot[sid];
    if (s->ep[1].state != 1) return 0;
    uint32_t a = s->ep[1].deq, c = mr32(a + 12);
    if ((int)(c & 1) != s->ep[1].cyc) return 0;
    if ((c >> 10 & 63) != 2) { mfault("a control TD that does not start with a setup TRB", c); s->ep[1].state = 2; return 0; }
    if (!(c & (1u << 6))) mfault("a setup TRB without IDT", c);
    unsigned char setup[8];
    uint32_t w0 = mr32(a), w1 = mr32(a + 4);
    for (int i = 0; i < 4; i++) { setup[i] = (unsigned char)(w0 >> (8 * i)); setup[4 + i] = (unsigned char)(w1 >> (8 * i)); }
    uint32_t sa = a, da = 0, dlen = 0, dc = 0, sta;
    ring_next(s, 1, &a);
    c = mr32(a + 12);
    if ((c >> 10 & 63) == 3) {
        da = a; dc = c; dlen = mr32(a + 8) & 0x1FFFF;
        ring_next(s, 1, &a);
        c = mr32(a + 12);
    }
    if ((c >> 10 & 63) != 4 || (int)(c & 1) != s->ep[1].cyc) return 0;    /* not all there yet */
    sta = a;
    unsigned wlen = setup[6] | setup[7] << 8;
    if (da && wlen != dlen) mfault("the data stage's length is not the setup's wLength", dlen);
    struct mdev *d = s->dev;
    if (!d || d->gone) { post(sa, 4u << 24, EV(32) | 1u << 16 | sid << 24); s->ep[1].state = 2; return 1; }
    unsigned char buf[600], out[600];
    int in = setup[0] & 0x80;
    if (da && !in) mrd(mr32(da), out, dlen > sizeof out ? sizeof out : dlen);
    int r = mdev_control(d, setup, buf, dlen > sizeof buf ? sizeof buf : dlen, out);
    if (r == -2) return 0;              /* the device NAKs: the TD stays */
    if (r == -1) {
        post(da ? da : sta, 6u << 24, EV(32) | 1u << 16 | sid << 24);
        s->ep[1].state = 2;
        s->ep[1].deq = sa;
        return 1;
    }
    if (da && in) {
        if (s->mps0 < (unsigned)d->dd[7] && dlen > s->mps0 && r > (int)s->mps0) {
            post(da, 3u << 24, EV(32) | 1u << 16 | sid << 24);  /* babble: EP0's packet size was never set */
            s->ep[1].state = 2;
            s->ep[1].deq = sa;
            return 1;
        }
        mwr(mr32(da), buf, (uint32_t)r);
        if ((uint32_t)r < dlen && (dc & 4))
            post(da, 13u << 24 | (dlen - (uint32_t)r), EV(32) | 1u << 16 | sid << 24);
    }
    if (setup[0] == 0 && setup[1] == 5) mfault("SET_ADDRESS sent by the driver (the controller sends it)", 0);
    ring_next(s, 1, &a);
    s->ep[1].deq = a;
    if (mr32(sta + 12) & (1u << 5)) post(sta, 1u << 24, EV(32) | 1u << 16 | sid << 24);
    return 1;
}

/* an interrupt IN endpoint: a TD answered when the device has a report */
static int run_bulk_in(unsigned sid, unsigned dci, uint32_t a, uint32_t want);
static int run_in(unsigned sid, unsigned dci)
{
    struct mslot *s = &M.slot[sid];
    if (s->ep[dci].state != 1) return 0;
    uint32_t a = s->ep[dci].deq, c = mr32(a + 12);
    if ((int)(c & 1) != s->ep[dci].cyc) return 0;
    if ((c >> 10 & 63) != 1) { mfault("a TD that is not a normal TRB", c); return 0; }
    struct mdev *d = s->dev;
    if (!d || d->gone) {
        post(a, 4u << 24, EV(32) | dci << 16 | sid << 24);
        s->ep[dci].state = 2;
        return 1;
    }
    unsigned char r[8];
    unsigned len;
    uint32_t want = mr32(a + 8) & 0x1FFFF;
    if (d->kind == MD_MSC) return run_bulk_in(sid, dci, a, want);
    if (d->kind == MD_HUB) {
        uint16_t bits = 0;
        for (int p = 0; p < d->nports; p++) if (d->port[p].chg) bits |= (uint16_t)(1u << (p + 1));
        if (!bits) return 0;
        r[0] = (unsigned char)bits; r[1] = (unsigned char)(bits >> 8);
        len = d->nports >= 8 ? 2 : 1;
    } else if (!rq_pop(d, r, &len))
        return 0;
    if (len > want) len = want;
    uint32_t ta = a;
    mwr(mr32(a), r, len);
    ring_next(s, dci, &a);
    s->ep[dci].deq = a;
    uint32_t cc = len < want ? 13 : 1;
    post(ta, cc << 24 | (want - len), EV(32) | dci << 16 | sid << 24);
    return 1;
}

/* ---- the stick: SCSI over bulk-only ---- */
static void msc_csw(struct mdev *d, uint32_t tag, uint32_t residue, int status)
{
    uint32_t sig = d->bad_sig ? 0x12345678u : 0x53425355u;
    if (d->bad_tag) tag ^= 0x55;
    if (d->phase_err) status = 2;
    for (int i = 0; i < 4; i++) {
        d->csw[i] = (unsigned char)(sig >> (8 * i));
        d->csw[4 + i] = (unsigned char)(tag >> (8 * i));
        d->csw[8 + i] = (unsigned char)(residue >> (8 * i));
    }
    d->csw[12] = (unsigned char)status;
    d->bot = d->never_csw ? 3 : 2;      /* 3: the CSW never comes */
}
static void sense_set(struct mdev *d, int key, int asc, int ascq) { d->sense[0] = (unsigned char)key; d->sense[1] = (unsigned char)asc; d->sense[2] = (unsigned char)ascq; }
static uint32_t rbe(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wbe(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

/* a CBW arrived on the OUT endpoint */
static void msc_cbw(struct mdev *d, const unsigned char *w, unsigned n)
{
    uint32_t sig = w[0] | w[1] << 8 | w[2] << 16 | (uint32_t)w[3] << 24, tag = w[4] | w[5] << 8 | w[6] << 16 | (uint32_t)w[7] << 24;
    uint32_t xlen = w[8] | w[9] << 8 | w[10] << 16 | (uint32_t)w[11] << 24;
    if (n != 31 || sig != 0x43425355u || w[14] < 1 || w[14] > 16) { mfault("a CBW that is not one", sig); d->halt_in = d->halt_out = 1; return; }
    const unsigned char *c = w + 15;
    unsigned char *o = d->data;
    uint32_t len = 0;
    int status = 0;
    d->doff = 0;
    switch (c[0]) {
    case 0x12:                          /* INQUIRY */
        memset(o, 0, 36); o[0] = (unsigned char)d->devtype; o[4] = 31;
        memcpy(o + 8, "Corsair Voyager GTX     ", 24);
        len = 36;
        break;
    case 0x00:                          /* TEST UNIT READY */
        if (d->no_medium) { status = 1; sense_set(d, 2, 0x3A, 0); }
        else if (d->not_ready > 0) { d->not_ready--; status = 1; sense_set(d, 2, 4, 1); }
        break;
    case 0x03:                          /* REQUEST SENSE */
        memset(o, 0, 18); o[0] = 0x70; o[2] = d->sense[0]; o[7] = 10; o[12] = d->sense[1]; o[13] = d->sense[2];
        sense_set(d, 0, 0, 0);
        len = 18;
        break;
    case 0x25:                          /* READ CAPACITY (10) */
        memset(o, 0, 8);
        wbe(o, d->cap16 ? 0xFFFFFFFFu : d->sectors - 1); wbe(o + 4, d->blocksize);
        len = 8;
        break;
    case 0x9E:                          /* READ CAPACITY (16) */
        if ((c[1] & 31) != 0x10) { status = 1; sense_set(d, 5, 0x24, 0); break; }
        memset(o, 0, 32);
        wbe(o, d->cap16 == 3 ? 0xFFFFFFFFu : d->cap16 > 1 ? 1 : 0);      /* 3: an impossible size (the last LBA wraps) */
        wbe(o + 4, d->cap16 == 3 ? 0xFFFFFFFFu : d->cap16 > 1 ? 0 : d->sectors - 1); wbe(o + 8, d->blocksize);
        len = 32;
        break;
    case 0x28: {                        /* READ (10) */
        uint32_t lba = rbe(c + 2), cnt = (uint32_t)c[7] << 8 | c[8];
        d->reads++;
        if (d->read_fail > 0) { d->read_fail--; status = 1; sense_set(d, 3, 0x11, 0); break; }
        if (lba + cnt > d->sectors || cnt * 512 > 65536) { status = 1; sense_set(d, 5, 0x21, 0); break; }
        if (d->stall_data) { d->halt_in = 1; msc_csw(d, tag, xlen, 1); sense_set(d, 3, 0x11, 0); return; }
        for (uint32_t i = 0; i < cnt; i++) {
            unsigned char *sec = o + 512 * i;
            if (d->imgfd < 0 || pread(d->imgfd, sec, 512, (off_t)(lba + i) * 512) != 512) {
                memset(sec, 0, 512); wbe(sec, lba + i);
            }
        }
        len = cnt * 512;
        break;
    }
    default:
        status = 1; sense_set(d, 5, 0x20, 0);
    }
    if (status) len = 0;
    if (len > xlen) len = xlen;
    d->dlen = len + (d->babble && len ? 64 : 0);        /* a babbler sends more than was asked */
    if (d->dlen) { d->bot = 1; d->csw[0] = 0; msc_csw(d, tag, xlen - len, status); d->bot = 1; }
    else msc_csw(d, tag, xlen, status);
}

/* the bulk IN endpoint: data, then the CSW */
static int run_bulk_in(unsigned sid, unsigned dci, uint32_t a, uint32_t want)
{
    struct mslot *s = &M.slot[sid];
    struct mdev *d = s->dev;
    if (d->halt_in) {
        post(a, 6u << 24 | want, EV(32) | dci << 16 | sid << 24);
        s->ep[dci].state = 2;
        return 1;
    }
    const unsigned char *src;
    uint32_t len;
    if (d->bot == 1) { src = d->data + d->doff; len = d->dlen - d->doff; }
    else if (d->bot == 2) { src = d->csw; len = 13; }
    else return 0;                      /* nothing to send: NAK */
    unsigned mps = (unsigned)s->ep[dci].mps ? (unsigned)s->ep[dci].mps : 64;
    if (s->ep[dci].tog != d->tog_in) {  /* the host takes the packets as repeats and drops them; the device thinks they
                                         * arrived: they are lost, and the TD waits on */
        uint32_t l = len > want ? want : len, np = (l + mps - 1) / mps + !l;
        d->tog_in ^= (int)(np & 1);
        d->dropped++;
        if (d->bot == 1) { d->doff += l; if (d->doff >= d->dlen) d->bot = d->never_csw ? 3 : 2; }
        else d->bot = 0;
        return 1;
    }
    if (len > want) {
        if (len - want >= 64 || d->bot == 2) {          /* more than the TD holds: babble (USB 2.0 8.4.4) */
            post(a, 3u << 24 | want, EV(32) | dci << 16 | sid << 24);
            s->ep[dci].state = 2;
            d->bot = 0;
            return 1;
        }
        len = want;
    }
    mwr(mr32(a), src, len);
    {
        uint32_t np = (len + mps - 1) / mps + !len;
        d->tog_in ^= (int)(np & 1);
        s->ep[dci].tog ^= (int)(np & 1);
    }
    if (d->bot == 1) { d->doff += len; if (d->doff >= d->dlen) d->bot = d->never_csw ? 3 : 2; }
    else d->bot = 0;
    uint32_t ta = a;
    ring_next(s, dci, &a);
    s->ep[dci].deq = a;
    post(ta, (len < want ? 13u : 1u) << 24 | (want - len), EV(32) | dci << 16 | sid << 24);
    return 1;
}

/* the bulk OUT endpoint: a CBW */
static int run_out(unsigned sid, unsigned dci)
{
    struct mslot *s = &M.slot[sid];
    if (s->ep[dci].state != 1) return 0;
    uint32_t a = s->ep[dci].deq, c = mr32(a + 12);
    if ((int)(c & 1) != s->ep[dci].cyc) return 0;
    struct mdev *d = s->dev;
    uint32_t len = mr32(a + 8) & 0x1FFFF;
    if (!d || d->gone) { post(a, 4u << 24, EV(32) | dci << 16 | sid << 24); s->ep[dci].state = 2; return 1; }
    if (d->halt_out) { post(a, 6u << 24 | len, EV(32) | dci << 16 | sid << 24); s->ep[dci].state = 2; return 1; }
    unsigned char w[64];
    mrd(mr32(a), w, len > 64 ? 64 : len);
    unsigned mps = (unsigned)s->ep[dci].mps ? (unsigned)s->ep[dci].mps : 64;
    uint32_t np = (len + mps - 1) / mps + !len;
    if (s->ep[dci].tog != d->tog_out) d->dropped++;    /* the device takes it as a repeat: ACKed and thrown away */
    else {
        if (d->bot != 0 && d->bot != 3) mfault("a CBW while the last command is not done", (uint32_t)d->bot);
        msc_cbw(d, w, len);
        d->tog_out ^= (int)(np & 1);
    }
    s->ep[dci].tog ^= (int)(np & 1);
    uint32_t ta = a;
    ring_next(s, dci, &a);
    s->ep[dci].deq = a;
    post(ta, 1u << 24, EV(32) | dci << 16 | sid << 24);
    return 1;
}

static void model_tick(void)
{
    if (M.usbsts & 1) return;
    for (unsigned i = 1; i <= M_SLOTS; i++) {
        if (!M.slot[i].on) continue;
        if (M.slot[i].ep[1].state == 1) while (run_control(i)) ;
        for (unsigned dci = 2; dci < 32; dci++) (dci & 1 ? run_in : run_out)(i, dci);
    }
}

/* ---- the root port ---- */
static void port_event(unsigned p)
{
    if (!(M.usbsts & 1)) post((uint32_t)p << 24, 1u << 24, EV(34));
}
static void model_attach(unsigned p, struct mdev *d)
{
    M.root[p] = d;
    d->parent = 0;
    M.portsc[p] |= 1u | 1u << 17;
    port_event(p);
}
static void model_detach(unsigned p)
{
    if (M.root[p]) M.root[p]->gone = 1;
    M.root[p] = 0;
    M.portsc[p] = (M.portsc[p] & ~3u) | 1u << 17;
    port_event(p);
}
static void hub_attach(struct mdev *h, int p, struct mdev *d)
{
    h->port[p - 1].dev = d;
    d->parent = h;
    d->parent_port = p;
    d->gone = 0;
    h->port[p - 1].st |= 1;
    h->port[p - 1].chg |= 1;
}
static void hub_detach(struct mdev *h, int p)
{
    if (h->port[p - 1].dev) h->port[p - 1].dev->gone = 1;
    h->port[p - 1].dev = 0;
    h->port[p - 1].st &= ~3;
    h->port[p - 1].chg |= 1;
}

/* ---- registers ---- */
static uint32_t model_rd(uint32_t off)
{
    uint32_t op = M_CAPLEN;
    if (M.allones) return 0xFFFFFFFFu;
    switch (off) {
    case 0x00: return M_CAPLEN | 0x0100u << 16;
    case 0x04: return M.maxslots | 1u << 8 | (uint32_t)M_PORTS << 24;
    case 0x08: return (M.nscratch & 31) << 27 | (M.nscratch >> 5) << 21;
    case 0x10: return M.csz == 64 ? 4 : 0;
    case 0x14: return M_DB;
    case 0x18: return M_RTS;
    }
    if (off == op) return M.usbcmd;
    if (off == op + 4) {
        if (M.cnr_ticks > 0) return M.usbsts | 0x800;
        return M.usbsts;
    }
    if (off == op + 8) return 1;
    if (off == op + 0x38) return M.config;
    if (off >= op + 0x400 && off < op + 0x400 + 0x10 * M_PORTS && !((off - op - 0x400) & 15))
        return M.portsc[(off - op - 0x400) / 16 + 1];
    if (off == M_RTS + 0x20) return M.iman;
    return 0;
}

static void model_wr(uint32_t off, uint32_t v)
{
    uint32_t op = M_CAPLEN;
    if (off == op) {
        if (v & 2) {                    /* HCRST */
            uint32_t keep_ns = M.nscratch, ks = M.maxslots, kc = M.csz;
            for (int i = 1; i <= M_SLOTS; i++) M.slot[i].on = 0;
            M.usbcmd = 0; M.usbsts = 1; M.config = 0; M.dcbaap = 0; M.cmd_set = 0; M.ev_base = 0; M.erstsz = 0;
            M.nscratch = keep_ns; M.maxslots = ks; M.csz = kc;
            M.cnr_ticks = 2;
            for (unsigned p = 1; p <= M_PORTS; p++)     /* a reset does not unplug: the device is seen again */
                M.portsc[p] = M.root[p] ? (1u | 1u << 9 | 1u << 17) : 1u << 9;
            return;
        }
        if ((v & 1) && !(M.usbcmd & 1)) {
            if (M.cnr_ticks > 0) mfault("Run/Stop while the controller is not ready", v);
            if (!M.dcbaap || !M.cmd_set || !M.ev_base) mfault("Run/Stop without the DCBAA, the command ring or the event ring", v);
            if (M.nscratch && !mr32(M.dcbaap)) mfault("no scratchpad array in DCBAA[0]", 0);
            M.usbsts &= ~1u;
        }
        if (!(v & 1)) M.usbsts |= 1;
        M.usbcmd = v & ~2u;
        return;
    }
    if (off == op + 4) { M.usbsts &= ~(v & 0x41C); return; }
    if (off == op + 0x18) {
        if (!(M.usbsts & 1)) mfault("CRCR written while running", v);
        M.cmd_deq = v & ~63u; M.cmd_ccs = (int)(v & 1); M.cmd_set = 1;
        in_page(M.cmd_deq, 16, "the command ring");
        return;
    }
    if (off == op + 0x30) {
        M.dcbaap = v;
        if (v & 63) mfault("a DCBAA not 64-byte aligned", v);
        in_page(v, 8 * (M.config + 1), "the DCBAA crossing a page");
        return;
    }
    if (off == op + 0x38) { M.config = v & 0xFF; return; }
    if (off == op + 0x1C || off == op + 0x34) { if (v) mfault("a high address word not 0", v); return; }
    if (off >= op + 0x400 && off < op + 0x400 + 0x10 * M_PORTS && !((off - op - 0x400) & 15)) {
        unsigned p = (off - op - 0x400) / 16 + 1;
        uint32_t *ps = &M.portsc[p];
        if (v & 2) mfault("PORTSC written with PED 1 (disables the port)", v);
        *ps &= ~(v & (0x7Fu << 17));    /* change bits: write 1 to clear */
        if (v & (1u << 9)) *ps |= 1u << 9;
        if ((v & 16) && (*ps & 1)) {    /* a reset: enabled, its speed, PRC */
            struct mdev *d = M.root[p];
            *ps = (*ps & ~(15u << 10)) | 2u | (uint32_t)(d ? d->speed : 1) << 10 | 1u << 21;
            port_event(p);
        }
        return;
    }
    if (off == M_RTS + 0x20) { M.iman = (M.iman & ~(v & 1) & ~2u) | (v & 2); if (v & 2) mfault("interrupts enabled (usbd polls)", v); return; }
    if (off == M_RTS + 0x24) { M.imod = v; return; }
    if (off == M_RTS + 0x28) { M.erstsz = v; return; }
    if (off == M_RTS + 0x30) {
        M.erstba = v;
        if (M.erstsz != 1) mfault("ERSTBA before ERSTSZ, or not one segment", M.erstsz);
        M.ev_base = mr32(v) & ~63u; M.ev_n = mr32(v + 8) & 0xFFFF;
        M.ev_enq = M.ev_base; M.ev_pcs = 1;
        if (M.ev_n < 16) mfault("an event ring segment under 16 TRBs", M.ev_n);
        in_page(M.ev_base, M.ev_n * 16, "the event ring crossing a page");
        return;
    }
    if (off == M_RTS + 0x38) { M.erdp = v & ~8u; return; }
    if (off == M_RTS + 0x34 || off == M_RTS + 0x3C) { if (v) mfault("a high address word not 0", v); return; }
    if (off >= M_DB && off < M_DB + 4 * (M_SLOTS + 1)) {
        unsigned sid = (off - M_DB) / 4;
        if (M.usbsts & 1) { mfault("a doorbell while halted", off); return; }
        if (!sid) { run_commands(); return; }
        if (!M.slot[sid].on) { mfault("a doorbell for a slot never enabled", sid); return; }
        unsigned dci = v & 31;
        if (dci == 1) { if (M.slot[sid].ep[1].state == 1) while (run_control(sid)) ; }
        else if (dci & 1) run_in(sid, dci);
        else { run_out(sid, dci); model_tick(); }
        return;
    }
}

static void model_reset(unsigned nscratch, unsigned csz)
{
    int ef = M.expect_faults, q = M.quiet;
    memset(&M, 0, sizeof M);
    M.expect_faults = ef;
    M.quiet = q;
    M.maxslots = 32;
    M.nscratch = nscratch;
    M.csz = csz;
    M.usbsts = 1;
    for (unsigned p = 1; p <= M_PORTS; p++) M.portsc[p] = 1u << 9;
}

/* ---- device factory ---- */
static void mdev_init(struct mdev *d, const char *name, int kind, int speed, unsigned vid, unsigned pid)
{
    memset(d, 0, sizeof *d);
    d->name = name;
    d->kind = kind;
    d->speed = speed;
    unsigned char dd[18] = { 18, 1, 0x00, 0x02, kind == MD_HUB ? 9 : 0, 0, kind == MD_HUB ? 1 : 0,
                             speed == 3 ? 64 : 8, (unsigned char)vid, (unsigned char)(vid >> 8), (unsigned char)pid,
                             (unsigned char)(pid >> 8), 0, 1, 1, 2, 0, 1 };
    memcpy(d->dd, dd, 18);
    unsigned char *c = d->cfg;
    unsigned n = 0;
    unsigned char cf[9] = { 9, 2, 0, 0, 1, 1, 0, 0xA0, 50 };
    memcpy(c, cf, 9); n = 9;
    if (kind == MD_HUB) {
        unsigned char i[9] = { 9, 4, 0, 0, 1, 9, 0, 0, 0 }, e[7] = { 7, 5, 0x81, 3, 1, 0, 12 };
        memcpy(c + n, i, 9); n += 9; memcpy(c + n, e, 7); n += 7;
        d->nports = d->hubdesc_ports = 4;
    } else if (kind == MD_KBD || kind == MD_MOUSE) {
        unsigned char i[9] = { 9, 4, 0, 0, 1, 3, 1, (unsigned char)(kind == MD_KBD ? 1 : 2), 0 };
        unsigned char h[9] = { 9, 0x21, 0x11, 1, 0, 1, 0x22, 63, 0 };
        unsigned char e[7] = { 7, 5, 0x81, 3, (unsigned char)(kind == MD_KBD ? 8 : 4), 0, 10 };
        memcpy(c + n, i, 9); n += 9; memcpy(c + n, h, 9); n += 9; memcpy(c + n, e, 7); n += 7;
    } else if (kind == MD_MSC) {        /* a stick: class 8, SCSI 6, bulk-only 0x50; bulk IN 0x81 and OUT 0x02 */
        unsigned mps = speed == 3 ? 512 : 64;
        unsigned char i[9] = { 9, 4, 0, 0, 2, 8, 6, 0x50, 0 };
        unsigned char e1[7] = { 7, 5, 0x81, 2, (unsigned char)mps, (unsigned char)(mps >> 8), 0 };
        unsigned char e2[7] = { 7, 5, 0x02, 2, (unsigned char)mps, (unsigned char)(mps >> 8), 0 };
        memcpy(c + n, i, 9); n += 9; memcpy(c + n, e1, 7); n += 7; memcpy(c + n, e2, 7); n += 7;
        d->imgfd = -1; d->blocksize = 512; d->sectors = 20000;
    } else {
        unsigned char i[9] = { 9, 4, 0, 0, 0, 0xFF, 0, 0, 0 };   /* a vendor-specific device: no driver */
        memcpy(c + n, i, 9); n += 9;
    }
    c[2] = (unsigned char)n; c[3] = (unsigned char)(n >> 8);
    d->cfglen = n;
}
