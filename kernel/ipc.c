#include "kernel.h"

/* Named ports with synchronous calls. See kernel.h for the model.
 * A client in P_MSG is woken only by the reply (or by its server dying),
 * never by a kill: the server may still be using the client's buffers. */

static int strncmp_(const char *a, const char *b, int n);
static struct port {
    int used, owner;
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

int port_create(const char *name, int owner)
{
    for (int i = 0; i < NPORT; i++)
        if (ports[i].used && !strncmp_(ports[i].name, name, 16))
            return -1;                          /* name taken */
    for (int i = 0; i < NPORT; i++)
        if (!ports[i].used) {
            memset(&ports[i], 0, sizeof ports[i]);
            ports[i].used = 1;
            ports[i].owner = owner;
            for (int k = 0; k < 15 && name[k]; k++)
                ports[i].name[k] = name[k];
            return i;
        }
    return -2;
}

int port_find(const char *name)
{
    for (int i = 0; i < NPORT; i++)
        if (ports[i].used && !strncmp_(ports[i].name, name, 16))
            return i;
    return -1;
}

static void finish(struct umsg *m, s32 result)
{
    m->result = result;
    m->state = MSG_DONE;
    struct proc *c = proc_by_pid(m->from);
    if (c && c->state == P_MSG && c->wait_msg == m)
        c->state = P_READY;
}

long ipc_call(int port, struct umsg *m)
{
    if (port < 0 || port >= NPORT || !ports[port].used)
        return -1;
    struct port *p = &ports[port];
    m->next = 0;
    m->from = current->pid;
    m->server = 0;
    m->state = MSG_QUEUED;
    m->result = 0;
    if (p->tail)
        p->tail->next = m;
    else
        p->head = m;
    p->tail = m;
    current->wait_msg = m;
    current->state = P_MSG;
    if (p->waiter) {                            /* wake the server */
        p->waiter->state = P_READY;
        p->waiter = 0;
    }
    while (m->state != MSG_DONE) {
        current->state = P_MSG;
        schedule();
    }
    current->wait_msg = 0;
    return m->result;
}

struct umsg *ipc_recv(int port, int wait)
{
    if (port < 0 || port >= NPORT || !ports[port].used ||
        ports[port].owner != current->pid)
        return (struct umsg *)-1;
    struct port *p = &ports[port];
    while (!p->head) {
        if (!wait)
            return 0;
        p->waiter = current;
        current->state = P_MSG;
        current->wait_port = port;
        schedule();
        if (!ports[port].used || current->killed)
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

long ipc_reply(struct umsg *m, s32 result)
{
    struct proc *c = proc_by_pid(m ? m->from : 0);
    if (!c || c->state != P_MSG || c->wait_msg != m ||
        m->state != MSG_SERVING || m->server != current->pid)
        return -1;
    finish(m, result);
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
            ports[i].waiter->state = P_READY;
        ports[i].used = 0;
    }
    for (int i = 1; i < NPROC; i++) {
        struct proc *c = &procs[i];
        if (c->state == P_MSG && c->wait_msg && c->wait_msg->state == MSG_SERVING &&
            c->wait_msg->server == pid)
            finish(c->wait_msg, EIO_);
    }
}
