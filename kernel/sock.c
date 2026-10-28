/* SPDX-License-Identifier: GPL-2.0-only */
/* The kernel's socket layer (DESIGN D19, docs/d19-network.md sections 4.1-4.3): a socket is a file (FT_SOCK) whose
 * kernel object, a ksock, names it to the network server on port "net" (its index and a generation). The server owns
 * the network; the kernel owns the waiting:
 *   - every BSD socket call is SYS_sock (op, fd, request): the request is copied here and every pointer in it checked
 *     by the op's directions (an address length clamped to NET_ADDR_MAX, a buffer to NET_BUF_MAX); the server reads
 *     and writes the program's buffers in place, and every count it returns is clamped to what was checked;
 *   - the server never keeps a caller waiting (rule 2): a request it cannot do now is answered -EAGAIN, and the
 *     caller waits here, in the kernel's event wait (D16), where ^C and handlers reach it;
 *   - the ready bits are the server's alone (rule 1): set only inside its own calls, NETSRV_REPLY (a reply and the
 *     socket's new bits in one trap) and NETSRV_READY (changes from the network, once a turn). So a reply of -EAGAIN
 *     always comes with the bits that say "not ready", and a waiter never spins on a stale bit;
 *   - the epoch (rule 9): raised when the server ends (sock_server_gone, from svc.c); every socket of the old epoch is
 *     dead at once (POLLERR|POLLHUP, its waiters woken) and fails with -ENETDOWN; a new socket is kept only if the
 *     server's reply carries the live epoch.
 * Built at -Os (cold: the time goes to the server); tools/sock-host-test.c runs the rules' interleavings on the host. */
#include "kernel.h"
#include "netproto.h"

_Static_assert(sizeof(struct usockreq) == USOCKREQ_FIRST, "usockreq's first size is frozen");
#define NSOCK_ROOT 4                    /* the last ksocks are root's: two users cannot take every socket */

struct ksock {
    u8 used;                            /* KS_PENDING: its SOCKET or ACCEPT is in flight; KS_LIVE */
    u8 dead;                            /* its epoch ended: POLLERR|POLLHUP, every call -ENETDOWN, close only */
    u8 stream;                          /* SOCK_STREAM: a blocking send queues the whole buffer, piece by piece */
    u8 connecting;                      /* a blocking connect is under way: a restarted one only waits (rule 6) */
    u16 gen;
    u16 bits;                           /* the server's ready bits (POLL*) */
    u32 epoch;
    u32 wait;                           /* D16: the slots waiting on it */
    u32 rcvto, sndto;                   /* SO_RCVTIMEO, SO_SNDTIMEO in ticks (0: none) */
};
#define KS_PENDING 1
#define KS_LIVE    2
static struct ksock ks[NSOCK];
u32 net_epoch = 1;
/* the socket a process's call in flight is about (and ACCEPT's new one): NETSRV_REPLY sets their bits from these, the
 * kernel's own record, never from names in the message the server could have changed */
static u32 call_name[NPROC], call_new[NPROC];

static u32 sname(int i) { return (u32)i | (u32)ks[i].gen << 8; }

/* the ksock a name gives, if it is one in use (live or pending), else 0 */
static struct ksock *by_name(u32 name)
{
    u32 i = name & 0xFF;
    if (i >= NSOCK || !ks[i].used || ks[i].gen != (u16)(name >> 8))
        return 0;
    return &ks[i];
}

static void set_bits(u32 name, u32 bits)
{
    struct ksock *k = by_name(name);
    if (!k || k->dead)
        return;
    if (k->bits != (u16)bits) {
        k->bits = (u16)bits;
        ev_wake(&k->wait);              /* waiters check again: the bits are the truth */
    }
}

static int ks_alloc(void)
{
    int nfree = 0, found = -1;
    for (int i = 0; i < NSOCK; i++)
        if (!ks[i].used) {
            nfree++;
            if (found < 0)
                found = i;
        }
    if (found < 0 || (nfree <= NSOCK_ROOT && current->uid != 0))
        return -ENFILE;
    struct ksock *k = &ks[found];
    u16 g = (u16)(k->gen + 1);
    memset(k, 0, sizeof *k);
    k->gen = g;
    k->used = KS_PENDING;
    return found;
}

/* a request to the server. ns: SVC_NS_SKIP for NET_CLOSE (never starts it, never waits for one). The server answers
 * with NETSRV_REPLY, which marks the message: one it did not answer (it is gone, cannot be started, was given up)
 * gives -ENETDOWN, so a peer's ECONNRESET and the server's absence never look alike */
static long net_call(u32 type, u32 name, struct netreq *q, int ns, struct umsg *m)
{
    int port = port_find("net");
    if (port < 0)
        return -ENETDOWN;               /* no network service registered */
    memset(m, 0, sizeof *m);
    m->type = type;
    m->a[0] = name;
    m->a[1] = (u32)q;
    call_name[current - procs] = name;
    long r = ipc_kcall(port, m, ns);
    call_name[current - procs] = 0;
    return (m->kflags & UMSG_NETREPLY) ? r : -ENETDOWN;
}

/* the blocking loop (rule 3): need is what the call waits for (0: it never waits); POLLERR and POLLHUP always end the
 * wait, the server then says what happened. A wake-up posted while the call is in flight stays pending; after an
 * -EAGAIN the loop starts at the top, so it reads the bits that reply set */
static long sock_req(int i, u32 type, struct netreq *q, u16 need, int nonblock, u32 timeout, int restart)
{
    struct ksock *k = &ks[i];
    u32 until = ticks + timeout;
    for (;;) {
        if (current->killed)
            return -EINTR;
        ev_arm(&k->wait);
        if (k->dead)
            return -ENETDOWN;
        if (!need || nonblock || (k->bits & (need | POLLERR | POLLHUP))) {
            struct umsg m;
            long r = net_call(type, sname(i), q, 0, &m);
            if (k->dead && r == -ENETDOWN)
                return r;
            if (r != -EAGAIN || nonblock || !need)
                return r;
            continue;
        }
        if (timeout && (s32)(ticks - until) >= 0)
            return -EAGAIN;             /* SO_RCVTIMEO / SO_SNDTIMEO */
        long e = ev_block(EV_IO, timeout != 0, until, restart);
        if (e)
            return e;
    }
}

/* the last close (file_close): NET_CLOSE only while its epoch lives (a dying process never waits for a new server
 * that never knew the socket); freed after the reply */
void sock_release(u32 i)
{
    if (i >= NSOCK)
        return;
    struct ksock *k = &ks[i];
    if (k->used == KS_LIVE && !k->dead && k->epoch == net_epoch) {
        struct netreq q;
        struct umsg m;
        memset(&q, 0, sizeof q);
        q.pid = (u32)current->pid;
        net_call(NET_CLOSE, sname((int)i), &q, SVC_NS_SKIP, &m);
    }
    ev_wake(&k->wait);
    k->used = 0;
}

/* netd ended (svc.c, before its memory is freed): a new epoch; every socket of the old one is dead and its waiters
 * are woken (rule 10). Pending ones are decided by their reply's epoch */
void sock_server_gone(void)
{
    net_epoch++;
    for (int i = 0; i < NSOCK; i++) {
        struct ksock *k = &ks[i];
        if (k->used == KS_LIVE && !k->dead) {
            k->dead = 1;
            k->bits = POLLERR | POLLHUP;
            ev_wake(&k->wait);
        }
    }
}

u16 sock_poll(struct file *f)
{
    struct ksock *k = &ks[f->ino < NSOCK ? f->ino : 0];
    ev_arm(&k->wait);
    return k->dead ? POLLERR | POLLHUP : k->bits;
}

/* a new socket (SOCKET, ACCEPT): a ksock, a file and a free fd reserved first (rule 7), kept only if the server made
 * the socket in the live epoch. The fd, or the error */
static long new_sock(u32 type, int li, struct netreq *q, int nonblock, int stream, int *again)
{
    *again = 0;
    int j = ks_alloc();
    if (j < 0)
        return j;
    struct file *f = file_new(FT_SOCK);
    if (!f) {
        ks[j].used = 0;
        return -ENFILE;
    }
    int fd = fd_install(current, f);
    if (fd < 0) {
        f->type = FT_NONE;
        ks[j].used = 0;
        return fd;
    }
    current->fd[fd] = 0;                /* only reserved: installed below once the server made it */
    f->ino = (u32)j;
    q->newsock = sname(j);
    struct umsg m;
    call_new[current - procs] = q->newsock;
    long r = net_call(type, li < 0 ? q->newsock : sname(li), q, 0, &m);
    call_new[current - procs] = 0;
    if (r >= 0 && m.a[3] == net_epoch && !current->killed && !current->fd[fd]) {
        ks[j].used = KS_LIVE;
        ks[j].epoch = net_epoch;
        ks[j].stream = (u8)stream;
        if (nonblock)
            f->flags |= O_NONBLOCK;
        f->flags |= O_RDWR;
        current->fd[fd] = f;
        return fd;
    }
    if (r >= 0) {                       /* made, but not to be kept: the server forgets it again */
        ks[j].used = KS_LIVE;
        ks[j].epoch = m.a[3];
        sock_release((u32)j);
        r = current->killed ? -EINTR : -ENETDOWN;
    }
    ks[j].used = 0;
    f->type = FT_NONE;
    if (r == -EAGAIN)
        *again = 1;
    return r;
}

static long sock_accept(int i, struct netreq *q, int nonblock)
{
    struct ksock *k = &ks[i];
    u32 until = ticks + k->rcvto;
    for (;;) {
        if (current->killed)
            return -EINTR;
        ev_arm(&k->wait);
        if (k->dead)
            return -ENETDOWN;
        if (nonblock || (k->bits & (POLLIN | POLLERR | POLLHUP))) {
            int again;
            long r = new_sock(SOCK_ACCEPT, i, q, (q->r.flags & SOCK_NONBLOCK) != 0, 1, &again);
            if (!again || nonblock)
                return r;
            continue;
        }
        if (k->rcvto && (s32)(ticks - until) >= 0)
            return -EAGAIN;
        long e = ev_block(EV_IO, k->rcvto != 0, until, !k->rcvto);
        if (e)
            return e;
    }
}

/* rule 6: CONNECT once; a blocking one then waits for POLLOUT or POLLERR and asks SO_ERROR (never CONNECT again: a
 * second connect would fail). Interrupted, it returns -EINTR and the connection goes on; restarted, it only waits */
static long sock_connect(int i, struct netreq *q, int nonblock)
{
    struct ksock *k = &ks[i];
    if (!k->connecting || nonblock) {
        long r = sock_req(i, SOCK_CONNECT, q, 0, 1, 0, 0);
        if (r != -EINPROGRESS || nonblock)
            return r;
        k->connecting = 1;
    }
    u32 until = ticks + k->sndto;
    for (;;) {
        if (current->killed)
            return -EINTR;
        ev_arm(&k->wait);
        if (k->dead)
            return -ENETDOWN;
        if (k->bits & (POLLOUT | POLLERR | POLLHUP))
            break;
        if (k->sndto && (s32)(ticks - until) >= 0)
            return -EINPROGRESS;        /* SO_SNDTIMEO ran out: the connection goes on (as Linux) */
        long e = ev_block(EV_IO, k->sndto != 0, until, !k->sndto);
        if (e)
            return e;
    }
    s32 err = 0;
    u32 four = 4;
    struct netreq e;
    memset(&e, 0, sizeof e);
    e.r.level = SOL_SOCKET;
    e.r.option = SO_ERROR;
    e.r.val = (u32)&err;                /* the server writes the answer here, in this kernel stack */
    e.r.vlen = (u32)&four;
    e.vlen = 4;
    e.pid = (u32)current->pid;
    e.uid = current->uid;
    e.gid = current->gid;
    struct umsg m;
    long r = net_call(SOCK_GETSOCKOPT, sname(i), &e, 0, &m);
    k->connecting = 0;
    return r < 0 ? r : err > 0 ? -err : 0;
}

static void sigpipe(u32 flags)
{
    if (!(flags & MSG_NOSIGNAL))
        sig_send(current, SIGPIPE);     /* by default the sender ends (141), as on a pipe */
}

/* rule 4: a blocking stream send queues the whole buffer, at most NET_BUF_MAX a message; after progress a signal or
 * a timeout returns the count (no restart: SA_RESTART never sends the start again) */
static long sock_send(int i, struct netreq *q, int nonblock)
{
    struct ksock *k = &ks[i];
    u32 buf = q->r.buf, total = q->r.len, done = 0;
    if (!k->stream && total > NET_BUF_MAX)
        return -EMSGSIZE;
    do {
        q->r.buf = buf + done;
        q->len = total - done < NET_BUF_MAX ? total - done : NET_BUF_MAX;
        long r = sock_req(i, SOCK_SENDTO, q, POLLOUT, nonblock, k->sndto, !k->sndto && !done);
        if (r < 0) {
            if (r == -EPIPE || r == -ENETDOWN) {
                if (done)
                    return (long)done;
                sigpipe(q->r.flags);
            }
            if (done && r == -EAGAIN && k->sndto)
                return (long)done;
            return done ? (long)done : r;
        }
        if (!r)
            break;                      /* nothing taken and no error: the server's limit; never spin */
        done += (u32)r < q->len ? (u32)r : q->len;
    } while (k->stream && !nonblock && done < total);
    return (long)done;
}

static long sock_recv(int i, struct netreq *q, int nonblock)
{
    struct ksock *k = &ks[i];
    long r = sock_req(i, SOCK_RECVFROM, q, POLLIN, nonblock, k->rcvto, !k->rcvto);
    return r > (long)q->len ? (long)q->len : r;
}

/* read() and write() on a socket: recv and send without flags or an address */
long sock_rw(struct file *f, void *buf, u32 n, int write)
{
    struct netreq q;
    memset(&q, 0, sizeof q);
    q.r.buf = (u32)buf;
    q.r.len = n;
    q.len = n < NET_BUF_MAX ? n : NET_BUF_MAX;
    q.pid = (u32)current->pid;
    q.uid = current->uid;
    q.gid = current->gid;
    int nb = (f->flags & O_NONBLOCK) != 0;
    if (f->ino >= NSOCK)
        return -EBADF;
    return write ? sock_send((int)f->ino, &q, nb) : sock_recv((int)f->ino, &q, nb);
}

/* the directions of each op's pointers (the server reads R, writes W) */
#define D_BUFR 0x01
#define D_BUFW 0x02
#define D_ADDRR 0x04
#define D_ADDRW 0x08
#define D_VALR 0x10
#define D_VALW 0x20
static const u8 dirs[SOCK_OPS + 1] = {
    [SOCK_BIND] = D_ADDRR, [SOCK_CONNECT] = D_ADDRR, [SOCK_ACCEPT] = D_ADDRW,
    [SOCK_SENDTO] = D_BUFR | D_ADDRR, [SOCK_RECVFROM] = D_BUFW | D_ADDRW,
    [SOCK_GETSOCKOPT] = D_VALW, [SOCK_SETSOCKOPT] = D_VALR, [SOCK_GETSOCKNAME] = D_ADDRW, [SOCK_GETPEERNAME] = D_ADDRW,
    [SOCK_IOCTL] = D_VALR | D_VALW, [SOCK_RESOLVE] = D_BUFR | D_ADDRW, [SOCK_IFLIST] = D_BUFW, [SOCK_IFSET] = D_BUFR,
    [SOCK_ROUTE] = D_BUFR | D_BUFW, [SOCK_NETSTAT] = D_BUFW,
};

/* a length behind a pointer (alen, vlen): checked, read, clamped. 0, or -EFAULT */
static long ulen(u32 p, int write, u32 max, u32 *out)
{
    if (!user_ok_rd(current, p, 4) || (write && !user_ok_wr(current, p, 4)))
        return -EFAULT;
    u32 v = *(const u32 *)p;
    *out = v < max ? v : max;
    return 0;
}

static int ok_dir(u32 p, u32 n, int w)
{
    return !n || (w ? user_ok_wr(current, p, n) : user_ok_rd(current, p, n));
}

/* SYS_sock (op | request size << 16, fd, request) */
long sock_sys(u32 opw, u32 fd, u32 ureq)
{
    u32 op = opw & 0xFFFF, rsize = opw >> 16;
    if (op < 1 || op > SOCK_OPS || (rsize && rsize < USOCKREQ_FIRST))
        return -EINVAL;
    struct netreq q;
    memset(&q, 0, sizeof q);
    u32 n = rsize && rsize < sizeof q.r ? rsize : sizeof q.r;
    if (!user_ok_rd(current, ureq, n))
        return -EFAULT;
    memcpy(&q.r, (const void *)ureq, n);
    q.pid = (u32)current->pid;
    q.uid = current->uid;
    q.gid = current->gid;
    struct file *f = 0;
    if (op != SOCK_SOCKET) {            /* the descriptor first, as Linux: -EBADF, -ENOTSOCK before any -EFAULT */
        f = fd < NFD ? current->fd[fd] : 0;
        if (!f)
            return -EBADF;
        if (f->type != FT_SOCK || f->ino >= NSOCK)
            return -ENOTSOCK;
    }
    if (op == SOCK_IOCTL && q.r.option == FIONBIO && !q.r.val)
        return (f->flags & O_NONBLOCK) != 0;    /* asks (libz's fcntl F_GETFL): an error before step 4 */
    u8 d = dirs[op];
    if (d & (D_ADDRR | D_ADDRW)) {
        if (q.r.addr || q.r.alen) {
            if (!q.r.alen || ulen(q.r.alen, (d & D_ADDRW) != 0, NET_ADDR_MAX, &q.alen) ||
                !ok_dir(q.r.addr, q.alen, (d & D_ADDRW) != 0) || (q.alen && !q.r.addr))
                return -EFAULT;
        }
    }
    if (d & (D_VALR | D_VALW)) {
        if (op == SOCK_IOCTL)
            q.vlen = 4;
        else if (ulen(q.r.vlen, (d & D_VALW) != 0, NET_VAL_MAX, &q.vlen))
            return -EFAULT;
        if ((d & D_VALR) && !ok_dir(q.r.val, q.vlen, 0))
            return -EFAULT;
        if ((d & D_VALW) && !ok_dir(q.r.val, q.vlen, 1))
            return -EFAULT;
    }
    if (d & (D_BUFR | D_BUFW)) {
        q.len = q.r.len < NET_BUF_MAX ? q.r.len : NET_BUF_MAX;
        /* a stream send checks its whole buffer, the pieces after the first are sent from it too */
        u32 chk = op == SOCK_SENDTO ? q.r.len : q.len;
        if (((d & D_BUFR) && !ok_dir(q.r.buf, chk, 0)) || ((d & D_BUFW) && !ok_dir(q.r.buf, q.len, 1)))
            return -EFAULT;
    }
    if (op == SOCK_SOCKET) {
        int again;
        u32 t = q.r.flags & 0xF;
        if (q.r.flags & ~(0xFu | SOCK_NONBLOCK | SOCK_CLOEXEC))
            return -EINVAL;             /* as Linux; netd reads the type as these 4 bits too (audit 2026-10-28 S7) */
        return new_sock(SOCK_SOCKET, -1, &q, (q.r.flags & SOCK_NONBLOCK) != 0, t == SOCK_STREAM, &again);
    }
    int i = (int)f->ino;
    struct ksock *k = &ks[i];
    int nb = (f->flags & O_NONBLOCK) || (q.r.flags & MSG_DONTWAIT);
    if (k->dead && op != SOCK_GETSOCKOPT)
        return op == SOCK_SENDTO ? (sigpipe(q.r.flags), -ENETDOWN) : -ENETDOWN;
    /* the kernel's own options: the timeouts of its wait, the file's O_NONBLOCK */
    if ((op == SOCK_SETSOCKOPT || op == SOCK_GETSOCKOPT) && q.r.level == SOL_SOCKET &&
        (q.r.option == SO_RCVTIMEO || q.r.option == SO_SNDTIMEO)) {
        u32 *t = q.r.option == SO_RCVTIMEO ? &k->rcvto : &k->sndto;
        if (q.vlen < 8)
            return -EINVAL;
        u32 *tv = (u32 *)q.r.val;
        if (op == SOCK_GETSOCKOPT) {
            tv[0] = *t / 100;
            tv[1] = *t % 100 * 10000;
            *(u32 *)q.r.vlen = 8;
            return 0;
        }
        if (tv[1] >= 1000000)
            return -EINVAL;
        /* at most 2^31 - 16 ticks (248 days): the wait's deadline test is signed, so a longer one (the old cap
         * 0xFFFFFFF0) was over at once (audit 2026-10-28 S6) */
        *t = tv[0] > 21000000 ? 0x7FFFFFF0u : tv[0] * 100 + (tv[1] + 9999) / 10000;
        return 0;
    }
    if (op == SOCK_IOCTL && q.r.option == FIONBIO) {
        if (*(const s32 *)q.r.val)
            f->flags |= O_NONBLOCK;
        else
            f->flags &= (u16)~O_NONBLOCK;
        return 0;
    }
    if (k->dead)
        return -ENETDOWN;
    switch (op) {
    case SOCK_ACCEPT:
        return sock_accept(i, &q, nb);
    case SOCK_CONNECT:
        return sock_connect(i, &q, nb);
    case SOCK_SENDTO:
        return sock_send(i, &q, nb);
    case SOCK_RECVFROM:
        return sock_recv(i, &q, nb);
    }
    long r = sock_req(i, op, &q, 0, 1, 0, 0);
    if (op == SOCK_GETSOCKOPT && q.r.level == SOL_SOCKET && q.r.option == SO_ERROR)
        k->connecting = 0;
    if (r > (long)q.len && (d & D_BUFW))
        r = (long)q.len;
    return r;
}

/* SYS_netsrv (op, a, b): only the process the kernel started for "net" */
long netsrv_sys(u32 op, u32 a, u32 b)
{
    if (!svc_is("net"))
        return -EPERM;
    if (op == NETSRV_HELLO)
        return (long)net_epoch;
    if (op == NETSRV_READY) {
        if (b > NSOCK)
            return -EINVAL;
        if (!user_ok_rd(current, a, b * sizeof(struct unetready)))
            return -EFAULT;
        const struct unetready *it = (const struct unetready *)a;
        long n = 0;
        for (u32 j = 0; j < b; j++) {
            struct ksock *k = by_name(it[j].sock);
            if (k && !k->dead) {
                set_bits(it[j].sock, it[j].bits & 0xFFFF);
                n++;
            }
        }
        return n;
    }
    if (op != NETSRV_REPLY)
        return -EINVAL;
    struct umsg *m = (struct umsg *)a;
    struct proc *c = ipc_client(m);     /* the request must be in flight and served by the caller: before m is read */
    if (!c)
        return -EINVAL;
    if (m->kflags & UMSG_KERNEL) {
        int ci = (int)(c - procs);
        if (call_name[ci])
            set_bits(call_name[ci], m->a[2] & 0xFFFF);
        if (call_new[ci] && call_new[ci] != call_name[ci])
            set_bits(call_new[ci], m->a[2] >> 16);
    }
    m->kflags |= UMSG_NETREPLY;
    return ipc_reply(m, (s32)b);
}
