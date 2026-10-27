/* SPDX-License-Identifier: GPL-2.0-only */
/* netd's interfaces (DESIGN D19 2.1, 5.3, 6): SLIP over a serial port and the loopback live inside netd; an
 * Ethernet card's driver is a process of its own (the driver protocol, D19 step 7, not here).
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
#include "netif/slipif.h"
#include "netd_int.h"

#define SLIP_TXQ 4096           /* the send queue: two frames of 1006 bytes at their worst (every byte escaped) */
#define SLIP_RXBUF 512
#define TURN_READ 2048          /* bytes read from one port in one turn: the turn stays short (5.3) */

struct nif {
    struct netif netif;
    int used, handle;
    char name[NETD_IFNAME];
    netif_output_fn slip_output;        /* slipif's own output, called after the room check */
    unsigned char rx[SLIP_RXBUF];
    unsigned rxpos, rxlen;
    unsigned char *txq;
    unsigned long txn;
    int overflow;
    unsigned long rx_frames, tx_frames, rx_bytes, tx_bytes, tx_drop, rx_bad;
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
        if (!f->used) continue;
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
        if (nifs[i].used) { h[n] = nifs[i].handle; want_write[n] = nifs[i].txn != 0; n++; }
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

int netd_set_dns(const unsigned long *servers, int n)
{
    int i;
    for (i = 0; i < DNS_MAX_SERVERS; i++) {
        ip_addr_t a;
        ip_addr_set_ip4_u32_val(a, i < n ? servers[i] : 0);
        dns_setserver((u8_t)i, i < n ? &a : NULL);
    }
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
