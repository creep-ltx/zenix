/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* Named ports with synchronous calls. See kernel.h for the model.
 * A client in P_MSG is woken only by the reply (or by its server dying),
 * never by a kill: the server may still be using the client's buffers. */

/* Slots 0..NSVC-1 are the held ports of the services (D11, svc.c): a registered name keeps its slot and its number
 * across restarts; owner 0 there means "no server running", and a call to it goes to svc_call. */
struct port ports[NPORT];

static int strncmp_(const char *a, const char *b, int n)
{
    while (n-- && (*a || *b)) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        a++;
        b++;
    }
    return 0;
}

/* Reserved port names (audit D): "fs" (the root filesystem), "fs.*" (mounted filesystems, named by mount) and "disk"
 * belong to the system, and "net" (the network server, DESIGN D19: the kernel sends it every socket call): only root may
 * create them, so a normal user cannot put a fake server in their place, also not after the real one died. */
static int reserved(const char *n)
{
    return (n[0] == 'f' && n[1] == 's' && (!n[2] || n[2] == '.')) || !strncmp_(n, "disk", 16) || !strncmp_(n, "net", 16);
}

/* A port number is (generation << 8) | slot. Each create of a slot counts its generation up, so a number someone kept
 * from a server that died (a mount's cached port, fsd's disk port) no longer names whatever port takes the slot next:
 * port_of() refuses it, and a call to it fails as a call to a closed port (audit G1: otherwise a user's new port in
 * that slot received the kernel's filesystem requests and could serve a set-user-id root program). */
static struct port *port_of(int id)
{
    u32 i = (u32)id & 0xFF;
    if (id < 0 || i >= NPORT || !ports[i].used || ports[i].gen != ((u32)id >> 8))
        return 0;
    return &ports[i];
}

int port_slot(const char *name)
{
    for (int i = 0; i < NPORT; i++)
        if (ports[i].used && !strncmp_(ports[i].name, name, 16))
            return i;
    return -1;
}

/* A registered name (D11) is taken over only by the process the kernel started for it, anyone else - root too - gets
 * -EBUSY; "fs", "fs.*" and "disk" not registered yet need root. */
int port_create(const char *name, int owner)
{
    int i = port_slot(name);
    if (i >= 0)
        return i < NSVC && svc_handover(i) ? PORT_ID(i) : -EBUSY;
    if (reserved(name) && current->uid != 0)
        return -13;
    for (i = NSVC; i < NPORT; i++)
        if (!ports[i].used) {
            u32 gen = (ports[i].gen + 1) & 0x7FFFFF;          /* positive port numbers: 23 bits of generation */
            memset(&ports[i], 0, sizeof ports[i]);
            ports[i].gen = gen;
            ports[i].used = 1;
            ports[i].owner = owner;
            ports[i].op = current;
            for (int k = 0; k < 15 && name[k]; k++)
                ports[i].name[k] = name[k];
            return PORT_ID(i);
        }
    return -2;
}

int port_find(const char *name)
{
    int i = port_slot(name);
    return i < 0 ? -1 : PORT_ID(i);
}

/* who works for the port's callers: its server, or for a held port the loader or the starting server (D11) */
struct proc *port_owner(int port)
{
    struct port *p = port_of(port);
    return !p ? 0 : p->op ? p->op : svc_proc((int)(p - ports));
}

/* priority inheritance (DESIGN D10): the best (lowest) level among the clients queued on pid's ports and those whose
 * request pid is serving now; NLEVEL if none */
u8 ipc_best_client(int pid)
{
    u8 best = NLEVEL;
    for (int i = 0; i < NPORT; i++)
        if (ports[i].used && ports[i].owner == pid)
            for (struct umsg *m = ports[i].head; m; m = m->next)
                if (m->client && m->client->prio < best)
                    best = m->client->prio;
    for (int i = 1; i < NPROC; i++) {
        struct proc *c = &procs[i];
        if (c->state == P_MSG && c->wait_msg && c->wait_msg->state == MSG_SERVING && c->wait_msg->server == pid &&
            c->prio < best)
            best = c->prio;
    }
    return best;
}

int ipc_serving(int pid)
{
    for (int i = 1; i < NPROC; i++) {
        struct proc *c = &procs[i];
        if (c->state == P_MSG && c->wait_msg && c->wait_msg->state == MSG_SERVING && c->wait_msg->server == pid)
            return 1;
    }
    return 0;
}

void ipc_finish(struct umsg *m, s32 result)
{
    m->result = result;
    m->state = MSG_DONE;
    struct proc *c = m->client;
    if (c && c->state == P_MSG && c->wait_msg == m)
        ready(c);
}

/* queue m on p (at the front: a loader's own message once its server runs) and wake the server */
void ipc_enqueue(struct port *p, struct umsg *m, int front)
{
    m->state = MSG_QUEUED;
    if (front || !p->head) {
        m->next = p->head;
        p->head = m;
        if (!p->tail)
            p->tail = m;
    } else {
        m->next = 0;
        p->tail->next = m;
        p->tail = m;
    }
    struct proc *o = p->op ? p->op : svc_proc((int)(p - ports));
    if (o && current->prio < o->prio)           /* the server works at least at the caller's level until it replies */
        prio_raise(o, current->prio);
    if (p->waiter) {                            /* wake the server */
        ready(p->waiter);
        p->waiter = 0;
    }
    ev_wake(&p->evw);                           /* and whoever polls the port (D16) */
}

/* SYS_evwait on a port (D16, POLLPORT): POLLIN when a message is queued; only the owner may wait on it */
u16 port_poll(int port)
{
    struct port *p = port_of(port);
    if (!p || p->owner != current->pid)
        return POLLNVAL;
    ev_arm(&p->evw);
    return p->head ? POLLIN : 0;
}

void ipc_unqueue(struct port *p, struct umsg *m)
{
    struct umsg **q = &p->head, *prev = 0;
    while (*q && *q != m) { prev = *q; q = &(*q)->next; }
    if (!*q)
        return;
    *q = m->next;
    if (p->tail == m)
        p->tail = prev;
    m->next = 0;
}

/* wait for the reply to m; a client woken to load or to watch for a held port does that first (svc.c) */
long ipc_wait(struct umsg *m)
{
    while (m->state != MSG_DONE) {
        current->wait_msg = m;                  /* (a load's own calls used it) */
        if (current->svc_role) {
            long r = svc_role(m);
            if (r) {
                current->wait_msg = 0;
                return r;
            }
            continue;
        }
        current->state = P_MSG;
        schedule();
    }
    current->wait_msg = 0;
    current->svc_role = 0;
    return m->result;
}

static long call(int port, struct umsg *m, u16 kflags, int ns)
{
    struct port *p = port_of(port);
    if (!p)
        return -ECONNRESET;                     /* closed or reused (G1): its server is gone (A11: was a bare -1) */
    if (p->owner == current->pid)
        return -EINVAL;                         /* one's own port: that would wait for ever */
    m->next = 0;
    m->from = current->pid;
    m->client = current;
    /* a service's program is read as root (D11): whether it starts never depends on who needed it first */
    m->uid = current->svc_depth ? 0 : current->uid; m->gid = current->svc_depth ? 0 : current->gid;
    m->umask = current->umask;
    m->kflags = kflags;                         /* a user's SYS_call can never claim to be the kernel */
    m->server = 0;
    m->result = 0;
    current->wait_port = port;
    current->wait_msg = m;
    current->call_ns = (u8)(ns ? ns : current->exiting);   /* every call from proc_exit is a no-start call */
    if (!p->op) {                               /* a held port: svc.c starts the server or queues */
        long r = svc_call((int)(p - ports), m, current->call_ns);
        /* every way out of svc_call ends the call: a failed load or watch left wait_msg naming m (in a frame about to
         * die: kill_now and the stop check would refuse for ever) and kept a loader's boost from the queued clients */
        current->wait_msg = 0;
        current->svc_role = 0;
        prio_recalc(current);
        return r;
    }
    ipc_enqueue(p, m, 0);
    return ipc_wait(m);
}

long ipc_call(int port, struct umsg *m) { return call(port, m, 0, 0); }
/* the kernel's requests on behalf of the current process (its ids are stamped as usual): the filesystem servers do
 * inode-number operations (FS_READ, FS_WRITE, FS_CLOSE, ...) only for these, or for root */
long ipc_kcall(int port, struct umsg *m, int ns) { return call(port, m, UMSG_KERNEL, ns); }

/* wait: 0 = do not wait (0 if nothing is queued), 1 = wait for a message, N > 1 = wait at most N-1 ticks (then 0).
 * A timed wait sleeps as P_SLEEP with the port's waiter set, so either a message (ipc_call readies the waiter) or
 * the tick (schedule wakes sleepers) ends it - a server can keep a deadline without polling. */
struct umsg *ipc_recv(int port, int wait)
{
    struct port *p = port_of(port);
    if (!p || p->owner != current->pid || current->killed)
        return (struct umsg *)-1;               /* a kill acts between requests: before the next one is taken */
    u32 until = ticks + (u32)(wait - 1);
    while (!p->head) {
        if (!wait)
            return 0;
        if (wait > 1 && (s32)(ticks - until) >= 0) {
            if (p->waiter == current)
                p->waiter = 0;
            return 0;
        }
        p->waiter = current;
        current->wait_port = port;
        if (wait > 1) {
            sleep_until(until);
        } else {
            current->state = P_MSG;
        }
        schedule();
        if (!port_of(port) || current->killed)
            return (struct umsg *)-1;
        if (!p->head && SIG_DUE(current) && wait == 1) {    /* a handler (D16): -EINTR, restarted with SA_RESTART;
                                                             * a timed receive is never cut (rule 4) */
            if (p->waiter == current)
                p->waiter = 0;
            current->sig_intr = 1;
            return (struct umsg *)-EINTR;
        }
    }
    struct umsg *m = p->head;
    p->head = m->next;
    if (!p->head)
        p->tail = 0;
    m->next = 0;
    m->state = MSG_SERVING;
    m->server = current->pid;
    return m;
}

/* m comes from user space (a server's reply): it is looked up among the requests that are really in flight before
 * anything in it is read, so a stray or forged pointer cannot make the kernel read or write elsewhere */
struct proc *ipc_client(struct umsg *m)
{
    struct proc *c = 0;
    if (m)
        for (int i = 1; i < NPROC; i++)
            if (procs[i].state == P_MSG && procs[i].wait_msg == m) {
                c = &procs[i];
                break;
            }
    if (!c || m->client != c || m->state != MSG_SERVING || m->server != current->pid)
        return 0;
    return c;
}

long ipc_reply(struct umsg *m, s32 result)
{
    if (!ipc_client(m))
        return -EINVAL;
    ipc_finish(m, result);
    if (current->prio < current->blevel)        /* the boost from this client ends with the reply */
        prio_recalc(current);
    return 0;
}

/* a process died: its service ports go back to held (svc.c: their queued messages wait for the next server), its
 * other ports close (waking their clients with -ECONNRESET), and every request it was serving fails */
void ipc_proc_exit(int pid)
{
    svc_exit(current);
    for (int i = 0; i < NPORT; i++) {
        if (!ports[i].used || ports[i].owner != pid)
            continue;
        for (struct umsg *m = ports[i].head, *n; m; m = n) {
            n = m->next;
            ipc_finish(m, -ECONNRESET);
        }
        if (ports[i].waiter)
            ready(ports[i].waiter);
        ev_wake(&ports[i].evw);
        ports[i].used = 0;
    }
    for (int i = 1; i < NPROC; i++) {
        struct proc *c = &procs[i];
        if (c->state == P_MSG && c->wait_msg && c->wait_msg->state == MSG_SERVING &&
            c->wait_msg->server == pid)
            ipc_finish(c->wait_msg, -ECONNRESET);
    }
}

/* reply to m (if any), then wait for the next message: one trap instead of two */
struct umsg *ipc_replyrecv(struct umsg *m, s32 result, int port)
{
    if (m)
        ipc_reply(m, result);
    return ipc_recv(port, 1);
}
