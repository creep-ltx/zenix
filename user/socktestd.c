/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/netproto.h"
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef int s32;
/* socktestd: the kernel socket layer's test server (DESIGN D19 step 3), in netd's place on port "net": registered
 * with `svctest reg net 0 /bin/socktestd`, the kernel starts it at the first socket call. It answers from a small
 * table, as netd will: every request at once, done or -EAGAIN (rule 2), with NETSRV_REPLY and the socket's new ready
 * bits; changes to other sockets (a peer's data, a connection to accept) are pushed with NETSRV_READY once a turn.
 * Its "network" is a loopback of stream sockets on port numbers: connect to a port someone listens on makes a pair;
 * each socket holds up to CAP bytes the peer sent (so a big send waits for the reader: partial sends). Connects to a
 * port of 8000 or more complete one second later (-EINPROGRESS first: the connect rule). Commands, as setsockopt
 * at level ZZ: 1 crash at once (an illegal instruction: its death, without a reply), 2 the next recv answers a count
 * far too big (the kernel's clamp), 3 end cleanly, 4 (getsockopt) the requests served so far, 5 hang once the next close
 * is answered (a busy loop that takes no more requests, as S22's loop inside lwIP), 6 hang inside the request (never answers it):
 * the kernel's watchdog must end it (audit O9). A name it does not know is -ENETDOWN (rule 9). */
#define NS  24
#define CAP 512
#define ZZ  0x7A7A
static struct sk {
    u32 name;                           /* 0: not attached yet (a connection waiting in a backlog) */
    u8 used, listening, closed, peer_gone, connecting;
    s8 peer, lis;                       /* the other end; the listener a waiting connection belongs to */
    u16 port;
    u32 done_at, count;                 /* connecting: completes at this tick */
    u8 buf[CAP];
} sk[NS];
static u32 epoch, served, lie, hang_after_close;
static struct unetready push[NS];
static int npush;

static int by_name(u32 name)
{
    for (int i = 0; i < NS; i++)
        if (sk[i].used && sk[i].name == name && name)
            return i;
    return -1;
}

static int fresh(void)
{
    for (int i = 0; i < NS; i++)
        if (!sk[i].used) {
            memset(&sk[i], 0, sizeof sk[i]);
            sk[i].used = 1;
            sk[i].peer = sk[i].lis = -1;
            return i;
        }
    return -1;
}

static int backlog(int li)
{
    for (int i = 0; i < NS; i++)
        if (sk[i].used && !sk[i].name && sk[i].lis == li)
            return i;
    return -1;
}

static u32 bits(int i)                  /* the truth about socket i */
{
    struct sk *s = &sk[i];
    if (s->listening)
        return backlog(i) >= 0 ? POLLIN : 0;
    if (s->connecting)
        return 0;
    u32 b = 0;
    if (s->count || s->peer_gone)
        b |= POLLIN;
    if (s->peer_gone)
        b |= POLLHUP;
    if (s->peer >= 0 && sk[s->peer].count < CAP)
        b |= POLLOUT;
    if (s->peer < 0 && !s->peer_gone)
        b |= POLLOUT;                   /* not connected: a send says why at once */
    return b;
}

static void mark(int i)                 /* push i's bits at the end of this turn */
{
    if (i < 0 || !sk[i].name)
        return;
    for (int k = 0; k < npush; k++)
        if (push[k].sock == sk[i].name)
            return;
    push[npush].sock = sk[i].name;
    npush++;
}

static void put_addr(const struct netreq *q, int port)
{
    if (!q->r.alen)
        return;
    u8 a[16];
    memset(a, 0, sizeof a);
    a[1] = AF_INET;
    a[2] = (u8)(port >> 8);
    a[3] = (u8)port;
    a[4] = 127;
    a[7] = 1;
    memcpy((void *)q->r.addr, a, q->alen < 16 ? q->alen : 16);   /* at most what the kernel checked */
    *(u32 *)q->r.alen = 16;             /* the true length */
}

static int port_of(const struct netreq *q)
{
    const u8 *a = (const u8 *)q->r.addr;
    return q->alen >= 4 ? a[2] << 8 | a[3] : -1;
}

static s32 serve(struct umsg *m, u32 *b, u32 *b2)
{
    struct netreq *q = (struct netreq *)m->a[1];
    int i = m->type == SOCK_SOCKET ? -1 : by_name(m->a[0]);
    if (m->type != SOCK_SOCKET && i < 0)
        return -ENETDOWN;
    struct sk *s = i >= 0 ? &sk[i] : 0;
    switch (m->type) {
    case SOCK_SOCKET:
        if (q->r.level != AF_INET)
            return -EAFNOSUPPORT;
        if ((q->r.flags & 0xFF) != SOCK_STREAM)
            return -EPROTONOSUPPORT;
        if ((i = fresh()) < 0)
            return -ENOBUFS;
        sk[i].name = m->a[0];
        *b = bits(i);
        return 0;
    case NET_CLOSE:
        if (s->peer >= 0) {
            sk[s->peer].peer_gone = 1;
            sk[s->peer].peer = -1;
            mark(s->peer);
        }
        for (int k = 0; k < NS; k++)    /* connections still waiting to be accepted on it go too */
            if (sk[k].used && !sk[k].name && sk[k].lis == i) {
                if (sk[k].peer >= 0) {
                    sk[sk[k].peer].peer_gone = 1;
                    sk[sk[k].peer].peer = -1;
                    mark(sk[k].peer);
                }
                sk[k].used = 0;
            }
        s->used = 0;
        return 0;
    case SOCK_BIND:
        if (port_of(q) < 0)
            return -EINVAL;
        if (port_of(q) < 1024 && q->uid != 0)
            return -EACCES;
        s->port = (u16)port_of(q);
        break;
    case SOCK_LISTEN:
        s->listening = 1;
        break;
    case SOCK_CONNECT: {
        int port = port_of(q), li = -1;
        if (s->peer >= 0 || s->connecting)
            return s->connecting ? -EALREADY : -EISCONN;
        for (int k = 0; k < NS; k++)
            if (sk[k].used && sk[k].listening && sk[k].port == port)
                li = k;
        if (li < 0)
            return -ECONNREFUSED;
        int j = fresh();
        if (j < 0)
            return -ENOBUFS;
        sk[j].lis = (s8)li;
        sk[j].peer = (s8)i;
        s->peer = (s8)j;
        mark(li);
        if (port >= 8000) {             /* the slow kind: done in a second */
            s->connecting = 1;
            s->done_at = sys_ticks() + 100;
            *b = bits(i);
            return -EINPROGRESS;
        }
        break;
    }
    case SOCK_ACCEPT: {
        int j = backlog(i);
        if (j < 0) {
            *b = bits(i);
            return -EAGAIN;
        }
        sk[j].name = q->newsock;
        put_addr(q, 40000 + j);
        *b = bits(i);
        *b2 = bits(j);
        return 0;
    }
    case SOCK_SENDTO: {
        if (s->peer_gone)
            return -EPIPE;
        if (s->peer < 0)
            return -ENOTCONN;
        struct sk *p = &sk[s->peer];
        u32 room = CAP - p->count, n = q->len < room ? q->len : room;
        if (!n) {
            *b = bits(i);
            return -EAGAIN;
        }
        memcpy(p->buf + p->count, (const void *)q->r.buf, n);
        p->count += n;
        mark(s->peer);
        *b = bits(i);
        return (s32)n;
    }
    case SOCK_RECVFROM: {
        if (!s->count) {
            *b = bits(i);
            return s->peer_gone ? 0 : -EAGAIN;
        }
        u32 n = q->len < s->count ? q->len : s->count;
        memcpy((void *)q->r.buf, s->buf, n);
        memmove(s->buf, s->buf + n, s->count - n);
        s->count -= n;
        if (s->peer >= 0)
            mark(s->peer);              /* room again */
        put_addr(q, 40000 + i);
        *b = bits(i);
        if (lie) {
            lie = 0;
            return 1000000;
        }
        return (s32)n;
    }
    case SOCK_SHUTDOWN:
        if (s->peer >= 0 && q->r.flags != 0) {
            sk[s->peer].peer_gone = 1;
            mark(s->peer);
        }
        break;
    case SOCK_GETSOCKNAME:
        put_addr(q, s->port);
        break;
    case SOCK_GETSOCKOPT:
        if (q->vlen < 4)
            return -EINVAL;
        if (q->r.level == SOL_SOCKET && q->r.option == SO_ERROR)
            *(s32 *)q->r.val = 0;
        else if (q->r.level == ZZ && q->r.option == 4)
            *(u32 *)q->r.val = served;
        else
            return -ENOPROTOOPT;
        *(u32 *)q->r.vlen = 4;
        break;
    case SOCK_SETSOCKOPT:
        if (q->r.level != ZZ)
            return -ENOPROTOOPT;
        if (q->r.option == 1)
            __asm__ volatile("illegal");        /* dies in the middle of the request, never answers */
        if (q->r.option == 2)
            lie = 1;
        if (q->r.option == 3)
            return 0x7FFF0003;                  /* the caller ends the loop (main) */
        if (q->r.option == 5)
            hang_after_close = 1;               /* answer, and hang after the next close is answered (main) */
        if (q->r.option == 6)
            for (;;)                            /* hang inside the request: never answers (audit O9's test) */
                __asm__ volatile("");
        break;
    case SOCK_IOCTL:
        if (q->r.option != FIONREAD)
            return -EINVAL;
        *(s32 *)q->r.val = (s32)s->count;
        break;
    default:
        return -EOPNOTSUPP;
    }
    *b = bits(i);
    return 0;
}

int main(void)
{
    int port = sys_port_create("net");
    if (port < 0) {
        printf("socktestd: port net: %d\n", port);
        return 1;
    }
    epoch = (u32)sys_netsrv(NETSRV_HELLO, 0, 0);
    printf("socktestd: serving net, epoch %u\n", epoch);
    for (;;) {
        u32 now = sys_ticks();
        int wait = 1;
        for (int i = 0; i < NS; i++)
            if (sk[i].used && sk[i].connecting) {
                if ((s32)(now - sk[i].done_at) >= 0) {
                    sk[i].connecting = 0;   /* the slow connect completes */
                    mark(i);
                } else
                    wait = 11;              /* look again in at most 10 ticks */
            }
        if (npush) {
            for (int k = 0; k < npush; k++) {
                int i = by_name(push[k].sock);
                push[k].bits = i >= 0 ? bits(i) : POLLERR | POLLHUP;
            }
            sys_netsrv(NETSRV_READY, (u32)push, (u32)npush);
            npush = 0;
        }
        struct umsg *m = sys_recv(port, wait);
        if ((int)m < 0 && (int)m > -4096)
            return 2;
        if (!m)
            continue;
        served++;
        if (!(m->kflags & UMSG_KERNEL)) {
            sys_reply(m, -EACCES);              /* only the kernel's requests (D19 9.1) */
            continue;
        }
        u32 b = 0, b2 = 0;
        s32 r = serve(m, &b, &b2);
        int si = m->type == NET_CLOSE ? -1 : by_name(m->a[0]);    /* every reply carries the truth (rule 1) */
        b = si >= 0 ? bits(si) : 0;
        if (m->type == SOCK_ACCEPT && r == 0) {
            int ni = by_name(((struct netreq *)m->a[1])->newsock);
            b2 = ni >= 0 ? bits(ni) : 0;
        }
        int quit = r == 0x7FFF0003, hang = hang_after_close && m->type == NET_CLOSE;
        m->a[2] = b | b2 << 16;
        m->a[3] = epoch;
        sys_netsrv(NETSRV_REPLY, (u32)m, quit ? 0 : (u32)r);
        if (quit)
            return 0;
        if (hang)
            for (;;)                            /* runs on, takes no request: S22's loop inside lwIP (audit O9) */
                __asm__ volatile("");
    }
}
