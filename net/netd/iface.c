/* SPDX-License-Identifier: GPL-2.0-only */
/* netd's interfaces (DESIGN D19 2.1, 2.3, 2.4, 5.3, 6): SLIP over a serial port and the loopback live inside netd; an
 * Ethernet card's driver is a process of its own, netd's client (the driver protocol: its side is the second half of
 * this file; the processes, the doorbell and the supervision are netd's main program's).
 * SLIP is lwIP's slipif, fed from netd's own buffers: netd never waits on the port (5.3). It reads what has come
 * (at most a turn's worth), and every frame lwIP sends goes into the interface's send queue, which is written out
 * only as far as the port takes it now (Zenix's /dev/ttyS0 reports its free room truthfully, 5.3); a frame that does
 * not fit the queue is refused (ERR_MEM: TCP sends it again, UDP reports ENOBUFS). */
#include "lwip/ip4.h"
#include "lwip/ip.h"
#include "lwip/dns.h"
#include "lwip/sio.h"
#include "lwip/pbuf.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip4.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "netif/ethernet.h"
#include "netif/slipif.h"
#include "netd_int.h"

#define SLIP_TXQ 4096           /* the send queue: two frames of 1006 bytes at their worst (every byte escaped) */
#define SLIP_RXBUF 512
#define TURN_READ 2048          /* bytes read from one port in one turn: the turn stays short (5.3) */

struct drvif;
struct nif {
    struct netif netif;
    int used, handle;                   /* handle: SLIP's serial port */
    struct drvif *drv;                  /* a driver's interface (Ethernet); 0 for SLIP */
    unsigned char dhcp, leased;         /* its address comes from DHCP; the lease is logged */
    u32_t dhcp_dns[2];                  /* the name servers the DHCP server named (network order) */
    char name[NETD_IFNAME];
    netif_output_fn slip_output;        /* slipif's own output, called after the room check */
    unsigned char rx[SLIP_RXBUF];
    unsigned rxpos, rxlen;
    unsigned char *txq;
    unsigned long txn;
    int overflow;
    unsigned long rx_frames, tx_frames, rx_bytes, tx_bytes, tx_drop, rx_bad, rx_drop;
};
static struct nif nifs[NETD_IFMAX];

static int is_loop(const struct netif *n) { return n->name[0] == 'l' && n->name[1] == 'o'; }

static struct nif *nif_of(struct netif *n)
{
    int i;
    for (i = 0; i < NETD_IFMAX; i++) if (nifs[i].used && &nifs[i].netif == n) return &nifs[i];
    return NULL;
}

/* ---- lwIP's serial layer (sio.h) over the buffers ---- */
sio_fd_t sio_open(u8_t devnum)
{
    if (devnum >= NETD_IFMAX || !nifs[devnum].used) return NULL;
    return &nifs[devnum];
}
void sio_send(u8_t c, sio_fd_t fd)
{
    struct nif *f = fd;
    if (f->txn >= SLIP_TXQ) { f->overflow = 1; return; }    /* cannot happen after the room check in slip_out */
    f->txq[f->txn++] = c;
}
u32_t sio_tryread(sio_fd_t fd, u8_t *data, u32_t len)
{
    struct nif *f = fd;
    u32_t n = 0;
    while (n < len && f->rxpos < f->rxlen) data[n++] = f->rx[f->rxpos++];
    return n;
}
u32_t sio_read(sio_fd_t fd, u8_t *data, u32_t len) { return sio_tryread(fd, data, len); }

static err_t slip_out(struct netif *n, struct pbuf *p, const ip4_addr_t *ip)
{
    struct nif *f = nif_of(n);
    err_t e;
    if (!f) return ERR_IF;
    if (f->txn + 2ul * p->tot_len + 2 > SLIP_TXQ) { f->tx_drop++; return ERR_MEM; }
    e = f->slip_output(n, p, ip);
    if (e == ERR_OK) { f->tx_frames++; f->tx_bytes += p->tot_len; }
    return e;
}
/* SLIP has no checksum of its own (RFC 1055): a byte changed, lost or added on the line, or noise between two ENDs,
 * reaches netd as a frame. So each one is checked here first (D19 8.2, "noise on the line"): a whole IPv4 packet (its
 * header's checksum, its length the frame's) and, unless it is a fragment, its TCP, UDP or ICMP checksum. A damaged
 * frame is counted (ifconfig's "bad") and dropped. lwIP checks the same sums again: at SLIP's 11.5 KB/s the second
 * sum costs little, and it keeps lwIP unchanged */
static int frame_ok(struct pbuf *p)
{
    unsigned char h[60];
    unsigned hl, tl, proto, ok = 1;
    ip4_addr_t src, dst;
    if (p->tot_len < IP_HLEN || pbuf_copy_partial(p, h, IP_HLEN, 0) != IP_HLEN || (h[0] >> 4) != 4) return 0;
    hl = (h[0] & 15) * 4u;
    tl = (unsigned)h[2] << 8 | h[3];
    if (hl < IP_HLEN || tl < hl || tl != p->tot_len || pbuf_copy_partial(p, h, (u16_t)hl, 0) != hl) return 0;
    if (inet_chksum(h, (u16_t)hl) != 0) return 0;
    if ((h[6] & 0x3f) || h[7]) return 1;            /* a fragment: its transport sum is checked after reassembly */
    proto = h[9];
    if (proto != IP_PROTO_TCP && proto != IP_PROTO_UDP && proto != IP_PROTO_ICMP) return 1;
    if (proto == IP_PROTO_UDP && tl - hl >= 8 && !pbuf_get_at(p, (u16_t)(hl + 6)) && !pbuf_get_at(p, (u16_t)(hl + 7))) return 1;
    memcpy(&src, h + 12, 4);
    memcpy(&dst, h + 16, 4);
    pbuf_remove_header(p, hl);
    if (proto == IP_PROTO_ICMP) ok = inet_chksum_pbuf(p) == 0;
    else ok = inet_chksum_pseudo(p, (u8_t)proto, (u16_t)(tl - hl), &src, &dst) == 0;
    pbuf_add_header(p, hl);
    return ok;
}
static err_t slip_in(struct pbuf *p, struct netif *n)
{
    struct nif *f = nif_of(n);
    if (f && !frame_ok(p)) { f->rx_bad++; pbuf_free(p); return ERR_OK; }
    if (f) { f->rx_frames++; f->rx_bytes += p->tot_len; }
    return ip_input(p, n);
}
static err_t drop_in(struct pbuf *p, struct netif *n) { (void)n; pbuf_free(p); return ERR_OK; }

static void tx_flush(struct nif *f)
{
    long w;
    if (!f->txn) return;
    w = netd_os_write(f->handle, f->txq, f->txn);
    if (w <= 0) return;
    if ((unsigned long)w > f->txn) w = (long)f->txn;
    memmove(f->txq, f->txq + w, f->txn - (unsigned long)w);
    f->txn -= (unsigned long)w;
}

void netd_if_init(void) { memset(nifs, 0, sizeof nifs); }

void netd_if_turn(void)
{
    int i;
    for (i = 0; i < NETD_IFMAX; i++) {
        struct nif *f = &nifs[i];
        unsigned got = 0;
        long r;
        if (!f->used || f->drv) continue;
        tx_flush(f);
        while (got < TURN_READ) {
            r = netd_os_read(f->handle, f->rx, SLIP_RXBUF);
            if (r <= 0) break;
            f->rxpos = 0;
            f->rxlen = (unsigned)r;
            got += (unsigned)r;
            slipif_poll(&f->netif);
            f->rxpos = f->rxlen = 0;
        }
        tx_flush(f);
    }
}

int netd_if_handles(int *h, int *want_write, int max)
{
    int i, n = 0;
    for (i = 0; i < NETD_IFMAX && n < max; i++)
        if (nifs[i].used && !nifs[i].drv) { h[n] = nifs[i].handle; want_write[n] = nifs[i].txn != 0; n++; }
    return n;
}

static int parse_slip_args(const char *args, char *dev, unsigned devmax, unsigned long *baud)
{
    unsigned n = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ') { if (n + 1 >= devmax) return -EINVAL; dev[n++] = *args++; }
    dev[n] = 0;
    if (!n) return -EINVAL;
    while (*args == ' ') args++;
    *baud = 115200;
    if (*args) {
        unsigned long b = 0;
        while (*args >= '0' && *args <= '9') b = b * 10 + (unsigned long)(*args++ - '0');
        if (*args || !b) return -EINVAL;
        *baud = b;
    }
    return 0;
}

int netd_if_up(const struct netd_ifcfg *c)
{
    struct nif *f = NULL;
    char dev[48];
    unsigned long baud;
    ip4_addr_t a, m, g;
    int i, h;
    for (i = 0; i < NETD_IFMAX; i++) if (nifs[i].used && !strcmp(nifs[i].name, c->name)) return -EEXIST;
    if (strcmp(c->driver, "slip")) return -ENODEV;          /* drivers' interfaces: the driver protocol (step 7) */
    if (c->method != NETD_INET_STATIC || !c->addr) return -EINVAL;   /* SLIP has no DHCP: a static address */
    if (c->mtu && (c->mtu < 68 || c->mtu > SLIP_MAX_SIZE)) return -EINVAL;
    if (parse_slip_args(c->args, dev, sizeof dev, &baud)) return -EINVAL;
    for (i = 0; i < NETD_IFMAX; i++) if (!nifs[i].used) { f = &nifs[i]; break; }
    if (!f) return -ENOSPC;
    memset(f, 0, sizeof *f);
    f->txq = netd_malloc(SLIP_TXQ);
    if (!f->txq) return -ENOMEM;
    h = netd_os_serial_open(dev, baud);
    if (h < 0) { netd_free(f->txq); f->txq = NULL; return h; }
    f->handle = h;
    f->used = 1;
    strncpy(f->name, c->name, NETD_IFNAME - 1);
    ip4_addr_set_u32(&a, c->addr);
    ip4_addr_set_u32(&m, c->mask);
    ip4_addr_set_u32(&g, c->gateway);
    if (!netif_add(&f->netif, &a, &m, &g, (void *)(mem_ptr_t)(f - nifs), slipif_init, slip_in)) {
        netd_os_close(h);
        netd_free(f->txq);
        f->used = 0;
        return -ENOMEM;
    }
    f->slip_output = f->netif.output;
    f->netif.output = slip_out;
    if (c->mtu) f->netif.mtu = c->mtu;
#if LWIP_NETIF_HOSTNAME
    if (c->hostname[0]) {
        static char hn[NETD_IFMAX][32];
        strncpy(hn[f - nifs], c->hostname, 31);
        netif_set_hostname(&f->netif, hn[f - nifs]);
    }
#endif
    netif_set_up(&f->netif);
    netif_set_link_up(&f->netif);
    if (c->gateway || c->ptp) netd_route_default(c->gateway ? c->gateway : c->ptp);
    netd_logf("netd: %s up: SLIP on %s at %lu, %u.%u.%u.%u", f->name, dev, baud,
              ip4_addr1_16(&a), ip4_addr2_16(&a), ip4_addr3_16(&a), ip4_addr4_16(&a));
    return 0;
}

int netd_if_down(const char *name)
{
    int i;
    for (i = 0; i < NETD_IFMAX; i++) {
        struct nif *f = &nifs[i];
        if (!f->used || strcmp(f->name, name)) continue;
        if (f->drv) return -EBUSY;          /* a driver's: netd_drv_gone (its main program stops the driver) */
        /* a frame half received: ENDs hand it to an input that drops it, so slipif frees its buffers; two, because
         * after an ESC slipif takes the first END as data (found by tools/host/lwip-fuzz: two pbufs lost) */
        f->netif.input = drop_in;
        f->rx[0] = f->rx[1] = 0xC0;
        f->rxpos = 0;
        f->rxlen = 2;
        slipif_poll(&f->netif);
        netif_set_down(&f->netif);
        /* packets to itself wait in the interface's loopback queue, which netif_remove leaves behind (found by
         * tools/host/lwip-fuzz): with the interface down they are only dropped */
        netif_poll(&f->netif);
        netif_remove(&f->netif);            /* ends the connections bound to its address (6.2) */
        mem_free(f->netif.state);           /* slipif's own data (it has no function to free it) */
        netd_os_close(f->handle);
        netd_free(f->txq);
        f->used = 0;
        netd_logf("netd: %s down", name);
        return 0;
    }
    return -ENOENT;
}

int netd_route_default(unsigned long gw)
{
    struct netif *n;
    ip4_addr_t g;
    if (!gw) { netif_set_default(NULL); return 0; }
    ip4_addr_set_u32(&g, gw);
    NETIF_FOREACH(n) {
        if (!netif_is_up(n) || is_loop(n)) continue;
        if (ip4_addr_net_eq(&g, netif_ip4_addr(n), netif_ip4_netmask(n)) || ip4_addr_isany_val(*netif_ip4_netmask(n))) {
            netif_set_gw(n, &g);
            netif_set_default(n);
            return 0;
        }
    }
    /* SLIP is point to point: the far end may be outside the address's net */
    NETIF_FOREACH(n) {
        if (netif_is_up(n) && nif_of(n)) { netif_set_gw(n, &g); netif_set_default(n); return 0; }
    }
    return -ENETUNREACH;
}

int netd_route_get(unsigned long *gw, char name[NETD_IFNAME])
{
    struct netif *n = netif_default;
    struct nif *f;
    *gw = 0;
    name[0] = 0;
    if (!n || !netif_is_up(n)) return 0;
    *gw = ip4_addr_get_u32(netif_ip4_gw(n));
    f = nif_of(n);
    if (f) strncpy(name, f->name, NETD_IFNAME - 1);
    else if (is_loop(n)) strcpy(name, "lo");
    return 0;
}

int netd_if_addr(const char *name, unsigned long addr, unsigned long mask)
{
    int i;
    ip4_addr_t a, m;
    for (i = 0; i < NETD_IFMAX; i++) {
        struct nif *f = &nifs[i];
        if (!f->used || strcmp(f->name, name)) continue;
        ip4_addr_set_u32(&a, addr);
        if (mask) ip4_addr_set_u32(&m, mask); else ip4_addr_copy(m, *netif_ip4_netmask(&f->netif));
        netif_set_addr(&f->netif, &a, &m, netif_ip4_gw(&f->netif));  /* ends the connections on the old address (6.2) */
        netd_logf("netd: %s: address %u.%u.%u.%u", f->name, ip4_addr1_16(&a), ip4_addr2_16(&a), ip4_addr3_16(&a),
                  ip4_addr4_16(&a));
        return 0;
    }
    return -ENOENT;
}

/* the name servers lwIP asks: /etc/resolv.conf's when it names any, else those the first interface's DHCP server
 * named (6.1: DHCP's stay in netd's memory, nothing is written behind the owner's back) */
static unsigned long resolv_dns[2];
static int resolv_n;
static void dns_apply(void)
{
    u32_t d[2] = { 0, 0 };
    int i, n = 0;
    if (resolv_n) { d[0] = resolv_dns[0]; d[1] = resolv_dns[1]; n = resolv_n; }
    else
        for (i = 0; i < NETD_IFMAX && !n; i++)
            if (nifs[i].used && nifs[i].dhcp_dns[0]) { d[0] = nifs[i].dhcp_dns[0]; d[1] = nifs[i].dhcp_dns[1]; n = d[1] ? 2 : 1; }
    for (i = 0; i < DNS_MAX_SERVERS; i++) {
        ip_addr_t a;
        ip_addr_set_ip4_u32_val(a, i < n ? d[i] : 0);
        dns_setserver((u8_t)i, i < n ? &a : NULL);
    }
}
void netd_dns_servers(unsigned long out[2])
{
    int i;
    for (i = 0; i < 2; i++) {
        const ip_addr_t *a = dns_getserver((u8_t)i);
        out[i] = a ? ip4_addr_get_u32(ip_2_ip4(a)) : 0;
    }
}
int netd_set_dns(const unsigned long *servers, int n)
{
    resolv_n = n < 0 ? 0 : n > 2 ? 2 : n;
    resolv_dns[0] = resolv_n > 0 ? servers[0] : 0;
    resolv_dns[1] = resolv_n > 1 ? servers[1] : 0;
    dns_apply();
    return 0;
}

int netd_if_info(int i, struct netd_ifinfo *o)
{
    struct netif *n;
    struct nif *f;
    int k = 0;
    NETIF_FOREACH(n) {
        if (k++ != i) continue;
        memset(o, 0, sizeof *o);
        f = nif_of(n);
        if (f) {
            strncpy(o->name, f->name, NETD_IFNAME - 1);
            o->rx_frames = f->rx_frames; o->tx_frames = f->tx_frames;
            o->rx_bytes = f->rx_bytes; o->tx_bytes = f->tx_bytes; o->tx_drop = f->tx_drop; o->rx_bad = f->rx_bad;
            o->rx_drop = f->rx_drop;
            if (f->drv) {
                o->ether = 1;
                memcpy(o->hwaddr, n->hwaddr, 6);
            }
            if (f->dhcp) {
                struct dhcp *d = netif_dhcp_data(n);
                o->dhcp = 1;
                if (d && dhcp_supplied_address(n)) {
                    o->lease = d->t0_timeout > d->lease_used ? (unsigned long)(d->t0_timeout - d->lease_used) * DHCP_COARSE_TIMER_SECS : 0;
                    o->dhcp_server = ip4_addr_get_u32(ip_2_ip4(&d->server_ip_addr));
                }
            }
        } else if (is_loop(n)) strcpy(o->name, "lo");
        else { o->name[0] = n->name[0]; o->name[1] = n->name[1]; o->name[2] = (char)('0' + n->num % 10); }
        o->addr = ip4_addr_get_u32(netif_ip4_addr(n));
        o->mask = ip4_addr_get_u32(netif_ip4_netmask(n));
        o->gateway = ip4_addr_get_u32(netif_ip4_gw(n));
        o->mtu = n->mtu;
        o->up = netif_is_up(n);
        o->link = netif_is_link_up(n);
        return 0;
    }
    return -ENOENT;
}

int netd_addr_is_local(const ip4_addr_t *a)
{
    struct netif *n;
    if (ip4_addr_isany(a) || ip4_addr_isloopback(a)) return 1;
    NETIF_FOREACH(n) if (ip4_addr_eq(a, netif_ip4_addr(n))) return 1;
    return 0;
}

unsigned netd_route_mtu(const ip4_addr_t *dst)
{
    struct netif *n = ip4_route(dst);
    return n ? (n->mtu ? n->mtu : 0xffff) : 0;   /* the loopback has no MTU */
}

/* ==== drivers' interfaces (D19 2.3, 2.4; step 7) ====
 * netd lends a driver its receive buffers: it fills one and hands it over (netd_drv_rx), and gets it back empty
 * (netd_drv_freed). A frame received is given to lwIP without a copy (a custom pbuf whose free gives the buffer back)
 * while at most half the buffers are held by the stack; past that, and always for a DMA interface, it is copied into
 * a pool buffer and the driver's buffer goes back at once (a slow reader or TCP's out-of-order queue never starves
 * the driver, and a DMA region never outlives a frame in the stack). Frames to send are lent the other way: pieces
 * in netd's memory (each held, pbuf_ref, until the driver says it is sent: a TCP retransmit never rewrites a header
 * still in a ring), or copied into a send buffer of the DMA region for a DMA interface. One call a batch. */
#define TXQ 32                          /* frames queued or out with the driver */
enum { B_DRIVER = 1, B_STACK, B_RETURN };
enum { Q_FREE = 0, Q_WAIT, Q_OUT };

struct dbuf {
    struct pbuf_custom pc;              /* first: the custom pbuf's free finds the buffer from it */
    unsigned char *p;
    unsigned char state;
    struct drvif *d;
};
struct txslot { struct pbuf *p; unsigned char state; unsigned short len; unsigned long seq; };
struct drvif {
    int used, dead;                     /* dead: the driver ended; kept while the stack still holds its buffers */
    struct nif *f;
    unsigned nbuf, bufsize, held, dma, ntxq;
    unsigned char *mem;                 /* netd's own memory for the buffers, after this struct in its region (0: DMA) */
    unsigned char *dmatx;               /* DMA: the send buffers */
    unsigned long seq;
    int reports;                        /* wrong numbers logged so far (at most a few a driver) */
    struct dbuf b[NETD_DRV_MAXBUF];
    struct txslot tx[TXQ];
};
/* taken when a driver attaches, not in netd's BSS: a SLIP-only machine pays nothing for them */
static struct drvif *drvifs[NETD_IFMAX * 2];

static void drv_free_mem(struct drvif *d)
{
    int i;
    for (i = 0; i < NETD_IFMAX * 2; i++) if (drvifs[i] == d) drvifs[i] = NULL;
    netd_os_free(d);                    /* the struct and the buffers: one region (a process has 16, netd's heap at most 5) */
}
static void dbuf_free(struct pbuf *p)
{
    struct dbuf *b = (struct dbuf *)p;
    struct drvif *d = b->d;
    b->state = B_RETURN;
    d->held--;
    if (d->dead && !d->held) drv_free_mem(d);
}
static void drv_report(struct drvif *d, const char *what, unsigned v)
{
    if (d->f) d->f->rx_bad++;
    if (d->reports++ < 4) netd_logf("netd: %s: the driver gave %s %u: ignored", d->f ? d->f->name : "?", what, v);
}

static err_t drv_linkoutput(struct netif *n, struct pbuf *p)
{
    struct nif *f = nif_of(n);
    struct drvif *d = f ? f->drv : NULL;
    unsigned i;
    struct txslot *t = NULL;
    if (!d || d->dead) return ERR_IF;
    for (i = 0; i < d->ntxq; i++) if (d->tx[i].state == Q_FREE) { t = &d->tx[i]; break; }
    if (!t || p->tot_len > d->bufsize) { f->tx_drop++; return ERR_MEM; }    /* TCP sends it again */
    if (d->dma) {
        pbuf_copy_partial(p, d->dmatx + (unsigned long)i * d->bufsize, p->tot_len, 0);
        t->p = NULL;
    } else {
        struct pbuf *q = p;
        if (pbuf_clen(p) > 4 && !(q = pbuf_clone(PBUF_RAW, PBUF_RAM, p))) { f->tx_drop++; return ERR_MEM; }
        if (q == p) pbuf_ref(p);
        t->p = q;
    }
    t->state = Q_WAIT;
    t->len = p->tot_len;
    t->seq = ++d->seq;
    f->tx_frames++;
    f->tx_bytes += p->tot_len;
    return ERR_OK;
}
static err_t drv_netif_init(struct netif *n)
{
    n->name[0] = 'e';
    n->name[1] = 'n';
    n->output = etharp_output;
    n->linkoutput = drv_linkoutput;
    n->hwaddr_len = ETH_HWADDR_LEN;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

int netd_drv_attach(const struct netd_ifcfg *c, const struct netd_drvattach *a, unsigned *bufsize, unsigned *nbuf,
                    void **bufs)
{
    struct nif *f = NULL;
    struct drvif *d = NULL;
    ip4_addr_t ad, m, g;
    unsigned i, bs = 1536;              /* a frame of 1514 bytes, rounded to whole 64-byte lines */
    int slot;
    for (i = 0; i < NETD_IFMAX; i++) if (nifs[i].used && !strcmp(nifs[i].name, c->name)) return -EEXIST;
    if (a->nrx < 1 || a->nrx > NETD_DRV_MAXBUF || a->mtu < 576 || a->mtu > 1500) return -EINVAL;
    if (c->method == NETD_INET_STATIC && !c->addr) return -EINVAL;
    if (a->dma && (!a->dmamem || ((unsigned long)a->dmamem & 63) ||
                   a->dmalen < (unsigned long)(a->nrx + NETD_DRV_BATCH) * bs)) return -EINVAL;
    for (i = 0; i < NETD_IFMAX; i++) if (!nifs[i].used) { f = &nifs[i]; break; }
    for (i = 0; i < NETD_IFMAX * 2; i++) if (!drvifs[i]) break;
    if (!f || i == NETD_IFMAX * 2) return -ENOSPC;
    unsigned long head = (sizeof *d + 63) & ~63ul;
    if (!(d = netd_os_alloc(head + (a->dma ? 0 : (unsigned long)a->nrx * bs)))) return -ENOMEM;
    memset(f, 0, sizeof *f);
    memset(d, 0, sizeof *d);
    drvifs[i] = d;
    slot = (int)i;
    d->nbuf = a->nrx;
    d->bufsize = bs;
    d->dma = a->dma != 0;
    d->ntxq = d->dma ? NETD_DRV_BATCH : TXQ;
    if (d->dma) {
        d->dmatx = (unsigned char *)a->dmamem + (unsigned long)a->nrx * bs;
        for (i = 0; i < d->nbuf; i++) d->b[i].p = (unsigned char *)a->dmamem + (unsigned long)i * bs;
    } else {
        d->mem = (unsigned char *)d + head;
        for (i = 0; i < d->nbuf; i++) d->b[i].p = d->mem + (unsigned long)i * bs;
    }
    for (i = 0; i < d->nbuf; i++) {
        d->b[i].state = B_DRIVER;
        d->b[i].d = d;
        d->b[i].pc.custom_free_function = dbuf_free;
        bufs[i] = d->b[i].p;
    }
    d->used = 1;
    d->f = f;
    f->used = 1;
    f->drv = d;
    f->dhcp = c->method == NETD_INET_DHCP;
    strncpy(f->name, c->name, NETD_IFNAME - 1);
    ip4_addr_set_u32(&ad, f->dhcp ? 0 : c->addr);
    ip4_addr_set_u32(&m, f->dhcp ? 0 : c->mask);
    ip4_addr_set_u32(&g, f->dhcp ? 0 : c->gateway);
    if (!netif_add(&f->netif, &ad, &m, &g, NULL, drv_netif_init, netif_input)) {
        drv_free_mem(d);
        f->used = 0;
        return -ENOMEM;
    }
    memcpy(f->netif.hwaddr, a->mac, 6);
    f->netif.mtu = (u16_t)(c->mtu && c->mtu < a->mtu ? c->mtu : a->mtu);
#if LWIP_NETIF_HOSTNAME
    if (c->hostname[0]) {
        static char hn[NETD_IFMAX][32];
        strncpy(hn[f - nifs], c->hostname, 31);
        netif_set_hostname(&f->netif, hn[f - nifs]);
    }
#endif
    netif_set_up(&f->netif);
    if (a->link) netif_set_link_up(&f->netif);
    if (f->dhcp) {
        if (dhcp_start(&f->netif) != ERR_OK) netd_logf("netd: %s: DHCP would not start", f->name);
    } else if (c->gateway) netd_route_default(c->gateway);
    *bufsize = bs;
    *nbuf = d->nbuf;
    netd_logf("netd: %s up: %02x:%02x:%02x:%02x:%02x:%02x, %s, %u buffers%s", f->name, a->mac[0], a->mac[1], a->mac[2],
              a->mac[3], a->mac[4], a->mac[5], f->dhcp ? "DHCP" : "static", d->nbuf, d->dma ? " in its DMA memory" : "");
    return slot;
}

static struct drvif *drv_get(int h)
{
    if (h < 0 || h >= NETD_IFMAX * 2 || !drvifs[h] || drvifs[h]->dead) return NULL;
    return drvifs[h];
}

int netd_drv_rx(int h, unsigned num, unsigned len)
{
    struct drvif *d = drv_get(h);
    struct dbuf *b;
    struct pbuf *p;
    struct nif *f;
    if (!d) return -ENOENT;
    f = d->f;
    if (num >= d->nbuf || d->b[num].state != B_DRIVER) { drv_report(d, "a buffer that is not its own:", num); return -EINVAL; }
    b = &d->b[num];
    if (len < 14 || len > d->bufsize) { drv_report(d, "a wrong length:", len); b->state = B_RETURN; return -EINVAL; }
    if (d->dma || d->held * 2 >= d->nbuf) {        /* a copy: the buffer goes back in this same call */
        b->state = B_RETURN;
        if (!(p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL))) { f->rx_drop++; return 0; }
        pbuf_take(p, b->p, (u16_t)len);
    } else {
        p = pbuf_alloced_custom(PBUF_RAW, (u16_t)len, PBUF_REF, &b->pc, b->p, (u16_t)d->bufsize);
        if (!p) { b->state = B_RETURN; f->rx_drop++; return 0; }
        b->state = B_STACK;
        d->held++;
    }
    f->rx_frames++;
    f->rx_bytes += len;
    if (f->netif.input(p, &f->netif) != ERR_OK) pbuf_free(p);
    return 0;
}

int netd_drv_done(int h, unsigned tag)
{
    struct drvif *d = drv_get(h);
    if (!d) return -ENOENT;
    if (tag >= d->ntxq || d->tx[tag].state != Q_OUT) { drv_report(d, "a tag it does not hold:", tag); return -EINVAL; }
    if (d->tx[tag].p) pbuf_free(d->tx[tag].p);
    d->tx[tag].p = NULL;
    d->tx[tag].state = Q_FREE;
    return 0;
}

void netd_drv_link(int h, int up)
{
    struct drvif *d = drv_get(h);
    if (!d || up == !!netif_is_link_up(&d->f->netif)) return;
    if (up) netif_set_link_up(&d->f->netif); else netif_set_link_down(&d->f->netif);
    netd_logf("netd: %s: link %s", d->f->name, up ? "up" : "down");
}

void netd_drv_count(int h, unsigned long bad, unsigned long drop, unsigned long txerr)
{
    struct drvif *d = drv_get(h);
    if (!d || bad + drop + txerr > 100000) return;
    d->f->rx_bad += bad;
    d->f->rx_drop += drop;
    d->f->tx_drop += txerr;
}

int netd_drv_freed(int h, unsigned short *num, int max)
{
    struct drvif *d = drv_get(h);
    unsigned i;
    int n = 0;
    if (!d) return 0;
    for (i = 0; i < d->nbuf && n < max; i++)
        if (d->b[i].state == B_RETURN) { d->b[i].state = B_DRIVER; num[n++] = (unsigned short)i; }
    return n;
}

int netd_drv_tx(int h, struct netd_frame *fr, int max)
{
    struct drvif *d = drv_get(h);
    int n = 0;
    if (!d) return 0;
    while (n < max) {                   /* the oldest waiting frame first: frames leave in the order lwIP sent them */
        unsigned i, best = d->ntxq;
        for (i = 0; i < d->ntxq; i++)
            if (d->tx[i].state == Q_WAIT && (best == d->ntxq || (long)(d->tx[i].seq - d->tx[best].seq) < 0)) best = i;
        if (best == d->ntxq) break;
        struct txslot *t = &d->tx[best];
        struct netd_frame *o = &fr[n++];
        memset(o, 0, sizeof *o);
        o->tag = (unsigned short)best;
        if (d->dma) {
            o->npieces = 1;
            o->piece[0].addr = d->dmatx + (unsigned long)best * d->bufsize;
            o->piece[0].len = t->len;
        } else {
            struct pbuf *q;
            for (q = t->p; q && o->npieces < 4; q = q->next) {
                if (!q->len) continue;
                o->piece[o->npieces].addr = q->payload;
                o->piece[o->npieces].len = q->len;
                o->npieces++;
            }
        }
        t->state = Q_OUT;
    }
    return n;
}

int netd_drv_pending(int h)
{
    struct drvif *d = drv_get(h);
    unsigned i;
    if (!d) return 0;
    for (i = 0; i < d->ntxq; i++) if (d->tx[i].state == Q_WAIT) return 1;
    for (i = 0; i < d->nbuf; i++) if (d->b[i].state == B_RETURN) return 1;
    return 0;
}

void netd_drv_gone(int h)
{
    struct drvif *d = drv_get(h);
    struct nif *f;
    unsigned i;
    if (!d) return;
    f = d->f;
    d->dead = 1;
    for (i = 0; i < d->ntxq; i++) {     /* frames the driver never sent: given up (TCP sends them again) */
        if (d->tx[i].p) pbuf_free(d->tx[i].p);
        d->tx[i].p = NULL;
        d->tx[i].state = Q_FREE;
    }
    if (f->dhcp) { dhcp_stop(&f->netif); dhcp_cleanup(&f->netif); }
    netif_set_down(&f->netif);
    netif_poll(&f->netif);
    netif_remove(&f->netif);            /* ends the connections bound to its address (6.2) */
    f->used = 0;
    f->drv = NULL;
    d->f = NULL;
    netd_logf("netd: %s down", f->name);
    memset(f->dhcp_dns, 0, sizeof f->dhcp_dns);
    dns_apply();
    if (!d->held) drv_free_mem(d);      /* else when the stack lets go of its last buffer (dbuf_free) */
}

/* DHCP's name servers (lwIP's own handling is off, LWIP_DHCP_MAX_DNS_SERVERS 0: they never replace resolv.conf's):
 * option 6 of an ACK, kept by the interface (net/port/netd_hooks.h) */
/* Whether an ACK's name servers (the option's first 8 bytes, len of them) are taken, and which (network order, 0 for
 * none): only in the states where lwIP itself takes that ACK. The hook runs while lwIP parses the options, before it
 * looks at its state: a forged ACK with the xid of the last REQUEST (which every machine on the LAN saw) was ignored
 * by lwIP after the lease was bound, but its name servers were used (audit 2026-10-28 S9). An address no name server
 * can have (0, 127/8, multicast and above) is dropped. */
int netd_dhcp_dns_take(int state, int msg_type, const unsigned char *b, int len, unsigned long out[2])
{
    int i, n = 0;
    out[0] = out[1] = 0;
    if (msg_type != 5 || len < 4) return 0;                     /* DHCP_ACK */
    if (state != DHCP_STATE_REQUESTING && state != DHCP_STATE_REBOOTING && state != DHCP_STATE_RENEWING &&
        state != DHCP_STATE_REBINDING) return 0;
    for (i = 0; i < 2 && (i + 1) * 4 <= len; i++) {
        const unsigned char *a = b + i * 4;
        if (a[0] == 0 || a[0] == 127 || a[0] >= 224) continue;
        memcpy(&out[n], a, 4);                                  /* network order, as lwIP's u32_t addresses */
        n++;
    }
    return n;
}

void netd_dhcp_option(struct dhcp *dh, int state, int msg_type, int option, int len, struct pbuf *p, int offset)
{
    struct nif *f = NULL;
    unsigned char b[8];
    unsigned long d[2];
    int i;
    /* the interface is the one whose DHCP client this is (lwIP's current netif is not set before the address is) */
    for (i = 0; i < NETD_IFMAX; i++) if (nifs[i].used && netif_dhcp_data(&nifs[i].netif) == dh) f = &nifs[i];
    if (!f || option != 6 || len < 4) return;                   /* DHCP_OPTION_DNS_SERVER */
    memset(b, 0, sizeof b);
    pbuf_copy_partial(p, b, (u16_t)(len >= 8 ? 8 : 4), (u16_t)offset);
    if (!netd_dhcp_dns_take(state, msg_type, b, len >= 8 ? 8 : 4, d)) return;
    f->dhcp_dns[0] = (u32_t)d[0];
    f->dhcp_dns[1] = (u32_t)d[1];
    dns_apply();
}

/* a DHCP interface becomes the default route when it has its lease and no other interface is the default */
void netd_drv_turn(void)
{
    int i;
    for (i = 0; i < NETD_IFMAX; i++) {
        struct nif *f = &nifs[i];
        if (!f->used || !f->dhcp) continue;
        if (dhcp_supplied_address(&f->netif) != f->leased) {
            const ip4_addr_t *a = netif_ip4_addr(&f->netif);
            f->leased = (unsigned char)dhcp_supplied_address(&f->netif);
            if (f->leased) netd_logf("netd: %s: DHCP gave %u.%u.%u.%u", f->name, ip4_addr1_16(a), ip4_addr2_16(a), ip4_addr3_16(a), ip4_addr4_16(a));
            else netd_logf("netd: %s: the DHCP lease is gone", f->name);
        }
        if (f->leased && (!netif_default || !netif_is_up(netif_default)))
            netif_set_default(&f->netif);
    }
}
