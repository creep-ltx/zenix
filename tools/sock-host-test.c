/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the socket layer's rules (DESIGN D19 4.3, kernel/sock.c with kernel/event.c), built 32-bit against a
 * model of the scheduler and of the network server: a wake-up (the server's push) at every point of the blocking loop
 * - before the call, between the check and the masked test, during the server's call, while asleep - is never lost;
 * a stale "ready" bit costs one call and then the waiter sleeps (no spin); a handler cuts the wait (-EINTR, restart
 * only without a timeout); the server's end wakes every waiter with -ENETDOWN, and an end in the middle of a request
 * gives -ENETDOWN, never a peer's ECONNRESET; a new socket in a stale epoch is dropped; NET_CLOSE only while the
 * epoch lives, as a no-start call; a partial send after progress returns its count without a restart; accept gives
 * its reservations back; a restarted connect never sends CONNECT again; poll reads the bits without a message; only
 * the server may use SYS_netsrv. Build and run: tools/sock-host-test.sh */
#include <stdio.h>
#include <setjmp.h>
#include "../kernel/event.c"
#include "../kernel/sock.c"

int printf(const char *, ...);
void *memcpy(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { char *a = d; while (n--) *a++ = (char)c; return d; }

/* ---- the scheduler's model ---- */
struct proc procs[NPROC];
struct proc *current;
volatile u32 ticks, wake_min;
int ntimer;
#define NETD 5                          /* the server's slot */
static jmp_buf out;
static int hung, blocks;
static void (*producer_hook)(void);     /* once, at the next irq_save: between the check and the masked test */
static void (*sched_hook)(void);        /* once, while the waiter sleeps: another process (the server's turn) */
u16 irq_save(void) { if (producer_hook) { void (*h)(void) = producer_hook; producer_hook = 0; h(); } return 0; }
void irq_restore(u16 s) { (void)s; }
void ready(struct proc *p)
{
    if (p->state == P_STOP || p->state == P_READY || p->state == P_FREE || p->state == P_ZOMBIE)
        return;
    if (p->state == P_SLEEP)
        ntimer--;
    p->state = P_READY;
}
void sleep_until(u32 t) { current->wake = t; current->state = P_SLEEP; ntimer++; }
void schedule(void)
{
    if (current->state != P_EV && current->state != P_SLEEP)
        return;
    blocks++;
    struct proc *w = current;
    if (sched_hook) {
        void (*h)(void) = sched_hook;
        sched_hook = 0;
        h();
    }
    if (w->state == P_SLEEP) {          /* a timed wait: the clock runs to its end */
        ticks = w->wake;
        ready(w);
    }
    if (w->state == P_EV) {             /* nobody will ever wake it */
        hung = 1;
        longjmp(out, 1);
    }
}
int proc_kill_now(void) { return current->killed != 0; }
void proc_kill(struct proc *p) { p->killed = 3; }           /* the watchdog (audit O9): never reached here */
void kprintf(const char *fmt, ...) { (void)fmt; }
int user_ok_wr(const struct proc *p, u32 ptr, u32 len) { (void)p; return ptr >= 0x1000 || !len; }
int user_ok_rd(const struct proc *p, u32 ptr, u32 len) { return user_ok_wr(p, ptr, len); }
static int sigs[NSIG];
u16 port_poll(int port) { (void)port; return POLLNVAL; }
u16 vfs_poll(struct file *f) { return sock_poll(f); }
void sig_send(struct proc *p, int sig) { (void)p; sigs[sig]++; }

/* files: the model's table */
static struct file files[16];
struct file *file_new(int type)
{
    for (int i = 0; i < 16; i++)
        if (!files[i].type) { memset(&files[i], 0, sizeof files[i]); files[i].type = (u8)type; files[i].refs = 1; return &files[i]; }
    return 0;
}
int fd_install(struct proc *p, struct file *f)
{
    for (int i = 0; i < NFD; i++)
        if (!p->fd[i]) { p->fd[i] = f; return i; }
    return -EMFILE;
}

/* ---- the server's model ---- */
static int registered = 1, srv_dies, stale_epoch, calls, ncalls[32], last_ns;
static int srv_data, srv_again_stale, push_after, connect_inprogress, send_chunk;
static struct umsg *inflight;
static struct proc *inflight_client;
int port_find(const char *n) { (void)n; return registered ? 5 : -1; }
struct proc *ipc_client(struct umsg *m) { return m && m == inflight && current == &procs[NETD] ? inflight_client : 0; }
long ipc_reply(struct umsg *m, s32 r) { m->result = r; inflight = 0; return 0; }
int svc_is(const char *n) { (void)n; return current == &procs[NETD]; }

static void as_server(void (*f)(void *), void *arg)
{
    struct proc *c = current;
    current = &procs[NETD];
    f(arg);
    current = c;
}
static void push_one(void *arg)         /* NETSRV_READY for one socket */
{
    struct unetready *u = arg;
    netsrv_sys(NETSRV_READY, (u32)u, 1);
}
static struct unetready pending;
static void push(void) { as_server(push_one, &pending); }
static void reply(struct umsg *m, s32 r, u32 bits) { m->a[2] = bits; m->a[3] = net_epoch - (u32)stale_epoch; netsrv_sys(NETSRV_REPLY, (u32)m, (u32)r); }

/* the server's answer to one request: the rules' netd */
static void serve(void *arg)
{
    struct umsg *m = arg;
    struct netreq *q = (struct netreq *)m->a[1];
    if (srv_dies) {                     /* it dies in the middle of this request: no reply */
        srv_dies = 0;
        sock_server_gone();
        return;
    }
    switch (m->type) {
    case SOCK_SOCKET: reply(m, 0, POLLOUT); break;
    case NET_CLOSE: reply(m, 0, 0); break;
    case SOCK_RECVFROM:
        if (srv_again_stale) {          /* the bit said ready, but it is not: -EAGAIN and the truth */
            srv_again_stale = 0;
            reply(m, -EAGAIN, POLLOUT);
            if (push_after) {           /* ... and in the same turn the data comes, pushed after the reply */
                push_after = 0;
                srv_data = 1;
                struct unetready u = { m->a[0], POLLIN | POLLOUT };
                netsrv_sys(NETSRV_READY, (u32)&u, 1);
            }
            break;
        }
        if (!srv_data) { reply(m, -EAGAIN, POLLOUT); break; }
        srv_data = 0;
        reply(m, 7, POLLOUT);
        break;
    case SOCK_SENDTO: {
        if (!send_chunk) { reply(m, -EAGAIN, 0); break; }
        u32 n = q->len < (u32)send_chunk ? q->len : (u32)send_chunk;
        send_chunk = 0;
        reply(m, (s32)n, 0);
        break;
    }
    case SOCK_ACCEPT: reply(m, -EAGAIN, 0); break;
    case SOCK_CONNECT: reply(m, connect_inprogress ? -EINPROGRESS : 0, 0); break;
    case SOCK_GETSOCKOPT: *(s32 *)q->r.val = 0; reply(m, 0, POLLOUT); break;
    default: reply(m, 0, POLLOUT);
    }
}
long ipc_kcall(int port, struct umsg *m, int ns)
{
    (void)port;
    calls++;
    if (m->type < 32) ncalls[m->type]++;
    last_ns = ns;
    m->kflags = UMSG_KERNEL;
    m->state = MSG_SERVING;
    if (!registered) return -ECONNRESET;
    inflight = m;
    inflight_client = current;
    m->result = -ECONNRESET;            /* what ipc gives a request whose server ended */
    as_server(serve, m);
    inflight = 0;
    return m->result;
}

static int bad;
#define EXPECT(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); bad++; } else printf("ok:   %s\n", msg); } while (0)

static void clear(void)
{
    memset(procs, 0, sizeof procs);
    memset(files, 0, sizeof files);
    memset(ks, 0, sizeof ks);
    memset(sigs, 0, sizeof sigs);
    memset(ncalls, 0, sizeof ncalls);
    for (int i = 0; i < NPROC; i++) procs[i].pid = i ? 100 + i : 0;
    current = &procs[1];
    procs[1].state = P_READY;
    procs[NETD].state = P_READY;
    registered = 1; srv_dies = stale_epoch = calls = srv_data = srv_again_stale = push_after = connect_inprogress = send_chunk = 0;
    hung = blocks = 0;
    producer_hook = 0; sched_hook = 0;
    ntimer = 0;
}

static int newsock(void)
{
    struct usockreq r;
    memset(&r, 0, sizeof r);
    r.level = AF_INET;
    r.flags = SOCK_STREAM;
    return (int)sock_sys(SOCK_SOCKET, 0, (u32)&r);
}
static char buf[4096];
static long do_recv(int fd)
{
    struct usockreq r;
    memset(&r, 0, sizeof r);
    r.buf = (u32)buf;
    r.len = 100;
    return sock_sys(SOCK_RECVFROM, (u32)fd, (u32)&r);
}
static void data_arrives(void)          /* the network: data for socket fd 0, pushed by the server */
{
    srv_data = 1;
    pending.sock = current->fd[0] ? sname((int)current->fd[0]->ino) : 0;
    pending.bits = POLLIN | POLLOUT;
    push();
}
static struct proc *waiter;
static void data_arrives_w(void) { struct proc *c = current; current = waiter; data_arrives(); current = c; }

/* a recv with the data arriving at point `at`; 1 if it got the data without a lost wake-up */
static int recv_at(int at, int *ncall, int *nblock)
{
    clear();
    int fd = newsock();
    waiter = current;
    calls = 0;
    long r = -1;
    if (!setjmp(out)) {
        if (at == 0) data_arrives();                    /* before the call */
        if (at == 2) producer_hook = data_arrives_w;    /* between the check and the masked test */
        if (at == 3) {                                  /* during the call: a stale bit, -EAGAIN, then the push */
            ks[current->fd[fd]->ino].bits = POLLIN | POLLOUT;
            srv_again_stale = push_after = 1;
        }
        if (at == 4) sched_hook = data_arrives_w;       /* while it sleeps */
        r = do_recv(fd);
    }
    *ncall = calls;
    *nblock = blocks;
    return !hung && r == 7;
}

static void netd_ends(void) { as_server((void (*)(void *))sock_server_gone, 0); }
static void handler_due(void) { waiter->sig_catch |= SIGBIT(SIGALRM); waiter->sig_pend |= SIGBIT(SIGALRM); ev_post(waiter, EV_SIG); }

int main(void)
{
    int nc, nb;
    EXPECT(recv_at(0, &nc, &nb) && nc == 1 && nb == 0, "data before the recv: one call, no sleep");
    EXPECT(recv_at(2, &nc, &nb) && nc == 1, "a push between the check and the masked test: not lost, one call");
    EXPECT(recv_at(3, &nc, &nb) && nc == 2 && nb == 0, "a push during the server's turn (after its -EAGAIN): not lost, no sleep");
    EXPECT(recv_at(4, &nc, &nb) && nc == 1 && nb == 1, "a push while it sleeps wakes it: one call after the wake");

    /* no spin: a stale POLLIN costs one call (the reply says not ready), then it sleeps */
    clear();
    int fd = newsock();
    waiter = current;
    ks[current->fd[fd]->ino].bits = POLLIN | POLLOUT;
    srv_again_stale = 1;
    calls = 0;
    if (!setjmp(out))
        do_recv(fd);
    EXPECT(hung && calls == 1 && ks[current->fd[fd]->ino].bits == POLLOUT, "a stale ready bit: one call, then asleep (no spin)");

    /* a handler cuts the wait: -EINTR, restartable without a timeout, not with one */
    clear();
    fd = newsock();
    waiter = current;
    sched_hook = handler_due;
    static long r;
    r = 0;
    if (!setjmp(out)) r = do_recv(fd);
    EXPECT(!hung && r == -EINTR && current->sig_intr == 1, "a handler: -EINTR, restarted by SA_RESTART");
    clear();
    fd = newsock();
    waiter = current;
    ks[current->fd[fd]->ino].rcvto = 500;
    sched_hook = handler_due;
    current->sig_intr = 0;
    if (!setjmp(out)) r = do_recv(fd);
    EXPECT(!hung && r == -EINTR && current->sig_intr == 0, "... with SO_RCVTIMEO set: never restarted");
    clear();
    fd = newsock();
    ks[current->fd[fd]->ino].rcvto = 40;
    ticks = 1000;
    if (!setjmp(out)) r = do_recv(fd);
    EXPECT(!hung && r == -EAGAIN && ticks >= 1040, "SO_RCVTIMEO: -EAGAIN at the deadline");
    /* audit 2026-10-28 S7: unknown type bits are -EINVAL (netd took 0x21 for a stream, the kernel for none) */
    clear();
    {
        struct usockreq q;
        memset(&q, 0, sizeof q);
        q.level = AF_INET;
        q.flags = 0x21;
        long sr = sock_sys(SOCK_SOCKET, 0, (u32)&q);
        q.flags = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
        long sr2 = sock_sys(SOCK_SOCKET, 0, (u32)&q);
        EXPECT(sr == -EINVAL && sr2 >= 0, "socket type 0x21: -EINVAL; SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC works");
    }
    /* audit 2026-10-28 S6: a timeout of 2^31 ticks or more (248 days, 0xFFFFFFF0 for "about forever") was already
     * over when the wait began: -EAGAIN at once */
    clear();
    fd = newsock();
    {
        static u32 tv[2] = { 50000000, 0 };
        static u32 tvlen = 8;
        struct usockreq q;
        memset(&q, 0, sizeof q);
        q.level = SOL_SOCKET; q.option = SO_RCVTIMEO; q.val = (u32)tv; q.vlen = (u32)&tvlen;
        long sr = sock_sys(SOCK_SETSOCKOPT, (u32)fd, (u32)&q);
        ticks = 1000;
        blocks = 0;
        if (!setjmp(out)) r = do_recv(fd);
        EXPECT(sr == 0 && !hung && r == -EAGAIN && blocks == 1 && ticks - 1000 >= 0x7F000000u,
               "SO_RCVTIMEO of 50,000,000 s: waits (about 248 days, the most), not -EAGAIN at once");
    }

    /* the server's end wakes every waiter; the dead socket answers -ENETDOWN without a message */
    clear();
    fd = newsock();
    waiter = current;
    u32 e0 = net_epoch;
    sched_hook = netd_ends;
    calls = 0;
    if (!setjmp(out)) r = do_recv(fd);
    EXPECT(!hung && r == -ENETDOWN && calls == 0 && net_epoch == e0 + 1, "the server ends while one waits: woken, -ENETDOWN, a new epoch");
    EXPECT(sock_poll(current->fd[fd]) == (POLLERR | POLLHUP) && do_recv(fd) == -ENETDOWN && calls == 0,
           "... the socket is dead: poll POLLERR|POLLHUP, calls -ENETDOWN, no message");
    calls = 0;
    sock_release(current->fd[fd]->ino);
    EXPECT(calls == 0, "... its close sends nothing (the epoch is gone)");

    /* an end in the middle of a request: -ENETDOWN, never ECONNRESET */
    clear();
    fd = newsock();
    srv_data = 1;
    srv_dies = 1;
    ks[current->fd[fd]->ino].bits = POLLIN | POLLOUT;
    r = 0;
    if (!setjmp(out)) r = do_recv(fd);
    EXPECT(r == -ENETDOWN && ks[current->fd[fd]->ino].dead, "the server dies in the request: -ENETDOWN (not -ECONNRESET), the socket dead");
    registered = 0;
    EXPECT(newsock() == -ENETDOWN, "no server registered: -ENETDOWN");

    /* a new socket in a stale epoch is dropped, its reservations given back */
    clear();
    stale_epoch = 1;
    int before = 0;
    for (int i = 0; i < NSOCK; i++) before += !ks[i].used;
    r = newsock();
    int after = 0;
    for (int i = 0; i < NSOCK; i++) after += !ks[i].used;
    int fds = 0;
    for (int i = 0; i < NFD; i++) fds += current->fd[i] != 0;
    EXPECT(r == -ENETDOWN && before == after && !fds, "a reply in a stale epoch: no socket kept, nothing left reserved");

    /* close: NET_CLOSE as a no-start call while the epoch lives */
    clear();
    fd = newsock();
    calls = 0;
    sock_release(current->fd[fd]->ino);
    EXPECT(calls == 1 && ncalls[NET_CLOSE] == 1 && last_ns == SVC_NS_SKIP, "the last close: NET_CLOSE, a no-start call");

    /* partial send: progress, then a handler: the count, no restart */
    clear();
    fd = newsock();
    waiter = current;
    ks[current->fd[fd]->ino].stream = 1;
    send_chunk = 1000;
    sched_hook = handler_due;
    struct usockreq rq;
    memset(&rq, 0, sizeof rq);
    rq.buf = (u32)buf;
    rq.len = 3000;
    if (!setjmp(out)) r = sock_sys(SOCK_SENDTO, (u32)fd, (u32)&rq);
    EXPECT(!hung && r == 1000 && current->sig_intr == 0, "a stream send: 1000 queued, then a handler: 1000, no restart");
    memset(&rq, 0, sizeof rq);
    rq.buf = (u32)buf;
    rq.len = 3000;
    ks[current->fd[fd]->ino].stream = 0;
    send_chunk = 0;
    ks[current->fd[fd]->ino].bits = POLLOUT;
    rq.flags = MSG_DONTWAIT;
    r = sock_sys(SOCK_SENDTO, (u32)fd, (u32)&rq);
    EXPECT(r == -EAGAIN, "MSG_DONTWAIT: -EAGAIN at once");
    rq.len = 70000;
    EXPECT(sock_sys(SOCK_SENDTO, (u32)fd, (u32)&rq) == -EMSGSIZE, "a datagram over 64 KB: -EMSGSIZE");

    /* accept: the reservations go back on -EAGAIN and on -EINTR */
    clear();
    int l = newsock();
    waiter = current;
    ks[current->fd[l]->ino].bits = POLLIN;            /* stale: the server says -EAGAIN */
    sched_hook = handler_due;
    memset(&rq, 0, sizeof rq);
    before = 0;
    for (int i = 0; i < NSOCK; i++) before += !ks[i].used;
    if (!setjmp(out)) r = sock_sys(SOCK_ACCEPT, (u32)l, (u32)&rq);
    after = 0;
    for (int i = 0; i < NSOCK; i++) after += !ks[i].used;
    int nf = 0;
    for (int i = 0; i < 16; i++) nf += files[i].type != 0;
    EXPECT(!hung && r == -EINTR && before == after && nf == 1, "accept: -EAGAIN then a handler: -EINTR, every reservation back");

    /* connect: CONNECT once; interrupted, then restarted: it only waits */
    clear();
    fd = newsock();
    waiter = current;
    connect_inprogress = 1;
    sched_hook = handler_due;
    memset(&rq, 0, sizeof rq);
    if (!setjmp(out)) r = sock_sys(SOCK_CONNECT, (u32)fd, (u32)&rq);
    EXPECT(!hung && r == -EINTR && ks[current->fd[fd]->ino].connecting && ncalls[SOCK_CONNECT] == 1, "a blocking connect interrupted: -EINTR, it goes on");
    current->sig_pend = 0;
    pending.sock = sname((int)current->fd[fd]->ino);
    pending.bits = POLLOUT;
    sched_hook = push;
    if (!setjmp(out)) r = sock_sys(SOCK_CONNECT, (u32)fd, (u32)&rq);
    EXPECT(!hung && r == 0 && ncalls[SOCK_CONNECT] == 1 && ncalls[SOCK_GETSOCKOPT] == 1,
           "... restarted it only waits, then asks SO_ERROR (CONNECT sent once)");

    /* poll: the bits, no message */
    calls = 0;
    EXPECT(sock_poll(current->fd[fd]) == POLLOUT && calls == 0, "poll reads the bits without a message");
    /* only the server may use SYS_netsrv */
    EXPECT(netsrv_sys(NETSRV_HELLO, 0, 0) == -EPERM, "SYS_netsrv from another process: -EPERM");
    clear();
    struct unetready u[2] = { { 0x7777, POLLIN }, { 0, 0 } };
    current = &procs[NETD];
    EXPECT(netsrv_sys(NETSRV_READY, (u32)u, 1) == 0 && netsrv_sys(NETSRV_READY, (u32)u, NSOCK + 1) == -EINVAL,
           "NETSRV_READY: an unknown name is ignored, too many items refused");
    struct umsg fake;
    memset(&fake, 0, sizeof fake);
    EXPECT(netsrv_sys(NETSRV_REPLY, (u32)&fake, 0) == -EINVAL, "NETSRV_REPLY to a message not in flight: -EINVAL");
    printf(bad ? "sock-host-test: %d FAILED\n" : "sock-host-test: all ok\n", bad);
    return bad != 0;
}
