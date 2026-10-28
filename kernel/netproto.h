/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The network's interface (DESIGN D19, docs/d19-network.md section 4): the program's side (SYS_sock, struct
 * usockreq) and the protocol between the kernel and the network server on port "net" (NET_* messages, struct netreq,
 * SYS_netsrv). Frozen and append-only like fsproto.h: numbers are never reused or changed, structs grow at the end.
 * The NET_* (kernel to the server, 1-63) and NETDRV_* (drivers' blocks the kernel forwards, from 64) ranges do not
 * overlap: both arrive on one port. Needs only u32 (usys.h and kernel.h both have it), so programs include it too. */

/* SYS_sock (op | request size << 16, fd, struct usockreq *): every BSD socket call. The ops (frozen, append-only); the
 * same numbers are the NET_* message types the kernel sends for them */
#define SOCK_SOCKET       1     /* level: domain (AF_INET), flags: type (SOCK_STREAM, ...) | SOCK_NONBLOCK, option: protocol -> fd */
#define SOCK_BIND         2     /* addr, alen (-> the address's length) */
#define SOCK_CONNECT      3     /* addr, alen */
#define SOCK_LISTEN       4     /* len: the backlog */
#define SOCK_ACCEPT       5     /* addr, alen (both may be 0), flags: SOCK_NONBLOCK for the new fd -> fd */
#define SOCK_SENDTO       6     /* buf, len, flags (MSG_*), addr, alen (addr 0: the connected peer) -> bytes */
#define SOCK_RECVFROM     7     /* buf, len, flags, addr, alen (both may be 0) -> bytes, 0 at the end */
#define SOCK_SHUTDOWN     8     /* flags: SHUT_RD 0, SHUT_WR 1, SHUT_RDWR 2 */
#define SOCK_GETSOCKOPT   9     /* level, option, val, vlen (-> its length, in and out) */
#define SOCK_SETSOCKOPT  10     /* level, option, val, vlen (-> its length) */
#define SOCK_GETSOCKNAME 11     /* addr, alen (in and out) */
#define SOCK_GETPEERNAME 12     /* addr, alen (in and out) */
#define SOCK_IOCTL       13     /* option: FIONREAD (val -> an int written) or FIONBIO (val -> an int read; the kernel's) */
#define SOCK_RESOLVE     14     /* buf, len: a name; the socket becomes a lookup: readable when the answer is in, recv
                                 * then gives up to 4 IPv4 addresses (4 bytes each) or the error (DESIGN D19 4.5) */
#define SOCK_IFLIST      15     /* buf, len: struct unetif rows; flags: the row size (0 = UNETIF_FIRST) -> rows */
#define SOCK_IFSET       16     /* buf: struct unetifreq (root) */
#define SOCK_ROUTE       17     /* buf: struct uroute, read and written back (changes: root) */
#define SOCK_NETSTAT     18     /* buf, len: struct unetstat rows; flags: the row size (0 = UNETSTAT_FIRST) -> rows */
#define SOCK_OPS         18
#define NET_CLOSE        19     /* message only: the last close of a socket (a no-start call, rule 11) */

/* SYS_sock's request (frozen, first size USOCKREQ_FIRST): which fields an op uses is listed above. Every length that
 * goes in or out is behind a pointer (alen, vlen), as BSD's socklen_t * */
struct usockreq {
    u32 buf, len, flags;
    u32 addr, alen;             /* a struct sockaddr and a pointer to its length */
    unsigned short level, option;   /* get/setsockopt; SOCKET: domain, protocol; IOCTL: option is the request */
    u32 val, vlen;              /* get/setsockopt's value and a pointer to its length; IOCTL's int */
};
#define USOCKREQ_FIRST 32

/* NET_* messages (kernel to the server, UMSG_KERNEL): a[0] the socket's name (ksock index | generation << 8; SOCKET:
 * the new one), a[1] a struct netreq * (in the caller's kernel stack: read it in place). The server reads and writes
 * the program's buffers in place, within what netreq allows. It answers every request at once (done or -EAGAIN,
 * rule 2) with SYS_netsrv NETSRV_REPLY, after writing into the message: a[2] the socket's new ready bits (POLL*;
 * ACCEPT: the new socket's in the upper 16), a[3] its epoch (NETSRV_HELLO). A name it does not know: -ENETDOWN. */
struct netreq {
    struct usockreq r;          /* the program's request as it gave it (pointers into its memory) */
    u32 newsock;                /* SOCKET, ACCEPT: the name the kernel reserved for the new socket */
    u32 len;                    /* the buffer's bytes the server may use: r.len clamped (NET_BUF_MAX) and checked */
    u32 alen;                   /* the address's bytes it may read or write: min(*r.alen, NET_ADDR_MAX), 0 for none */
    u32 vlen;                   /* the value's bytes: min(*r.vlen, NET_VAL_MAX), 0 for none */
    u32 pid;                    /* the caller */
    unsigned short uid, gid;    /* its effective user and group: ports below 1024, raw sockets need uid 0 */
};
#define NET_BUF_MAX  65536u     /* a bigger buffer is clamped (a blocking stream send loops) */
#define NET_ADDR_MAX 28u        /* the largest address (struct sockaddr_in6); the true length is still returned */
#define NET_VAL_MAX  64u

/* SYS_netsrv (op, a, b): only for the process the kernel started for port "net" */
#define NETSRV_HELLO 1          /* -> the epoch now (positive) */
#define NETSRV_READY 2          /* a: struct unetready[], b: count (at most NSOCK) -> how many named a live socket */
#define NETSRV_REPLY 3          /* a: the message, b: the result; a[2] holds the socket's bits (one trap, rule 1) */
struct unetready { u32 sock, bits; };

/* the values ported programs know (Linux's) */
#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOCK_RAW     3
#define SOCK_NONBLOCK 0x800     /* = O_NONBLOCK */
#define SOCK_CLOEXEC 0x80000    /* accepted, ignored (there is no exec) */
#define SOL_SOCKET   1
#define SO_ERROR     4
#define SO_RCVTIMEO 20          /* struct timeval { seconds, microseconds }: the kernel's own (its blocking wait) */
#define SO_SNDTIMEO 21
#define MSG_PEEK     0x02
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define FIONREAD     0x541B
#define FIONBIO      0x5421     /* val -> an int read (set or clear O_NONBLOCK); val 0: the answer is the flag now (1, 0) */

/* more of Linux's values (D19 step 4): libz's BSD calls and netd use them */
#define AF_UNSPEC    0
#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP 17
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_LINGER   13          /* struct linger (user/socket.h); {1, 0}: close by abort (D23 step 3) */
#define TCP_NODELAY  1          /* level IPPROTO_TCP */
#define SHUT_RD      0
#define SHUT_WR      1
#define SHUT_RDWR    2

/* SOCK_NETSTAT's rows (frozen, first size UNETSTAT_FIRST; the request's flags give the row size the caller knows,
 * 0 = UNETSTAT_FIRST, len the buffer's bytes): one row per TCP connection or listener (TIME_WAIT ones too), UDP socket
 * and ICMP echo socket; the answer is the number of rows written */
struct unetstat {
    unsigned char proto, state; /* IPPROTO_TCP, _UDP, _ICMP; TCP's state: 0 CLOSED, 1 LISTEN, 2 SYN_SENT, 3 SYN_RCVD,
                                 * 4 ESTABLISHED, 5 FIN_WAIT_1, 6 FIN_WAIT_2, 7 CLOSE_WAIT, 8 CLOSING, 9 LAST_ACK,
                                 * 10 TIME_WAIT (BSD's numbers) */
    unsigned short lport;       /* the local port (ICMP: the echo id), host order */
    u32 laddr, raddr;           /* network order */
    unsigned short rport, pad;
    u32 rxq, txq;               /* bytes received and not yet read; sent and not yet acknowledged */
    u32 uid;                    /* the socket's owner (0 for a connection no socket holds any more) */
    u32 reserved[3];
};
#define UNETSTAT_FIRST 40

/* SOCK_IFLIST's rows (frozen, first size UNETIF_FIRST): one per interface netd has */
struct unetif {
    char name[8];
    u32 addr, mask, gw;         /* network order */
    unsigned short mtu, flags;  /* NETIF_* */
    u32 rx_frames, tx_frames, rx_bytes, tx_bytes, rx_bad, tx_drop;
    u32 dns[2];                 /* the name servers netd uses (the same in every row; 0: none) */
    unsigned char hwaddr[6];    /* the MAC address (Ethernet; 0 for SLIP and the loopback) (D19 step 7, was reserved) */
    unsigned short pad;
    /* past the first size (D19 step 7; the caller's row size in flags): */
    u32 lease;                  /* seconds left of a DHCP lease (0: none) */
    u32 dhcp_server;            /* the DHCP server that gave it, network order */
    u32 rx_drop;                /* frames received and dropped for want of memory */
    u32 reserved[1];
};
#define UNETIF_FIRST 64
#define NETIF_UP       1
#define NETIF_LINK     2        /* the line is there (SLIP: the port is open) */
#define NETIF_LOOPBACK 4
#define NETIF_DEFAULT  8        /* the default route leaves through it */
#define NETIF_ETHER   16        /* an Ethernet interface (a driver's, D19 step 7): it has hwaddr */
#define NETIF_DHCP    32        /* its address comes from DHCP (inet dhcp) */

/* SOCK_IFSET's request (frozen, first size UNETIFREQ_FIRST: len): root only */
struct unetifreq {
    char name[8];               /* "": every interface with an "auto" line (UP), every one but the loopback (DOWN) */
    u32 op;                     /* NETIFREQ_* */
    u32 addr, mask;             /* NETIFREQ_ADDR, network order */
    u32 reserved[3];
};
#define UNETIFREQ_FIRST 32
#define NETIFREQ_UP   1         /* as its stanza in /etc/network/interfaces says (the files read again) */
#define NETIFREQ_DOWN 2
#define NETIFREQ_ADDR 3         /* a new address (and netmask, 0: keep it) on an interface that is up */

/* SOCK_ROUTE's request (frozen, first size UROUTE_FIRST: len), written back */
struct uroute {
    u32 op;                     /* ROUTE_* */
    u32 gw;                     /* network order; ROUTE_GET: the default gateway (0: none) */
    char name[8];               /* ROUTE_GET: the interface it leaves by */
    u32 reserved[4];
};
#define UROUTE_FIRST 32
#define ROUTE_GET 0
#define ROUTE_ADD 1             /* root: the default route through gw */
#define ROUTE_DEL 2             /* root: no default route */

/* ---- the drivers (DESIGN D19 2.3, 2.4, 5; step 7): SYS_drv (op, a, b), only for a child of the running network
 * server (-EPERM for anyone else; -ECONNRESET when no network server runs: an orphaned driver learns so), and the blocks the kernel forwards to "net" as NETDRV_* messages (a[0] the driver's pid, a[1] the block in
 * the driver's memory, a[2] its size; a call that never starts a server: an orphaned driver gets -ECONNRESET).
 * netd answers them with NETSRV_REPLY as it does the NET_* requests ---- */
#define DRV_HWINFO   1          /* a: buffer, b: its size (0 = HWINFO_FIRST) -> bytes copied of the hardware table
                                 * (kernel/hwinfo.h); -ENODEV without one */
#define DRV_CLAIM    2          /* a: a device's register base the hardware table names (genet, or a board's address;
                                 * never a RAM board) -> the claim (1..): the caller is its only owner; -EBUSY when
                                 * someone holds it */
#define DRV_DMAMEM   3          /* a: claim, b: bytes -> the address of the claim's DMA region (64-byte aligned, whole
                                 * 64-byte lines, cleared, fast RAM when there is any); one region a claim. It
                                 * belongs to the claim, not to a process: freed only when the claim ends */
#define DRV_QUIESCE  4          /* a: claim, b: struct udrvquiesce * -> 0: the register writes that stop the device,
                                 * run by the kernel when the driver ends and when the network server ends, before
                                 * anything of either is freed (then the DMA region is freed and the claim ends) */
#define DRV_CACHE    5          /* op | DRVC_* << 8, a: address, b: length, inside the caller's DMA region: the data
                                 * cache written back (and dropped) over whole 64-byte lines. Nothing to do on a 68k
                                 * (Zenix keeps the data cache off); on Emu68 the ARM's cache */
#define DRV_ATTACH   6          /* a: struct udrvattach *, b: its size -> the interface number; netd fills the block */
#define DRV_XFER     7          /* a: struct udrvxfer *, b: its size -> 0; netd reads and writes the block */
#define DRVC_CLEAN   1          /* frames the device will read: written back */
#define DRVC_INVAL   2          /* buffers the device wrote: written back and dropped (never dropped unwritten) */

#define NETDRV_ATTACH 64        /* the kernel to netd: a driver's DRV_ATTACH */
#define NETDRV_XFER   65        /* a driver's DRV_XFER */

/* DRV_QUIESCE's program (frozen): at most DRV_QSTEPS steps, run in order, no jumps; every address inside the
 * claim's register window, aligned to its size (1, 2 or 4 bytes) */
#define DRV_QSTEPS 8
struct udrvstep {
    unsigned char op, size;     /* DRVS_*; 1, 2, 4 */
    unsigned short pad;
    u32 addr, mask, value;
};
#define DRVS_WRITE 1            /* *addr = value */
#define DRVS_RMW   2            /* *addr = (*addr & ~mask) | value */
struct udrvquiesce { u32 n; struct udrvstep step[DRV_QSTEPS]; };

/* DRV_ATTACH's block (frozen, first size UDRVATTACH_FIRST; the caller's size in size and b). The driver says who it
 * is; netd lends it its receive buffers (all empty, the driver's to fill) and answers with the interface number */
#define DRV_MAXBUF 32
struct udrvattach {
    u32 size;                   /* this block's size (sizeof) */
    char name[8];               /* the interface ("eth0": netd gives the name as the driver's first argument) */
    unsigned char mac[6];       /* its hardware address */
    unsigned short mtu;         /* the largest packet without the Ethernet header (1500) */
    u32 flags;                  /* DRVA_* */
    u32 nrx;                    /* receive buffers it wants (1..DRV_MAXBUF) */
    u32 claim;                  /* DRVA_DMA: the claim whose DMA region (dma, dmalen) holds the buffers */
    u32 dma, dmalen;
    /* written by netd: */
    u32 ifindex;                /* the interface number (also the call's answer) */
    u32 bufsize;                /* every buffer's bytes */
    u32 nbuf;                   /* buffers lent: buf[0..nbuf-1] */
    u32 buf[DRV_MAXBUF];        /* their addresses (a buffer's number is its index here) */
};
#define UDRVATTACH_FIRST 180
#define DRVA_DMA  1             /* the device reads and writes host memory: netd makes the buffers in the DMA region
                                 * (nrx receive buffers, then DRV_BATCH send buffers) and copies every frame */
#define DRVA_LINK 2             /* the link is up now */

/* DRV_XFER's block (frozen, first size UDRVXFER_FIRST): one call a batch, never one a frame. In: the frames received
 * (the buffer's number and length: the buffer is netd's from here), the frames sent (their tags), the link, the
 * hardware's counts; out (netd writes): buffers given back (empty, the driver's again), frames to send (up to 4
 * pieces each, in netd's memory or, with DRVA_DMA, one piece in a send buffer of the DMA region; a frame's memory is
 * netd's until its tag comes back), and whether netd has more */
#define DRV_BATCH 16
struct udrvframe {
    unsigned short tag, npieces;    /* the tag to give back in done[] when it is sent; 1..4 pieces */
    u32 flags;                  /* none yet (a checksum request later) */
    struct { u32 addr, len; } piece[4];
};
struct udrvxfer {
    u32 size;                   /* this block's size (sizeof) */
    u32 link;                   /* in: 1 the link is up, 0 down */
    u32 nrx, ndone;             /* in: entries in rx[] and done[] */
    u32 rx_bad, rx_drop, tx_err;    /* in: the hardware's counts since the last DRV_XFER (added up by netd) */
    u32 nfree, ntx;             /* out: entries in freed[] and tx[] */
    u32 more;                   /* out: 1 if netd has more to send or give back: call again without waiting */
    struct { unsigned short num, len; u32 flags; } rx[DRV_BATCH];
    unsigned short done[DRV_BATCH];
    unsigned short freed[DRV_BATCH];
    struct udrvframe tx[DRV_BATCH];
};
#define UDRVXFER_FIRST 872
