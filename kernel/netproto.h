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
#define SOCK_RESOLVE     14     /* (DESIGN D19 4.5, step 6) */
#define SOCK_IFLIST      15     /* (section 6, step 5) */
#define SOCK_IFSET       16
#define SOCK_ROUTE       17
#define SOCK_NETSTAT     18     /* (step 4) */
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
#define FIONBIO      0x5421
