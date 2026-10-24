/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* Named ports with synchronous calls. See kernel.h for the model.
 * A client in P_MSG is woken only by the reply (or by its server dying),
 * never by a kill: the server may still be using the client's buffers. */

static int strncmp_(const char *a, const char *b, int n);
static struct port {
    int used, owner;
    u32 gen;                    /* how many times this slot was created: part of the port number (audit G1) */
    struct proc *op;            /* the owner's slot (valid while used: a dying owner's ports are closed) */
    char name[16];
    struct umsg *head, *tail;
    struct proc *waiter;        /* server blocked in recv */
} ports[NPORT];

#define EIO_ (-5)

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
 * belong to the system: only root may create them, so a normal user cannot put a fake server in their place, also
 * not after the real one died. */
static int reserved(const char *n)
{
    return (n[0] == 'f' && n[1] == 's' && (!n[2] || n[2] == '.')) || !strncmp_(n, "disk", 16);
}

/* A port number is (generation << 8) | slot. Each create of a slot counts its generation up, so a number someone kept
 * from a server that died (a mount's cached port, fsd's disk port) no longer names whatever port takes the slot next:
 * port_of() refuses it, and a call to it fails as a call to a closed port (audit G1: otherwise a user's new port in
 * that slot received the kernel's filesystem requests and could serve a set-user-id root program). */
#define PORT_ID(i) ((int)((ports[i].gen << 8) | (u32)(i)))
static struct port *port_of(int id)
{
    u32 i = (u32)id & 0xFF;
    if (id < 0 || i >= NPORT || !ports[i].used || ports[i].gen != ((u32)id >> 8))
        return 0;
    return &ports[i];
}

int port_create(const char *name, int owner)
{
    if (reserved(name) && current->uid != 0)
        return -13;
    for (int i = 0; i < NPORT; i++)
        if (ports[i].used && !strncmp_(ports[i].name, name, 16))
            return -1;                          /* name taken */
    for (int i = 0; i < NPORT; i++)
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
    for (int i = 0; i < NPORT; i++)
        if (ports[i].used && !strncmp_(ports[i].name, name, 16))
            return PORT_ID(i);
    return -1;
}

struct proc *port_owner(int port)
{
    struct port *p = port_of(port);
    return p ? p->op : 0;
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

static void finish(struct umsg *m, s32 result)
{
    m->result = result;
    m->state = MSG_DONE;
    struct proc *c = m->client;
    if (c && c->state == P_MSG && c->wait_msg == m)
        ready(c);
}

static long call(int port, struct umsg *m, u16 kflags)
{
    struct port *p = port_of(port);
    if (!p || p->owner == current->pid)
        return -1;                              /* closed, reused, or one's own port (that would wait for ever) */
    m->next = 0;
    m->from = current->pid;
    m->client = current;
    m->uid = current->uid; m->gid = current->gid; m->umask = current->umask;
    m->kflags = kflags;                         /* a user's SYS_call can never claim to be the kernel */
    m->server = 0;
    m->state = MSG_QUEUED;
    m->result = 0;
    if (p->tail)
        p->tail->next = m;
    else
        p->head = m;
    p->tail = m;
    current->wait_msg = m;
    current->wait_port = port;
    current->state = P_MSG;
    if (current->prio < p->op->prio)            /* the server works at least at the caller's level until it replies */
        prio_raise(p->op, current->prio);
    if (p->waiter) {                            /* wake the server */
        ready(p->waiter);
        p->waiter = 0;
    }
    while (m->state != MSG_DONE) {
        current->state = P_MSG;
        schedule();
    }
    current->wait_msg = 0;
    return m->result;
}

long ipc_call(int port, struct umsg *m) { return call(port, m, 0); }
/* the kernel's requests on behalf of the current process (its ids are stamped as usual): the filesystem servers do
 * inode-number operations (FS_READ, FS_WRITE, FS_CLOSE, ...) only for these, or for root */
long ipc_kcall(int port, struct umsg *m) { return call(port, m, UMSG_KERNEL); }

/* wait: 0 = do not wait (0 if nothing is queued), 1 = wait for a message, N > 1 = wait at most N-1 ticks (then 0).
 * A timed wait sleeps as P_SLEEP with the port's waiter set, so either a message (ipc_call readies the waiter) or
 * the tick (schedule wakes sleepers) ends it - a server can keep a deadline without polling. */
struct umsg *ipc_recv(int port, int wait)
{
    struct port *p = port_of(port);
    if (!p || p->owner != current->pid)
        return (struct umsg *)-1;
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
long ipc_reply(struct umsg *m, s32 result)
{
    struct proc *c = 0;
    if (m)
        for (int i = 1; i < NPROC; i++)
            if (procs[i].state == P_MSG && procs[i].wait_msg == m) {
                c = &procs[i];
                break;
            }
    if (!c || m->client != c || m->state != MSG_SERVING || m->server != current->pid)
        return -EINVAL;
    finish(m, result);
    if (current->prio < current->blevel)        /* the boost from this client ends with the reply */
        prio_recalc(current);
    return 0;
}

/* a process died: close its ports (waking clients with an error) and fail
 * every request it was serving */
void ipc_proc_exit(int pid)
{
    for (int i = 0; i < NPORT; i++) {
        if (!ports[i].used || ports[i].owner != pid)
            continue;
        for (struct umsg *m = ports[i].head, *n; m; m = n) {
            n = m->next;
            finish(m, EIO_);
        }
        if (ports[i].waiter)
            ready(ports[i].waiter);
        ports[i].used = 0;
    }
    for (int i = 1; i < NPROC; i++) {
        struct proc *c = &procs[i];
        if (c->state == P_MSG && c->wait_msg && c->wait_msg->state == MSG_SERVING &&
            c->wait_msg->server == pid)
            finish(c->wait_msg, EIO_);
    }
}

/* reply to m (if any), then wait for the next message: one trap instead of two */
struct umsg *ipc_replyrecv(struct umsg *m, s32 result, int port)
{
    if (m)
        ipc_reply(m, result);
    return ipc_recv(port, 1);
}
