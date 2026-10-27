/* SPDX-License-Identifier: GPL-2.0-only */
/* netd's core (DESIGN D19 sections 4.3, 4.5, 5.3, 6, 9): the sockets, the interfaces (SLIP, loopback), names, the
 * configuration files and the limits, over lwIP's raw API. It is plain C with no Zenix system call in it, so the
 * same files build for Zenix (netd's main program, D19 step 4, calls these functions from its message loop) and for
 * the PC (tools/host/netd-host.c, the host tests of 8.1).
 *
 * The rules of 4.3 this core keeps: no function here ever waits (rule 2): every call is done, or fails with
 * -EAGAIN; a socket's ready bits change only inside these calls (rule 1), and netd_bits() after a call is the truth
 * to send with its reply; netd_changed() lists the sockets whose bits changed otherwise (the push of each turn).
 * Sockets are named by the kernel's (index, generation); an index or generation netd does not know is -ENETDOWN
 * (rule 9).
 *
 * The message protocol that carries these calls (NET_* in kernel/netproto.h, struct usockreq, the epoch) is the
 * main line's (D19 steps 3 and 4) and is not here. The functions take what that protocol carries: addresses as the
 * program's struct sockaddr_in (Linux's layout: the family in the machine's byte order, port and address in network
 * order), buffers and lengths. */
#ifndef ZENIX_NETD_H
#define ZENIX_NETD_H

#include "../../kernel/errno.h"

/* the network's error numbers (Linux's values, D19 4.2; claimed for kernel/errno.h by the main line) */
#ifndef ENOTSOCK
#define ENOTSOCK         88
#define EDESTADDRREQ     89
#define EMSGSIZE         90
#define EPROTOTYPE       91
#define ENOPROTOOPT      92
#define EPROTONOSUPPORT  93
#define EAFNOSUPPORT     97
#define EADDRINUSE       98
#define EADDRNOTAVAIL    99
#define ENETDOWN        100
#define ENETUNREACH     101
#define ECONNABORTED    103
#define ENOBUFS         105
#define EISCONN         106
#define ENOTCONN        107
#define ETIMEDOUT       110
#define ECONNREFUSED    111
#define EHOSTUNREACH    113
#define EALREADY        114
#define EINPROGRESS     115
#endif

/* poll's bits (Linux's values, frozen in D16) */
#define NETD_POLLIN  0x0001
#define NETD_POLLOUT 0x0004
#define NETD_POLLERR 0x0008
#define NETD_POLLHUP 0x0010

/* BSD socket constants (Linux's values, D19 4.4) */
#define NETD_AF_INET      2
#define NETD_SOCK_STREAM  1
#define NETD_SOCK_DGRAM   2
#define NETD_SOCK_RAW     3
#define NETD_IPPROTO_ICMP 1
#define NETD_IPPROTO_TCP  6
#define NETD_IPPROTO_UDP  17
#define NETD_SOL_SOCKET   1
#define NETD_SO_REUSEADDR 2
#define NETD_SO_TYPE      3
#define NETD_SO_ERROR     4
#define NETD_SO_BROADCAST 6
#define NETD_SO_SNDBUF    7
#define NETD_SO_RCVBUF    8
#define NETD_SO_KEEPALIVE 9
#define NETD_TCP_NODELAY  1
#define NETD_MSG_PEEK     0x2
#define NETD_SHUT_RD      0
#define NETD_SHUT_WR      1
#define NETD_SHUT_RDWR    2
#define NETD_FIONREAD     0x541b

#define NETD_NSOCK        32     /* the kernel's NSOCK (D19 4.1): socket indexes 0..31 */
#define NETD_RESOLVE_MAX  4      /* addresses one name lookup answers */

/* ---- what netd's main program (or the host harness) gives the core ---- */
int netd_os_serial_open(const char *dev, unsigned long baud);       /* a handle >= 0, or -errno */
long netd_os_read(int h, void *buf, unsigned long n);                /* never waits: bytes, 0 (none), -errno */
long netd_os_write(int h, const void *buf, unsigned long n);         /* never waits: what fitted (0..n), -errno */
void netd_os_close(int h);
void netd_os_log(const char *line);                                  /* one line of netd's log, no line end */
void netd_os_fatal(void);                                            /* end netd at once (D11 starts it again) */
/* and netd_os_now_ms(), netd_os_clock4us() (net/port/netd_port.h) */

/* ---- start, the loop, the limits ---- */
/* The limits by the machine (D19 7.3 and 9.4): the two columns of 7.3 as profiles; netd's main program picks one
 * from the machine's memory (main line: which machine gets which). Every number can be changed before netd_init. */
struct netd_limits {
    unsigned short tcp_pcbs;        /* TCP connections (with TIME_WAIT ones, killed oldest first when full) */
    unsigned short tcp_listen;      /* listening TCP sockets */
    unsigned short tcp_segs;        /* queued TCP segments, all connections */
    unsigned short udp_pcbs;        /* UDP and ICMP sockets, DNS and DHCP included */
    unsigned short pbufs;           /* receive buffers (pool pbufs) */
    unsigned short ref_pbufs;       /* buffers that point at data elsewhere */
    unsigned short reass;           /* datagrams being reassembled */
    unsigned short reass_pbufs;     /* buffers held by reassembly */
    unsigned short arp_queue;       /* packets waiting for an ARP answer */
    unsigned short tcp_wnd;         /* receive window per connection, bytes */
    unsigned short tcp_snd_buf;     /* send buffer per connection, bytes */
    unsigned long oos_bytes;        /* out-of-order data per connection, bytes */
    unsigned short oos_pbufs;       /* and buffers */
    unsigned short backlog;         /* the most a listen() backlog may be */
    unsigned short udp_rxq;         /* datagrams queued per UDP/ICMP socket */
    unsigned long udp_rxq_bytes;    /* and their bytes */
    unsigned long heap;             /* all of lwIP's and netd's buffers together, bytes (0: no limit) */
};
#define NETD_PROFILE_SMALL 0        /* the stock A1200 on SLIP: 7.3's left column */
#define NETD_PROFILE_LARGE 1        /* a machine with fast RAM and Ethernet: the right column */
void netd_limits_profile(struct netd_limits *l, int profile);
int netd_init(const struct netd_limits *l, const unsigned char seed[48]);   /* 0, or -EINVAL for bad limits */
void netd_reseed(const unsigned char seed[48]);     /* fresh keys (/dev/random became readable, or every few minutes) */
/* One turn of the main loop: read the lines, run lwIP's timers and the loopback, send what waits. Returns the
 * milliseconds until the next timer (the timeout for the wait on the lines and the port). */
unsigned long netd_turn(void);
/* the handles netd waits on (SLIP's serial ports): up to max, each with whether it wants to write */
int netd_wait_handles(int *h, int *want_write, int max);

/* ---- sockets: each returns >= 0 or -errno, never waits ---- */
int netd_socket(unsigned idx, unsigned gen, int domain, int type, int protocol, unsigned uid);
int netd_bind(unsigned idx, unsigned gen, const void *addr, unsigned addrlen, unsigned uid);
int netd_connect(unsigned idx, unsigned gen, const void *addr, unsigned addrlen);
int netd_listen(unsigned idx, unsigned gen, int backlog);
/* the kernel has reserved (nidx, ngen) for the new socket (D19 4.3 rule 7) */
int netd_accept(unsigned idx, unsigned gen, unsigned nidx, unsigned ngen, void *addr, unsigned *addrlen);
long netd_send(unsigned idx, unsigned gen, const void *buf, unsigned long len, int flags, const void *addr,
               unsigned addrlen);
long netd_recv(unsigned idx, unsigned gen, void *buf, unsigned long len, int flags, void *addr, unsigned *addrlen);
int netd_shutdown(unsigned idx, unsigned gen, int how);
int netd_getsockopt(unsigned idx, unsigned gen, int level, int opt, void *val, unsigned *len);
int netd_setsockopt(unsigned idx, unsigned gen, int level, int opt, const void *val, unsigned len);
int netd_getname(unsigned idx, unsigned gen, int peer, void *addr, unsigned *addrlen);
int netd_ioctl(unsigned idx, unsigned gen, unsigned long req, long *val);
int netd_close(unsigned idx, unsigned gen);
/* Name lookups (4.5): a socket that becomes readable when the answer is in; netd_recv then gives up to
 * NETD_RESOLVE_MAX IPv4 addresses, 4 bytes each in network order, or -ENOENT (no such name: the name server said
 * so), -ETIMEDOUT (no answer from any name server), -ENETDOWN (no name server). Numbers ("10.0.2.2") are answered at
 * once. Order: /etc/hosts, the cache, DNS. A name without a dot gets the search domain (netd_set_search) */
int netd_resolve(unsigned idx, unsigned gen, const char *name, unsigned uid);
int netd_set_search(const char *domain);       /* resolv.conf's search (or domain) word; "" none */

unsigned netd_bits(unsigned idx, unsigned gen);     /* the socket's ready bits now (also what the next push compares) */
struct netd_ready { unsigned short idx, gen, bits; };
int netd_changed(struct netd_ready *out, int max);  /* sockets whose bits changed since netd_bits/netd_changed */

/* ---- configuration (6.1) ---- */
#define NETD_IFNAME 8
#define NETD_IFMAX 4
struct netd_ifcfg {
    char name[NETD_IFNAME];
    unsigned char is_auto, method;      /* method: NETD_INET_DHCP or NETD_INET_STATIC */
    char driver[16];                    /* "slip", "genet": no '/' (it names /sbin/net/NAME) */
    char args[64];                      /* the rest of the driver line ("/dev/ttyS0 115200") */
    unsigned long addr, mask, gateway, ptp;     /* network order; 0 when not given */
    unsigned short mtu;
    char hostname[32];
};
#define NETD_INET_DHCP 1
#define NETD_INET_STATIC 2
struct netd_config {
    struct netd_ifcfg ifs[NETD_IFMAX];
    int nifs;
    unsigned long dns[2];               /* resolv.conf's name servers, network order */
    int ndns;
    char search[64];
};
/* Each parser reports a word it does not know with a line "netd: FILE:LINE: ..." and ignores it (6.1); it returns
 * the number of such reports. Text comes as read from the file (any length; lines of at most 255 bytes). */
int netd_parse_interfaces(const char *text, unsigned long len, struct netd_config *c);
int netd_parse_resolv(const char *text, unsigned long len, struct netd_config *c);
int netd_hosts_load(const char *text, unsigned long len);   /* replaces the /etc/hosts table */
/* netd starts programs as root from /etc/network/interfaces: it uses the file only if root owns it and neither
 * group nor others may write it (6.1); mode and uid as stat gives them */
int netd_config_trusted(unsigned long mode, unsigned uid);

/* ---- interfaces (6.3, 6.4) ---- */
int netd_if_up(const struct netd_ifcfg *ifc);       /* SLIP here; a driver's interface through the driver protocol later */
int netd_if_down(const char *name);
int netd_route_default(unsigned long gw);           /* network order; 0 removes it */
/* the default route now: its gateway (0: none) and the interface it leaves by (D19 step 5, route) */
int netd_route_get(unsigned long *gw, char name[NETD_IFNAME]);
/* a new address (and netmask, 0: keep it) on an interface that is up (D19 6.1: ifconfig IFACE ADDRESS/PREFIX) */
int netd_if_addr(const char *name, unsigned long addr, unsigned long mask);
int netd_set_dns(const unsigned long *servers, int n);   /* resolv.conf's (they win over DHCP's) */
void netd_dns_servers(unsigned long out[2]);        /* the name servers in use now (network order; 0: none) */
struct netd_ifinfo {
    char name[NETD_IFNAME];
    unsigned long addr, mask, gateway;  /* network order */
    unsigned short mtu, up, link;
    unsigned long rx_frames, tx_frames, rx_bytes, tx_bytes, rx_bad, tx_drop;
    unsigned char ether, dhcp;          /* a driver's Ethernet interface; its address from DHCP */
    unsigned char hwaddr[6];
    unsigned long lease;                /* seconds left of the DHCP lease (0: none yet) */
    unsigned long dhcp_server;          /* network order */
    unsigned long rx_drop;              /* frames received and dropped for want of memory */
};
int netd_if_info(int i, struct netd_ifinfo *out);   /* 0 for interface i, -ENOENT past the last */
/* ---- drivers' interfaces (D19 2.3, 2.4; step 7): what the driver protocol (kernel/netproto.h NETDRV_*, adapted by
 * netd's main program) carries, as plain arguments. A handle names a driver's interface from its attach on ---- */
#define NETD_DRV_MAXBUF 32              /* = DRV_MAXBUF: receive buffers a driver may have */
#define NETD_DRV_BATCH  16              /* = DRV_BATCH: entries of each kind in one transfer */
struct netd_drvattach {
    unsigned char mac[6];
    unsigned mtu, nrx;                  /* 576..1500; 1..NETD_DRV_MAXBUF */
    int dma, link;                      /* the buffers in the driver's DMA memory (dmamem, dmalen: room for nrx receive
                                         * buffers and NETD_DRV_BATCH send buffers); the link is up */
    void *dmamem;
    unsigned long dmalen;
};
/* the driver of the stanza c attached: its interface comes up (static, or DHCP started). The buffers it is lent:
 * *nbuf of *bufsize bytes at bufs[]. -> the handle, or -EEXIST, -EINVAL, -ENOSPC, -ENOMEM */
int netd_drv_attach(const struct netd_ifcfg *c, const struct netd_drvattach *a, unsigned *bufsize, unsigned *nbuf,
                    void **bufs);
/* one transfer, in: a buffer filled with a frame (it is netd's then), a sent frame's tag, the link, the hardware's
 * counts. A wrong number or length is refused (-EINVAL), counted and logged: it fails that frame, never netd */
int netd_drv_rx(int h, unsigned num, unsigned len);
int netd_drv_done(int h, unsigned tag);
void netd_drv_link(int h, int up);
void netd_drv_count(int h, unsigned long bad, unsigned long drop, unsigned long txerr);
/* out: buffers given back (empty, the driver's again), frames to send (the oldest first) */
int netd_drv_freed(int h, unsigned short *num, int max);
struct netd_frame { unsigned short tag, npieces; struct { void *addr; unsigned long len; } piece[4]; };
int netd_drv_tx(int h, struct netd_frame *f, int max);
int netd_drv_pending(int h);            /* frames to send or buffers to give back: ring the driver's doorbell */
void netd_drv_gone(int h);              /* the driver ended or was stopped: its interface goes; its buffers are never
                                         * lent again (netd's own memory is freed when the stack lets go of the last) */
void netd_drv_turn(void);               /* once a turn: a DHCP interface with its lease becomes the default route */
/* the OS layer's memory for the buffers (not counted in lwIP's heap: the driver's fixed cost) */
void *netd_os_alloc(unsigned long n);
void netd_os_free(void *p);

/* netstat (6.4): one row per socket and per lwIP connection; the frozen unetstat layout is the main line's */
struct netd_connrow {
    unsigned char proto;                /* NETD_IPPROTO_TCP, _UDP, _ICMP */
    unsigned char state;                /* lwIP's TCP state (CLOSED 0 .. TIME_WAIT 10); 0 for UDP and ICMP */
    unsigned long laddr, raddr;         /* network order */
    unsigned short lport, rport;        /* host order */
    unsigned long rxq, txq;             /* bytes waiting */
    unsigned uid;
};
int netd_netstat(struct netd_connrow *rows, int max);

/* counters for the tests and ifconfig */
struct netd_stats {
    unsigned long udp_dropped;          /* datagrams dropped: the socket's queue was full */
    unsigned long accept_dropped;       /* connections refused: the listener's queue was full */
    unsigned long mem_refused;          /* allocations refused by the limits */
};
void netd_get_stats(struct netd_stats *s);

#endif
