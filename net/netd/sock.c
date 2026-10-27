/* SPDX-License-Identifier: GPL-2.0-only */
/* netd's sockets over lwIP's raw API (DESIGN D19 4, 6.4, 9.1, 9.4, 9.5): TCP, UDP, Linux-style ICMP echo
 * sockets (ping), and name lookups. No call waits (4.3 rule 2); the ready bits come from the socket's state only
 * (compute_bits), so a call that answers -EAGAIN always leaves bits that say "not ready" (rule 1). */
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/dns.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "netd_int.h"
#include "netd_rand.h"

enum { K_TCP = 1, K_UDP, K_ICMP, K_RESOLVE };
enum { T_NEW, T_CONNECTING, T_CONNECTED, T_LISTENING };     /* TCP's states as the program sees them */

struct dgram { struct dgram *next; u32_t addr; u16_t port, len; unsigned char data[1]; };

struct nsock {
    u16_t idx, gen;
    unsigned char kind, tstate;
    unsigned char eof, shut_rd, shut_wr, wblocked, pending, nodelay, broadcast;
    unsigned uid;
    int err;                            /* the error SO_ERROR (and the next call) reports, positive */
    struct tcp_pcb *tcp;
    struct udp_pcb *udp;
    struct pbuf *rx;                    /* TCP: received, not yet read */
    struct dgram *dq, **dq_tail;        /* UDP, ICMP: datagrams */
    unsigned dq_n;
    unsigned long dq_bytes;
    struct nsock **acq;                 /* a listener: connections lwIP has accepted, the program not yet */
    unsigned acq_n, acq_max;
    struct nsock *listener;             /* a connection in a listener's queue */
    u16_t icmp_id;
    u32_t peer_addr;                    /* UDP, ICMP: the address connect() gave */
    u16_t peer_port;
    unsigned char resolved, nres;       /* K_RESOLVE */
    u32_t res[NETD_RESOLVE_MAX];
    unsigned bits;                      /* what was last reported (a reply or a push) */
};

static struct slot { u16_t gen; struct nsock *s; } slots[NETD_NSOCK];
static struct raw_pcb *icmp_pcb;        /* one for every ICMP socket: replies go to the socket with their echo id */
#define CLOSING_MAX 8
static struct tcp_pcb *closing[CLOSING_MAX];    /* tcp_close said ERR_MEM: tried again every turn (9.5) */

static struct nsock *get(unsigned idx, unsigned gen)
{
    if (idx >= NETD_NSOCK || !slots[idx].s || slots[idx].gen != gen) return NULL;
    return slots[idx].s;
}

/* ---- addresses: struct sockaddr_in, Linux's layout ---- */
static int get_sin(const void *addr, unsigned len, ip_addr_t *ip, u16_t *port, int *unspec)
{
    const unsigned char *a = addr;
    unsigned short fam;
    u32_t v;
    if (unspec) *unspec = 0;
    if (!a || len < 2) return -EINVAL;
    memcpy(&fam, a, 2);
    if (fam == 0 && unspec) { *unspec = 1; return 0; }    /* AF_UNSPEC: connect() undoes a datagram connect */
    if (fam != NETD_AF_INET) return -EAFNOSUPPORT;
    if (len < 16) return -EINVAL;
    *port = (u16_t)(a[2] << 8 | a[3]);
    memcpy(&v, a + 4, 4);
    ip_addr_set_ip4_u32_val(*ip, v);
    return 0;
}
static void put_sin(void *addr, unsigned *len, u32_t ip, u16_t port)
{
    unsigned char a[16];
    unsigned short fam = NETD_AF_INET;
    if (!addr || !len) return;
    memset(a, 0, sizeof a);
    memcpy(a, &fam, 2);
    a[2] = (unsigned char)(port >> 8);
    a[3] = (unsigned char)port;
    memcpy(a + 4, &ip, 4);
    memcpy(addr, a, *len < 16 ? *len : 16);
    *len = 16;                          /* the true length (the kernel clamped what it lets netd write) */
}

static int lwip_errno(err_t e)
{
    switch (e) {
    case ERR_OK: return 0;
    case ERR_MEM: case ERR_BUF: return -ENOBUFS;
    case ERR_TIMEOUT: return -ETIMEDOUT;
    case ERR_RTE: return -ENETUNREACH;
    case ERR_INPROGRESS: return -EINPROGRESS;
    case ERR_VAL: case ERR_ARG: return -EINVAL;
    case ERR_WOULDBLOCK: return -EAGAIN;
    case ERR_USE: return -EADDRINUSE;
    case ERR_ALREADY: return -EALREADY;
    case ERR_ISCONN: return -EISCONN;
    case ERR_CONN: return -ENOTCONN;
    case ERR_IF: return -ENETDOWN;
    case ERR_ABRT: return -ECONNABORTED;
    case ERR_RST: return -ECONNRESET;
    case ERR_CLSD: return -ENOTCONN;
    default: return -EIO;
    }
}

/* ---- the ready bits (4.3 rules 1 and 8) ---- */
static unsigned tcp_out_ready(struct nsock *s)
{
    unsigned need = netd_tcp_snd_buf / 3, mss;
    if (!s->tcp) return 0;
    mss = tcp_mss(s->tcp);
    if (mss && mss < need) need = mss;      /* one MSS, or a third of the send buffer, free (rule 8) */
    if (s->wblocked || tcp_sndbuf(s->tcp) < need || tcp_sndqueuelen(s->tcp) >= TCP_SND_QUEUELEN - 1) return 0;
    return NETD_POLLOUT;
}
static unsigned compute_bits(struct nsock *s)
{
    unsigned b = 0;
    switch (s->kind) {
    case K_TCP:
        switch (s->tstate) {
        case T_LISTENING: return s->acq_n ? NETD_POLLIN : 0;
        case T_NEW: return NETD_POLLOUT | NETD_POLLHUP;              /* as Linux: nothing to wait for */
        case T_CONNECTING: return s->err ? NETD_POLLERR | NETD_POLLOUT | NETD_POLLHUP : 0;
        }
        if (s->rx || s->eof || s->shut_rd || s->err || !s->tcp) b |= NETD_POLLIN;
        if (!s->tcp || s->shut_wr) b |= NETD_POLLOUT;               /* a write would fail at once (EPIPE) */
        else b |= tcp_out_ready(s);
        if (s->err) b |= NETD_POLLERR;
        if (!s->tcp || (s->eof && s->shut_wr)) b |= NETD_POLLHUP;
        return b;
    case K_UDP: case K_ICMP:
        return NETD_POLLOUT | (s->dq ? NETD_POLLIN : 0);
    case K_RESOLVE:
        return s->resolved ? NETD_POLLIN : 0;
    }
    return 0;
}

unsigned netd_bits(unsigned idx, unsigned gen)
{
    struct nsock *s = get(idx, gen);
    if (!s) return NETD_POLLERR | NETD_POLLHUP;
    return s->bits = compute_bits(s);
}

int netd_changed(struct netd_ready *out, int max)
{
    int i, n = 0;
    for (i = 0; i < NETD_NSOCK && n < max; i++) {
        struct nsock *s = slots[i].s;
        unsigned b;
        if (!s) continue;
        b = compute_bits(s);
        if (b == s->bits) continue;
        s->bits = b;
        out[n].idx = (u16_t)i; out[n].gen = slots[i].gen; out[n].bits = (u16_t)b;
        n++;
    }
    return n;
}

/* ---- memory of a socket ---- */
static struct nsock *new_sock(int kind, unsigned uid)
{
    struct nsock *s = netd_malloc(sizeof *s);
    if (!s) { netd_mem_refused(); return NULL; }
    memset(s, 0, sizeof *s);
    s->kind = (unsigned char)kind;
    s->uid = uid;
    s->dq_tail = &s->dq;
    return s;
}
static void free_dgrams(struct nsock *s)
{
    while (s->dq) { struct dgram *d = s->dq; s->dq = d->next; netd_free(d); }
    s->dq_tail = &s->dq;
    s->dq_n = 0;
    s->dq_bytes = 0;
}

/* ---- TCP callbacks ---- */
static void tcp_set_callbacks(struct nsock *s);

static err_t cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct nsock *s = arg;
    (void)err;
    if (!s) { if (p) { tcp_recved(pcb, p->tot_len); pbuf_free(p); } return ERR_OK; }
    if (!p) { s->eof = 1; return ERR_OK; }              /* the peer's FIN */
    if (s->shut_rd) { tcp_recved(pcb, p->tot_len); pbuf_free(p); return ERR_OK; }
    if (s->rx) pbuf_cat(s->rx, p); else s->rx = p;      /* bounded by the window: tcp_recved comes after the read */
    return ERR_OK;
}
static err_t cb_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    struct nsock *s = arg;
    (void)pcb; (void)len;
    if (s) s->wblocked = 0;
    return ERR_OK;
}
static err_t cb_poll(void *arg, struct tcp_pcb *pcb)
{
    struct nsock *s = arg;
    (void)pcb;
    if (s) s->wblocked = 0;     /* memory that refused a write may be free again */
    return ERR_OK;
}
static void listener_unqueue(struct nsock *s);
static void cb_err(void *arg, err_t err)
{
    struct nsock *s = arg;          /* lwIP has freed the pcb already (9.5) */
    if (!s) return;
    s->tcp = NULL;
    if (s->pending) {                   /* died before the program accepted it */
        listener_unqueue(s);
        if (s->rx) pbuf_free(s->rx);
        netd_free(s);
        return;
    }
    if (s->tstate == T_CONNECTING) s->err = err == ERR_RST ? ECONNREFUSED : ETIMEDOUT;
    else s->err = err == ERR_RST ? ECONNRESET : err == ERR_CLSD ? 0 : ECONNABORTED;
    if (s->tstate == T_CONNECTING) s->tstate = T_CONNECTED;     /* failed: reads and writes report it */
}
static err_t cb_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct nsock *s = arg;
    (void)pcb; (void)err;
    if (s) s->tstate = T_CONNECTED;
    return ERR_OK;
}
static err_t cb_accept(void *arg, struct tcp_pcb *npcb, err_t err)
{
    struct nsock *l = arg, *s;
    if (err != ERR_OK || !npcb) return ERR_VAL;
    if (!l || l->acq_n >= l->acq_max || !(s = new_sock(K_TCP, l->uid))) {
        netd_st.accept_dropped++;
        tcp_abort(npcb);
        return ERR_ABRT;
    }
    s->tcp = npcb;
    s->tstate = T_CONNECTED;
    s->pending = 1;
    s->listener = l;
    s->nodelay = l->nodelay;
    if (s->nodelay) tcp_nagle_disable(npcb);
    tcp_set_callbacks(s);
    tcp_backlog_delayed(npcb);          /* counts in the backlog until the program accepts it */
    l->acq[l->acq_n++] = s;
    return ERR_OK;
}
static void tcp_set_callbacks(struct nsock *s)
{
    tcp_arg(s->tcp, s);
    tcp_recv(s->tcp, cb_recv);
    tcp_sent(s->tcp, cb_sent);
    tcp_err(s->tcp, cb_err);
    tcp_poll(s->tcp, cb_poll, 2);
}
static void tcp_detach(struct tcp_pcb *pcb)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);                /* lwIP's own: frees whatever still comes */
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
}
static void listener_unqueue(struct nsock *s)
{
    struct nsock *l = s->listener;
    unsigned i;
    if (!l) return;
    for (i = 0; i < l->acq_n; i++)
        if (l->acq[i] == s) { memmove(&l->acq[i], &l->acq[i + 1], (l->acq_n - i - 1) * sizeof *l->acq); l->acq_n--; break; }
    s->listener = NULL;
    s->pending = 0;
}
/* a pcb waiting to be closed that lwIP ends itself (a reset, a time out, killed for memory) leaves the list */
static void cb_closing_err(void *arg, err_t err)
{
    struct tcp_pcb **slot = arg;
    (void)err;
    if (slot) *slot = NULL;
}
static void tcp_close_or_queue(struct tcp_pcb *pcb)
{
    int i;
    if (tcp_close(pcb) == ERR_OK) return;
    for (i = 0; i < CLOSING_MAX; i++)
        if (!closing[i]) {
            closing[i] = pcb;
            tcp_arg(pcb, &closing[i]);
            tcp_err(pcb, cb_closing_err);
            return;
        }
    tcp_abort(pcb);                     /* no room to wait: a reset instead of a FIN */
}
void netd_sock_turn(void)
{
    int i;
    for (i = 0; i < CLOSING_MAX; i++) {
        struct tcp_pcb *p = closing[i];
        if (!p) continue;
        tcp_err(p, NULL);
        if (tcp_close(p) == ERR_OK) { closing[i] = NULL; continue; }
        tcp_err(p, cb_closing_err);
    }
}

/* ---- ICMP echo sockets (6.4): netd sets the echo id; replies go only to the socket with that id ---- */
static struct nsock *icmp_by_id(u16_t id)
{
    int i;
    for (i = 0; i < NETD_NSOCK; i++)
        if (slots[i].s && slots[i].s->kind == K_ICMP && slots[i].s->icmp_id == id) return slots[i].s;
    return NULL;
}
static int queue_dgram(struct nsock *s, struct pbuf *p, u16_t off, u32_t addr, u16_t port)
{
    struct dgram *d;
    u16_t n = (u16_t)(p->tot_len - off);
    if (s->dq_n >= netd_lim.udp_rxq || s->dq_bytes + n > netd_lim.udp_rxq_bytes) { netd_st.udp_dropped++; return 0; }
    d = netd_malloc(sizeof *d + n);
    if (!d) { netd_st.udp_dropped++; netd_mem_refused(); return 0; }
    d->next = NULL; d->addr = addr; d->port = port; d->len = n;
    pbuf_copy_partial(p, d->data, n, off);
    *s->dq_tail = d;
    s->dq_tail = &d->next;
    s->dq_n++;
    s->dq_bytes += n;
    return 1;
}
static u8_t cb_icmp(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    u8_t hdr[8];
    u16_t ihl;
    struct nsock *s;
    (void)arg; (void)pcb;
    if (p->tot_len < IP_HLEN + 8 || pbuf_copy_partial(p, hdr, 1, 0) != 1) return 0;
    ihl = (u16_t)((hdr[0] & 15) * 4);
    if (p->tot_len < ihl + 8 || pbuf_copy_partial(p, hdr, 8, ihl) != 8) return 0;
    if (hdr[0] != ICMP_ER || hdr[1] != 0) return 0;                 /* only echo replies; lwIP does the rest */
    s = icmp_by_id((u16_t)(hdr[4] << 8 | hdr[5]));
    if (!s) return 0;
    queue_dgram(s, p, ihl, ip4_addr_get_u32(ip_2_ip4(addr)), 0);
    pbuf_free(p);
    return 1;
}

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    struct nsock *s = arg;
    (void)pcb;
    if (s && !s->shut_rd) queue_dgram(s, p, 0, ip4_addr_get_u32(ip_2_ip4(addr)), port);
    pbuf_free(p);
}

void netd_sock_init(void)
{
    memset(slots, 0, sizeof slots);
    memset(closing, 0, sizeof closing);
    icmp_pcb = raw_new(IP_PROTO_ICMP);
    if (icmp_pcb) raw_recv(icmp_pcb, cb_icmp, NULL);
    else netd_logf("netd: no memory for ICMP sockets");
}

/* ---- the calls ---- */
int netd_socket(unsigned idx, unsigned gen, int domain, int type, int protocol, unsigned uid)
{
    struct nsock *s;
    int kind;
    if (idx >= NETD_NSOCK) return -EINVAL;
    if (slots[idx].s) return -EBUSY;
    if (domain != NETD_AF_INET) return -EAFNOSUPPORT;
    type &= 0xf;                        /* SOCK_NONBLOCK and SOCK_CLOEXEC are the kernel's */
    if (type == NETD_SOCK_STREAM && (protocol == 0 || protocol == NETD_IPPROTO_TCP)) kind = K_TCP;
    else if (type == NETD_SOCK_DGRAM && (protocol == 0 || protocol == NETD_IPPROTO_UDP)) kind = K_UDP;
    else if (type == NETD_SOCK_DGRAM && protocol == NETD_IPPROTO_ICMP) kind = K_ICMP;
    else if (type == NETD_SOCK_STREAM || type == NETD_SOCK_DGRAM) return -EPROTONOSUPPORT;
    else return type == NETD_SOCK_RAW ? -EPROTONOSUPPORT : -EINVAL;   /* raw IP sockets: not yet (9.1) */
    if (kind == K_ICMP && !icmp_pcb) return -ENOBUFS;
    s = new_sock(kind, uid);
    if (!s) return -ENOBUFS;
    if (kind == K_TCP) {
        s->tcp = tcp_new();
        if (!s->tcp) { netd_free(s); netd_mem_refused(); return -ENOBUFS; }
        tcp_set_callbacks(s);
    } else if (kind == K_UDP) {
        s->udp = udp_new();
        if (!s->udp) { netd_free(s); netd_mem_refused(); return -ENOBUFS; }
        udp_recv(s->udp, udp_recv_cb, s);
    } else {
        int tries;
        for (tries = 0; tries < 64; tries++) {
            u16_t id = (u16_t)netd_rand();
            if (id && !icmp_by_id(id)) { s->icmp_id = id; break; }
        }
        if (!s->icmp_id) { netd_free(s); return -EADDRINUSE; }
    }
    s->idx = (u16_t)idx; s->gen = (u16_t)gen;
    slots[idx].s = s;
    slots[idx].gen = (u16_t)gen;
    s->bits = compute_bits(s);
    return 0;
}

/* SO_REUSEADDR never shares a port between different users (9.1) */
static int port_taken_by_other(struct nsock *me, u16_t port)
{
    int i;
    for (i = 0; i < NETD_NSOCK; i++) {
        struct nsock *s = slots[i].s;
        if (!s || s == me || s->kind != me->kind || s->uid == me->uid) continue;
        if (s->kind == K_TCP && s->tcp && s->tcp->local_port == port) return 1;
        if (s->kind == K_UDP && s->udp && s->udp->local_port == port) return 1;
    }
    return 0;
}

int netd_bind(unsigned idx, unsigned gen, const void *addr, unsigned len, unsigned uid)
{
    struct nsock *s = get(idx, gen);
    ip_addr_t ip;
    u16_t port;
    int r;
    if (!s) return -ENETDOWN;
    if ((r = get_sin(addr, len, &ip, &port, NULL))) return r;
    if (port && port < 1024 && uid != 0) return -EACCES;           /* ports below 1024: root (9.1) */
    if (!netd_addr_is_local(ip_2_ip4(&ip))) return -EADDRNOTAVAIL;
    if (port && port_taken_by_other(s, port)) return -EADDRINUSE;
    switch (s->kind) {
    case K_TCP:
        if (!s->tcp || s->tstate != T_NEW) return -EINVAL;
        if (s->tcp->local_port) return -EINVAL;                     /* bound already */
        return lwip_errno(tcp_bind(s->tcp, &ip, port));
    case K_UDP:
        if (s->udp->local_port) return -EINVAL;
        return lwip_errno(udp_bind(s->udp, &ip, port));
    case K_ICMP:
        return 0;                       /* the echo id is netd's; the address is not used */
    }
    return -EOPNOTSUPP;
}

int netd_connect(unsigned idx, unsigned gen, const void *addr, unsigned len)
{
    struct nsock *s = get(idx, gen);
    ip_addr_t ip;
    u16_t port;
    int r, unspec;
    if (!s) return -ENETDOWN;
    if ((r = get_sin(addr, len, &ip, &port, s->kind == K_TCP ? NULL : &unspec))) return r;
    switch (s->kind) {
    case K_TCP:
        if (s->tstate == T_CONNECTING) return -EALREADY;
        if (s->tstate == T_CONNECTED) return -EISCONN;
        if (s->tstate == T_LISTENING) return -EINVAL;
        if (!s->tcp) return -EINVAL;
        if (ip_addr_isany(&ip) || !port) return -ECONNREFUSED;
        r = lwip_errno(tcp_connect(s->tcp, &ip, port, cb_connected));
        if (r) return r;
        s->tstate = T_CONNECTING;
        return -EINPROGRESS;            /* always: the kernel waits for POLLOUT|POLLERR, then asks SO_ERROR (rule 6) */
    case K_UDP:
        if (unspec) { udp_disconnect(s->udp); return 0; }
        return lwip_errno(udp_connect(s->udp, &ip, port));
    case K_ICMP:
        if (unspec) { s->peer_addr = 0; return 0; }
        s->peer_addr = ip4_addr_get_u32(ip_2_ip4(&ip));
        return 0;
    }
    return -EOPNOTSUPP;
}

int netd_listen(unsigned idx, unsigned gen, int backlog)
{
    struct nsock *s = get(idx, gen);
    struct tcp_pcb *l;
    err_t e;
    if (!s) return -ENETDOWN;
    if (s->kind != K_TCP) return -EOPNOTSUPP;
    if (backlog < 1) backlog = 1;
    if (backlog > netd_lim.backlog) backlog = netd_lim.backlog;     /* capped per listener (9.4) */
    if (s->tstate == T_LISTENING) {
        if ((unsigned)backlog < s->acq_n) backlog = (int)s->acq_n;
        tcp_backlog_set(s->tcp, (u8_t)backlog);
        s->acq_max = (unsigned)backlog;
        return 0;
    }
    if (s->tstate != T_NEW || !s->tcp) return -EINVAL;
    if (!s->tcp->local_port) {
        e = tcp_bind(s->tcp, IP_ADDR_ANY, 0);
        if (e != ERR_OK) return lwip_errno(e);
    }
    s->acq = netd_malloc((unsigned long)backlog * sizeof *s->acq);
    if (!s->acq) { netd_mem_refused(); return -ENOBUFS; }
    l = tcp_listen_with_backlog_and_err(s->tcp, (u8_t)backlog, &e);
    if (!l) { netd_free(s->acq); s->acq = NULL; return lwip_errno(e); }
    s->tcp = l;                         /* tcp_listen frees the old pcb (9.5) */
    s->acq_max = (unsigned)backlog;
    s->tstate = T_LISTENING;
    tcp_arg(l, s);
    tcp_accept(l, cb_accept);
    return 0;
}

int netd_accept(unsigned idx, unsigned gen, unsigned nidx, unsigned ngen, void *addr, unsigned *addrlen)
{
    struct nsock *l = get(idx, gen), *s;
    if (!l) return -ENETDOWN;
    if (l->kind != K_TCP || l->tstate != T_LISTENING) return -EINVAL;
    if (nidx >= NETD_NSOCK || slots[nidx].s) return -EINVAL;
    if (!l->acq_n) return -EAGAIN;
    s = l->acq[0];
    listener_unqueue(s);
    if (s->tcp) tcp_backlog_accepted(s->tcp);
    s->idx = (u16_t)nidx; s->gen = (u16_t)ngen;
    slots[nidx].s = s;
    slots[nidx].gen = (u16_t)ngen;
    if (s->tcp) put_sin(addr, addrlen, ip4_addr_get_u32(ip_2_ip4(&s->tcp->remote_ip)), s->tcp->remote_port);
    else put_sin(addr, addrlen, 0, 0);
    s->bits = compute_bits(s);
    return 0;
}

static long tcp_send(struct nsock *s, const void *buf, unsigned long len)
{
    unsigned long n;
    err_t e;
    if (s->tstate == T_NEW || s->tstate == T_LISTENING) return -ENOTCONN;
    if (s->tstate == T_CONNECTING) return -EAGAIN;
    if (s->err) { int r = -s->err; s->err = 0; return r; }
    if (!s->tcp || s->shut_wr) return -EPIPE;           /* the kernel raises SIGPIPE unless MSG_NOSIGNAL (rule 4) */
    if (!len) return 0;
    n = tcp_sndbuf(s->tcp);
    if (n > len) n = len;
    if (n > 0xffff) n = 0xffff;
    if (!n || tcp_sndqueuelen(s->tcp) >= TCP_SND_QUEUELEN - 1) return -EAGAIN;
    e = tcp_write(s->tcp, buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (e == ERR_MEM) {
        /* no memory for the segments: try a smaller piece once, else wait for an ACK (cb_sent clears it) */
        n = n > 2 * tcp_mss(s->tcp) ? tcp_mss(s->tcp) : 0;
        if (!n || tcp_write(s->tcp, buf, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            s->wblocked = 1;
            netd_mem_refused();
            return -EAGAIN;
        }
    } else if (e != ERR_OK) return lwip_errno(e);
    tcp_output(s->tcp);                 /* ERR_MEM here is fine: the data is queued, a timer sends it */
    return (long)n;
}

static long dgram_send(struct nsock *s, const void *buf, unsigned long len, const void *addr, unsigned addrlen)
{
    ip_addr_t ip;
    u16_t port = 0;
    struct pbuf *p;
    unsigned mtu, hdr = s->kind == K_UDP ? 8 : 0;
    err_t e;
    int r;
    if (addr) {
        if ((r = get_sin(addr, addrlen, &ip, &port, NULL))) return r;
    } else if (s->kind == K_UDP && (s->udp->flags & UDP_FLAGS_CONNECTED)) {
        ip_addr_copy(ip, s->udp->remote_ip);
        port = s->udp->remote_port;
    } else if (s->kind == K_ICMP && s->peer_addr) {
        ip_addr_set_ip4_u32_val(ip, s->peer_addr);
    } else return -EDESTADDRREQ;
    if (s->shut_wr) return -EPIPE;
    if (s->kind == K_UDP && !port) return -EINVAL;
    mtu = netd_route_mtu(ip_2_ip4(&ip));
    if (!mtu) return -ENETUNREACH;
    if (!s->broadcast && (ip4_addr_get_u32(ip_2_ip4(&ip)) == IPADDR_BROADCAST ||
                          (netif_default && ip4_addr_isbroadcast(ip_2_ip4(&ip), netif_default)))) return -EACCES;
    if (len + hdr + IP_HLEN > mtu) return -EMSGSIZE;    /* netd sends no fragments (IP_FRAG 0, 2.2) */
    if (s->kind == K_ICMP) {
        const unsigned char *b = buf;
        if (len < 8 || b[0] != ICMP_ECHO || b[1] != 0) return -EINVAL;     /* echo requests only (6.4) */
    }
    p = pbuf_alloc(s->kind == K_UDP ? PBUF_TRANSPORT : PBUF_IP, (u16_t)len, PBUF_RAM);
    if (!p) { netd_mem_refused(); return -ENOBUFS; }
    pbuf_take(p, buf, (u16_t)len);
    if (s->kind == K_UDP) {
        e = addr ? udp_sendto(s->udp, p, &ip, port) : udp_send(s->udp, p);
    } else {
        struct icmp_echo_hdr *h = p->payload;
        h->id = lwip_htons(s->icmp_id);
        h->chksum = 0;
        h->chksum = inet_chksum(h, (u16_t)len);
        e = raw_sendto(icmp_pcb, p, &ip);
    }
    pbuf_free(p);
    if (e != ERR_OK) return lwip_errno(e);
    return (long)len;
}

long netd_send(unsigned idx, unsigned gen, const void *buf, unsigned long len, int flags, const void *addr,
               unsigned addrlen)
{
    struct nsock *s = get(idx, gen);
    (void)flags;                        /* MSG_NOSIGNAL and MSG_DONTWAIT are the kernel's */
    if (!s) return -ENETDOWN;
    if (len > 65535) len = 65535;       /* the kernel clamps at 64 KB and loops (4.2, rule 4) */
    switch (s->kind) {
    case K_TCP: return addr && s->tstate != T_CONNECTED ? -EOPNOTSUPP : tcp_send(s, buf, len);
    case K_UDP: case K_ICMP: return dgram_send(s, buf, len, addr, addrlen);
    }
    return -EOPNOTSUPP;
}

static long tcp_recv_data(struct nsock *s, void *buf, unsigned long len, int flags)
{
    unsigned long n;
    if (s->tstate == T_NEW || s->tstate == T_LISTENING) return -ENOTCONN;
    if (s->tstate == T_CONNECTING) return -EAGAIN;
    if (s->shut_rd) return 0;
    if (s->rx && len) {
        n = s->rx->tot_len;
        if (n > len) n = len;
        n = pbuf_copy_partial(s->rx, buf, (u16_t)n, 0);
        if (!(flags & NETD_MSG_PEEK)) {
            s->rx = pbuf_free_header(s->rx, (u16_t)n);
            if (s->tcp) tcp_recved(s->tcp, (u16_t)n);   /* the window opens only now (9.5) */
        }
        return (long)n;
    }
    if (!len && s->rx) return 0;
    if (s->err) { int r = -s->err; if (!(flags & NETD_MSG_PEEK)) s->err = 0; return r; }
    if (s->eof || !s->tcp) return 0;
    return -EAGAIN;
}

long netd_recv(unsigned idx, unsigned gen, void *buf, unsigned long len, int flags, void *addr, unsigned *addrlen)
{
    struct nsock *s = get(idx, gen);
    if (!s) return -ENETDOWN;
    if (len > 65535) len = 65535;
    switch (s->kind) {
    case K_TCP:
        if (addr && addrlen) *addrlen = 0;
        return tcp_recv_data(s, buf, len, flags);
    case K_UDP: case K_ICMP: {
        struct dgram *d = s->dq;
        unsigned long n;
        if (!d) return s->shut_rd ? 0 : -EAGAIN;
        n = d->len < len ? d->len : len;               /* the rest of a longer datagram is lost, as on Linux */
        memcpy(buf, d->data, n);
        put_sin(addr, addrlen, d->addr, d->port);
        if (!(flags & NETD_MSG_PEEK)) {
            s->dq = d->next;
            if (!s->dq) s->dq_tail = &s->dq;
            s->dq_n--;
            s->dq_bytes -= d->len;
            netd_free(d);
        }
        return (long)n;
    }
    case K_RESOLVE: {
        unsigned long n = (unsigned long)s->nres * 4;
        if (!s->resolved) return -EAGAIN;
        if (s->err) return -s->err;
        if (n > len) n = len & ~3ul;
        memcpy(buf, s->res, n);
        return (long)n;
    }
    }
    return -EOPNOTSUPP;
}

int netd_shutdown(unsigned idx, unsigned gen, int how)
{
    struct nsock *s = get(idx, gen);
    int rd = how == NETD_SHUT_RD || how == NETD_SHUT_RDWR, wr = how == NETD_SHUT_WR || how == NETD_SHUT_RDWR;
    if (!s) return -ENETDOWN;
    if (!rd && !wr) return -EINVAL;
    if (s->kind == K_TCP) {
        if (s->tstate != T_CONNECTED) return -ENOTCONN;
        if (rd && !s->shut_rd) {
            s->shut_rd = 1;
            if (s->rx) {                /* what was not read is dropped, the window opens */
                if (s->tcp) tcp_recved(s->tcp, s->rx->tot_len);
                pbuf_free(s->rx);
                s->rx = NULL;
            }
        }
        if (wr && !s->shut_wr) {
            s->shut_wr = 1;
            if (s->tcp) tcp_shutdown(s->tcp, 0, 1);    /* the FIN */
        }
        return 0;
    }
    if (s->kind == K_UDP || s->kind == K_ICMP) {
        if (rd) { s->shut_rd = 1; free_dgrams(s); }
        if (wr) s->shut_wr = 1;
        return 0;
    }
    return -ENOTCONN;
}

static int put_int(void *val, unsigned *len, int v)
{
    if (!val || !len || *len < 4) return -EINVAL;
    memcpy(val, &v, 4);
    *len = 4;
    return 0;
}
int netd_getsockopt(unsigned idx, unsigned gen, int level, int opt, void *val, unsigned *len)
{
    struct nsock *s = get(idx, gen);
    if (!s) return -ENETDOWN;
    if (level == NETD_SOL_SOCKET) {
        switch (opt) {
        case NETD_SO_ERROR: { int e = s->err; s->err = 0; return put_int(val, len, e); }
        case NETD_SO_TYPE: return put_int(val, len, s->kind == K_TCP ? NETD_SOCK_STREAM : NETD_SOCK_DGRAM);
        case NETD_SO_REUSEADDR:
            return put_int(val, len, s->tcp ? ip_get_option(s->tcp, SOF_REUSEADDR) != 0 :
                                     s->udp ? ip_get_option(s->udp, SOF_REUSEADDR) != 0 : 0);
        case NETD_SO_KEEPALIVE: return put_int(val, len, s->tcp ? ip_get_option(s->tcp, SOF_KEEPALIVE) != 0 : 0);
        case NETD_SO_BROADCAST: return put_int(val, len, s->broadcast);
        case NETD_SO_RCVBUF: return put_int(val, len, s->kind == K_TCP ? netd_tcp_wnd : (int)netd_lim.udp_rxq_bytes);
        case NETD_SO_SNDBUF: return put_int(val, len, s->kind == K_TCP ? netd_tcp_snd_buf : 65535);
        }
        return -ENOPROTOOPT;
    }
    if (level == NETD_IPPROTO_TCP && s->kind == K_TCP && opt == NETD_TCP_NODELAY) return put_int(val, len, s->nodelay);
    return -ENOPROTOOPT;
}
int netd_setsockopt(unsigned idx, unsigned gen, int level, int opt, const void *val, unsigned len)
{
    struct nsock *s = get(idx, gen);
    int v;
    if (!s) return -ENETDOWN;
    if (!val || len < 4) return -EINVAL;
    memcpy(&v, val, 4);
    if (level == NETD_SOL_SOCKET) {
        switch (opt) {
        case NETD_SO_REUSEADDR:
            if (s->tcp) { if (v) ip_set_option(s->tcp, SOF_REUSEADDR); else ip_reset_option(s->tcp, SOF_REUSEADDR); }
            if (s->udp) { if (v) ip_set_option(s->udp, SOF_REUSEADDR); else ip_reset_option(s->udp, SOF_REUSEADDR); }
            return 0;
        case NETD_SO_KEEPALIVE:
            if (!s->tcp) return s->kind == K_TCP ? -EINVAL : -ENOPROTOOPT;
            if (v) ip_set_option(s->tcp, SOF_KEEPALIVE); else ip_reset_option(s->tcp, SOF_KEEPALIVE);
            return 0;
        case NETD_SO_BROADCAST: s->broadcast = v != 0; return 0;
        case NETD_SO_RCVBUF: case NETD_SO_SNDBUF: return 0;     /* the machine's numbers stay (7.3) */
        }
        return -ENOPROTOOPT;
    }
    if (level == NETD_IPPROTO_TCP && s->kind == K_TCP && opt == NETD_TCP_NODELAY) {
        s->nodelay = v != 0;
        if (s->tcp) { if (v) tcp_nagle_disable(s->tcp); else tcp_nagle_enable(s->tcp); }
        return 0;
    }
    return -ENOPROTOOPT;
}

int netd_getname(unsigned idx, unsigned gen, int peer, void *addr, unsigned *len)
{
    struct nsock *s = get(idx, gen);
    if (!s) return -ENETDOWN;
    switch (s->kind) {
    case K_TCP:
        if (!s->tcp) { if (peer) return -ENOTCONN; put_sin(addr, len, 0, 0); return 0; }
        if (peer) {
            if (s->tstate != T_CONNECTED) return -ENOTCONN;
            put_sin(addr, len, ip4_addr_get_u32(ip_2_ip4(&s->tcp->remote_ip)), s->tcp->remote_port);
        } else put_sin(addr, len, ip4_addr_get_u32(ip_2_ip4(&s->tcp->local_ip)), s->tcp->local_port);
        return 0;
    case K_UDP:
        if (peer) {
            if (!(s->udp->flags & UDP_FLAGS_CONNECTED)) return -ENOTCONN;
            put_sin(addr, len, ip4_addr_get_u32(ip_2_ip4(&s->udp->remote_ip)), s->udp->remote_port);
        } else put_sin(addr, len, ip4_addr_get_u32(ip_2_ip4(&s->udp->local_ip)), s->udp->local_port);
        return 0;
    case K_ICMP:
        if (peer) { if (!s->peer_addr) return -ENOTCONN; put_sin(addr, len, s->peer_addr, 0); }
        else put_sin(addr, len, 0, s->icmp_id);     /* as Linux: the echo id in the port's place */
        return 0;
    }
    return -EOPNOTSUPP;
}

int netd_ioctl(unsigned idx, unsigned gen, unsigned long req, long *val)
{
    struct nsock *s = get(idx, gen);
    if (!s) return -ENETDOWN;
    if (req != NETD_FIONREAD) return -ENOTTY;      /* FIONBIO is the kernel's file flag (4.1) */
    if (s->kind == K_TCP) *val = s->rx ? s->rx->tot_len : 0;
    else if (s->kind == K_UDP || s->kind == K_ICMP) *val = s->dq ? s->dq->len : 0;
    else *val = s->resolved ? (long)s->nres * 4 : 0;
    return 0;
}

int netd_close(unsigned idx, unsigned gen)
{
    struct nsock *s = get(idx, gen);
    unsigned i;
    if (!s) return -ENETDOWN;
    slots[idx].s = NULL;
    switch (s->kind) {
    case K_TCP:
        if (s->tstate == T_LISTENING) {
            for (i = 0; i < s->acq_n; i++) {           /* connections nobody accepted: a reset each */
                struct nsock *c = s->acq[i];
                if (c->tcp) { tcp_detach(c->tcp); tcp_abort(c->tcp); }
                if (c->rx) pbuf_free(c->rx);
                netd_free(c);
            }
            netd_free(s->acq);
            if (s->tcp) { tcp_arg(s->tcp, NULL); tcp_accept(s->tcp, NULL); tcp_close_or_queue(s->tcp); }
        } else if (s->tcp) {
            tcp_detach(s->tcp);
            /* data not read: lwIP answers the close with a reset (its window is not whole), as Linux does */
            if (s->rx) { pbuf_free(s->rx); s->rx = NULL; }
            tcp_close_or_queue(s->tcp);
        }
        if (s->rx) pbuf_free(s->rx);
        break;
    case K_UDP:
        udp_remove(s->udp);
        free_dgrams(s);
        break;
    case K_ICMP:
        free_dgrams(s);
        break;
    case K_RESOLVE:
        break;                          /* an answer still coming finds no socket (its generation) */
    }
    netd_free(s);
    return 0;
}

/* ---- names (4.5) ---- */
static void dns_done(const char *name, const ip_addr_t *ip, void *arg)
{
    mem_ptr_t v = (mem_ptr_t)arg;
    struct nsock *s = get((unsigned)(v >> 16), (unsigned)(v & 0xffff));
    (void)name;
    if (!s || s->kind != K_RESOLVE || s->resolved) return;
    s->resolved = 1;
    if (ip) { s->res[0] = ip4_addr_get_u32(ip_2_ip4(ip)); s->nres = 1; }
    else s->err = ENOENT;               /* lwIP tells a missing name from no answer apart in neither case */
}

int netd_resolve(unsigned idx, unsigned gen, const char *name, unsigned uid)
{
    struct nsock *s;
    ip_addr_t ip;
    u32_t a;
    int n;
    err_t e;
    if (idx >= NETD_NSOCK) return -EINVAL;
    if (slots[idx].s) return -EBUSY;
    if (!name || !*name || strlen(name) >= DNS_MAX_NAME_LENGTH) return -EINVAL;
    if (!(s = new_sock(K_RESOLVE, uid))) return -ENOBUFS;
    s->idx = (u16_t)idx; s->gen = (u16_t)gen;
    slots[idx].s = s;
    slots[idx].gen = (u16_t)gen;
    if (netd_parse_ip4(name, &a)) { s->res[0] = a; s->nres = 1; s->resolved = 1; }
    else if ((n = netd_hosts_find(name, s->res, NETD_RESOLVE_MAX)) > 0) { s->nres = (unsigned char)n; s->resolved = 1; }
    else {
        e = dns_gethostbyname(name, &ip, dns_done, (void *)(mem_ptr_t)(idx << 16 | (gen & 0xffff)));
        if (e == ERR_OK) { s->res[0] = ip4_addr_get_u32(ip_2_ip4(&ip)); s->nres = 1; s->resolved = 1; }
        else if (e != ERR_INPROGRESS) {
            s->resolved = 1;
            s->err = e == ERR_MEM ? ENOBUFS : ENETDOWN;
        }
    }
    s->bits = compute_bits(s);
    return 0;
}

/* ---- netstat (6.4) ---- */
int netd_netstat(struct netd_connrow *rows, int max)
{
    struct tcp_pcb *const *lists[] = { &tcp_active_pcbs, (struct tcp_pcb *const *)&tcp_listen_pcbs.pcbs, &tcp_tw_pcbs };
    unsigned li;
    int n = 0, i;
    struct udp_pcb *u;
    for (li = 0; li < 3; li++) {
        struct tcp_pcb *p;
        for (p = *lists[li]; p && n < max; p = p->next) {
            /* the argument is a socket only while netd's own callbacks are set (a pcb waiting to close has another) */
            struct nsock *s = p->state == LISTEN ? (((struct tcp_pcb_listen *)p)->accept == cb_accept ? p->callback_arg : NULL)
                                                 : (p->errf == cb_err ? p->callback_arg : NULL);
            struct netd_connrow *r = &rows[n++];
            memset(r, 0, sizeof *r);
            r->proto = NETD_IPPROTO_TCP;
            r->state = (unsigned char)p->state;
            r->laddr = ip4_addr_get_u32(ip_2_ip4(&p->local_ip));
            r->lport = p->local_port;
            if (p->state != LISTEN) {
                r->raddr = ip4_addr_get_u32(ip_2_ip4(&p->remote_ip));
                r->rport = p->remote_port;
                r->txq = (unsigned long)netd_tcp_snd_buf - tcp_sndbuf(p);
                if (s && s->rx) r->rxq = s->rx->tot_len;
            }
            r->uid = s ? s->uid : 0;
        }
    }
    for (u = udp_pcbs; u && n < max; u = u->next) {
        struct nsock *s = u->recv_arg;
        struct netd_connrow *r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->proto = NETD_IPPROTO_UDP;
        r->laddr = ip4_addr_get_u32(ip_2_ip4(&u->local_ip));
        r->lport = u->local_port;
        r->raddr = ip4_addr_get_u32(ip_2_ip4(&u->remote_ip));
        r->rport = u->remote_port;
        if (s && u->recv == udp_recv_cb) { r->rxq = s->dq_bytes; r->uid = s->uid; }
    }
    for (i = 0; i < NETD_NSOCK && n < max; i++) {
        struct nsock *s = slots[i].s;
        struct netd_connrow *r;
        if (!s || s->kind != K_ICMP) continue;
        r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->proto = NETD_IPPROTO_ICMP;
        r->lport = s->icmp_id;
        r->raddr = s->peer_addr;
        r->rxq = s->dq_bytes;
        r->uid = s->uid;
    }
    return n;
}
