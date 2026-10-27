/* SPDX-License-Identifier: GPL-2.0-only */
#pragma GCC optimize("Os")              /* rare paths (a start, a death): size first */
#include "kernel.h"

/* The service manager (DESIGN D11, docs/d11-service-manager.md). A service is a port name with a program: a built-in
 * image (@diskd, @fsd; @zfsd only in the test kernel kernel-zfs) or a file. A registered name always has its port (a held slot, ipc.c); the first call to
 * it, with no server running, makes the CALLER load the server in its own context (no loader process to deadlock
 * with); the new process takes the port over with port_create. When the server dies its port goes back to held: the
 * request it was serving fails with -ECONNRESET, the queued ones wait for the next server, which the first queued
 * client starts. A start has 30 s to open its port (kept by a watcher in its own context, never by the tick); ends
 * are counted, and the start after the 2nd, 3rd and 4th end in 60 s waits 1, 2 and 4 s.
 * The deadlock rules: nobody loads for someone else; diskd calls nobody; a filesystem server calls only diskd; loads
 * nest at most two deep (a file service read while the root server is stopped). */

enum { SV_FREE, SV_STOPPED, SV_LOADING, SV_STARTING, SV_RUNNING, SV_GIVENUP };
#define START_TICKS  3000u              /* 30 s for a started server to open its port */
#define WINDOW_TICKS 6000u              /* ends are counted in 60 s windows */
#define MAX_ENDS 5                      /* ... and the 5th in one gives the service up */
#define SVC_ARGS 6

static u8 hurry;                        /* the reset warning or reboot began (svc_hurry) */

static struct svc {
    u8 state, essential, served, ends, nwords, depth, midreq;
    s8 nice;
    int pid;                            /* LOADING: the loader; STARTING, RUNNING: the server */
    int watcher;                        /* STARTING: who keeps the deadline (0: nobody yet) */
    u32 deadline, win, starts;
    char prog[SVC_TEXT];                /* the program and its arguments, the words NUL-separated */
    u8 *saved;                          /* the table the server left for its next start (kmalloc'd at its first save) */
    u32 savedlen;
} svcs[NSVC];

extern const char fsd_blob[], fsd_blob_end[], diskd_blob[], diskd_blob_end[];
#ifdef KERNEL_ZFSD
extern const char zfsd_blob[], zfsd_blob_end[];
#endif
static const struct image { char name[6]; u8 priv; const char *s, *e; } images[] = {
    { "diskd", 1, diskd_blob, diskd_blob_end }, { "fsd", 0, fsd_blob, fsd_blob_end },
#ifdef KERNEL_ZFSD
    { "zfsd", 0, zfsd_blob, zfsd_blob_end },
#endif
};
/* The one alias (DESIGN D19 step 1): zfsd left the kernel image and is the file /sbin/zfsd (D11 6.4's file service),
 * but @zfsd is a frozen built-in name an older mount registers. With no built-in zfsd, @zfsd starts that file. Never
 * for an essential entry (disk, fs: the root): the file would be read through the server being started. */
static const char zfsd_file[] = "/sbin/zfsd";

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int differ(const char *a, const char *b, u32 n)
{
    while (n--)
        if (*a++ != *b++)
            return 1;
    return 0;
}

struct proc *svc_proc(int i)
{
    if (i >= NSVC || (svcs[i].state != SV_LOADING && svcs[i].state != SV_STARTING))
        return 0;
    return proc_by_pid(svcs[i].pid);
}

static void wake(int pid)                   /* a loader or watcher asleep in its back-off or deadline */
{
    struct proc *w = pid ? proc_by_pid(pid) : 0;
    if (w && w->state == P_SLEEP)
        ready(w);
}

/* every message queued on held port i fails: its start failed */
static void fail_queue(int i)
{
    struct umsg *m = ports[i].head, *n;
    ports[i].head = ports[i].tail = 0;
    for (; m; m = n) {
        n = m->next;
        ipc_finish(m, -ECONNRESET);
    }
    wake(svcs[i].watcher);
}

/* the entry is stopped and its server is needed again: the first queued client that may start servers becomes the
 * loader (its message leaves the queue: a loader keeps it out while it loads); no-start messages are dropped */
static void pass_load(int i)
{
    struct svc *s = &svcs[i];
    struct port *p = &ports[i];
    s->state = SV_STOPPED;
    s->pid = 0;
    while (p->head) {
        struct umsg *m = p->head;
        struct proc *c = m->client;
        ipc_unqueue(p, m);
        if (c->call_ns) {
            ipc_finish(m, -ECONNRESET);
            continue;
        }
        s->state = SV_LOADING;
        s->pid = c->pid;
        c->svc_role = SVC_ROLE_LOAD;
        ready(c);
        return;
    }
}

static void pass_watch(int i)               /* a starting entry always has a watcher: the first queued client */
{
    struct umsg *m = ports[i].head;
    svcs[i].watcher = 0;
    if (m) {
        svcs[i].watcher = m->client->pid;
        m->client->svc_role = SVC_ROLE_WATCH;
        ready(m->client);
    }
}

/* an end of entry i's server, counted: the 5th within 60 s gives the service up (its port stays held, every call
 * fails at once; for diskd and the root a panic). 1 if given up (its queue failed), else 0 */
static int count_end(int i)
{
    struct svc *s = &svcs[i];
    if (!s->ends || (s32)(ticks - s->win) >= (s32)WINDOW_TICKS) {
        s->win = ticks;
        s->ends = 0;
    }
    if (++s->ends < MAX_ENDS)
        return 0;
    kprintf("svc: %s: given up after %d ends in 60 s\n", ports[i].name, MAX_ENDS);
    if (s->essential)
        panic("svc: an essential server ended 5 times in 60 s");
    s->state = SV_GIVENUP;
    s->pid = 0;
    fail_queue(i);
    return 1;
}

/* start entry i's program (current is the loader): the new pid, or a negative errno */
static int spawn(struct svc *s)
{
    char text[SVC_TEXT], *argv[SVC_ARGS + 1];
    memcpy(text, s->prog, sizeof text);         /* a file read blocks: a new registration may change s meanwhile */
    char *w = text;
    for (int k = 0; k < s->nwords; k++) {
        argv[k] = w;
        while (*w++)
            ;
    }
    argv[s->nwords] = 0;
    int pid = -ENOENT;
    if (text[0] == '@') {
        argv[0] = text + 1;
        u32 k = 0;
        for (; k < sizeof images / sizeof images[0]; k++)
            if (eq(images[k].name, argv[0])) {
                pid = proc_spawn_image(images[k].s, (u32)(images[k].e - images[k].s), argv, s->nwords, images[k].priv, 0);
                break;
            }
        if (k == sizeof images / sizeof images[0] && eq(argv[0], "zfsd")) {
            if (s->essential)
                kprintf("svc: this kernel has no ZenixFS server built in: a ZenixFS root needs the test kernel "
                        "kernel-zfs (DESIGN D19)\n");
            else
                pid = proc_spawn(zfsd_file, argv, s->nwords, 0, 0, 0);
        }
    } else {
        pid = proc_spawn(text, argv, s->nwords, 0, 0, 0);   /* tty 0: output to the kernel log */
    }
    if (pid > 0)
        proc_renice(proc_by_pid(pid), s->nice);
    return pid;
}

/* wait for the start of entry i (current is its watcher), keeping the deadline. m is the caller's message, queued or
 * (a loader's) not yet. 0: m is queued on the running server (or was answered); else the error, m not queued. */
static long watch(int i, struct umsg *m, int queued)
{
    struct svc *s = &svcs[i];
    for (;;) {
        if (s->state == SV_RUNNING)
            break;
        if (s->state != SV_STARTING || s->watcher != current->pid)
            return queued ? 0 : -ECONNRESET;    /* the start failed (a queued m was answered with it) */
        if (current->killed) {                  /* the role goes to the first queued client */
            if (queued)
                ipc_unqueue(&ports[i], m);
            pass_watch(i);
            return -ECONNRESET;
        }
        if ((s32)(ticks - s->deadline) >= 0) {
            struct proc *q = proc_by_pid(s->pid);
            kprintf("svc: %s: %s did not open its port in 30 s\n", ports[i].name, q ? q->name : "?");
            if (q && q->killed != 2) {
                proc_kill(q);
                q->killed = 3;
            }
            if (!count_end(i)) {
                s->state = SV_STOPPED;
                s->pid = 0;
                fail_queue(i);
            }
            return queued ? 0 : -ECONNRESET;
        }
        sleep_until(s->deadline);
        schedule();
        current->wait_msg = m;
    }
    if (!queued) {                              /* the loader's own message goes first */
        current->call_ns = 0;
        ipc_enqueue(&ports[i], m, 1);
    }
    prio_recalc(current);                       /* a loader's boost from the queued clients ends here */
    return 0;
}

/* current loads entry i's server. 0: m is queued on it; else the error (m not queued) */
static long load(int i, struct umsg *m)
{
    struct svc *s = &svcs[i];
    s->state = SV_LOADING;
    s->pid = current->pid;
    if (current->svc_depth >= 2) {
        kprintf("svc: %s: not started: loads nest more than two deep\n", ports[i].name);
        pass_load(i);
        return -ECONNRESET;
    }
    s->depth = ++current->svc_depth;
    u32 back = s->ends >= 2 && (s32)(ticks - s->win) < (s32)WINDOW_TICKS ? 100u << (s->ends - 2) : 0, until = ticks + back;
    while (back && !current->killed && !hurry && (s32)(ticks - until) < 0) {
        sleep_until(until);                     /* the back-off: a kill wakes it */
        schedule();
    }
    int pid = current->killed ? -EINTR : spawn(s);
    current->svc_depth--;
    current->wait_msg = m;
    current->wait_port = PORT_ID(i);
    if (pid == -EINTR) {                        /* killed in its back-off: the next client loads */
        pass_load(i);
        return -ECONNRESET;
    }
    if (pid < 0) {                              /* not counted: no program ran */
        kprintf("svc: %s: cannot start %s (error %d)\n", ports[i].name, s->prog, pid);
        if (s->essential && !s->served)
            panic("svc: cannot start an essential server");
        s->state = SV_STOPPED;
        s->pid = 0;
        fail_queue(i);
        return -ECONNRESET;
    }
    kprintf("svc: %s: started %s (pid %d, for pid %d %s)\n", ports[i].name, s->prog, pid, current->pid, current->name);
    if (current->killed) {
        if (s->state == SV_STARTING && s->watcher == current->pid)
            pass_watch(i);
        return -ECONNRESET;
    }
    return watch(i, m, 0);
}

void svc_hurry(void)
{
    hurry = 1;
    for (int i = 0; i < NSVC; i++)
        if (svcs[i].state == SV_LOADING)
            wake(svcs[i].pid);
}

/* proc_load made process pid for the innermost load of current: from here (before the loader's next blocking call,
 * the close of a program file) the entry is starting, so the new process may take its port at once */
void svc_born(int pid)
{
    for (int i = 0; i < NSVC; i++) {
        struct svc *s = &svcs[i];
        if (s->state == SV_LOADING && s->pid == current->pid && s->depth == current->svc_depth) {
            s->state = SV_STARTING;
            s->pid = pid;
            s->watcher = current->pid;          /* the loader is the first watcher */
            s->deadline = ticks + START_TICKS;
            s->starts++;
            return;
        }
    }
}

long svc_call(int i, struct umsg *m, int ns)
{
    struct svc *s = &svcs[i];
    if (s->state == SV_STARTING && s->pid == current->pid) {
        current->wait_msg = 0;
        kprintf("svc: %s: its own server called it before it opened its port\n", ports[i].name);
        return -ECONNRESET;                     /* it would wait for itself */
    }
    if (s->state == SV_STOPPED && !ns && !disk_frozen) {     /* once disk writes are stopped nothing is started */
        long r = load(i, m);
        if (r)
            return r;
    } else if ((s->state == SV_LOADING || s->state == SV_STARTING) && ns != SVC_NS_SKIP) {
        ipc_enqueue(&ports[i], m, 0);
        if (s->state == SV_STARTING && !s->watcher) {
            s->watcher = current->pid;
            current->svc_role = SVC_ROLE_WATCH;
        }
    } else {
        current->wait_msg = 0;
        return -ECONNRESET;                     /* given up, or a no-start call with nobody serving */
    }
    return ipc_wait(m);
}

long svc_role(struct umsg *m)
{
    int i = current->wait_port & 0xFF, role = current->svc_role;
    current->svc_role = 0;
    if (i >= NSVC)
        return 0;
    return role == SVC_ROLE_LOAD ? load(i, m) : watch(i, m, 1);
}

int svc_handover(int i)
{
    struct svc *s = &svcs[i];
    struct port *p = &ports[i];
    if (s->state != SV_STARTING || s->pid != current->pid || p->op)
        return 0;
    p->owner = current->pid;
    p->op = current;
    s->state = SV_RUNNING;
    s->served = 1;
    vfs_server_started(p->name);                /* D13: its mounts' code segments are not reused */
    if (eq(p->name, "fs"))
        klog_mirror_off();                      /* the root is up: from here the screen belongs to the programs */
    wake(s->watcher);
    u8 b = ipc_best_client(current->pid);      /* the queued clients are its own now */
    if (b < current->prio)
        prio_raise(current, b);
    return 1;
}

/* p is dying (proc_exit, interrupts masked) */
void svc_exit(struct proc *p)
{
    for (int i = 0; i < NSVC; i++) {
        struct svc *s = &svcs[i];
        if (s->state == SV_LOADING && s->pid == p->pid) {   /* a loader that did not get to the end of its call */
            pass_load(i);
            continue;
        }
        if (s->state == SV_STARTING && s->watcher == p->pid)
            pass_watch(i);
        if ((s->state != SV_STARTING && s->state != SV_RUNNING) || s->pid != p->pid)
            continue;
        struct port *pt = &ports[i];
        if (eq(pt->name, "net"))
            sock_server_gone();                 /* D19 rule 10: its sockets die and their waiters wake, now */
        pt->owner = 0;                          /* back to held: the queued messages wait for the next server */
        pt->op = 0;
        pt->waiter = 0;
        struct umsg *in = 0;
        for (int k = 1; k < NPROC; k++) {
            struct proc *c = &procs[k];
            if (c->state == P_MSG && c->wait_msg && c->wait_msg->state == MSG_SERVING && c->wait_msg->server == p->pid)
                in = c->wait_msg;
        }
        int was_running = s->state == SV_RUNNING;
        if (was_running && p->exit_code == 0 && !in) {
            kprintf("svc: %s: %s (pid %d) stopped\n", pt->name, p->name, p->pid);
            s->midreq = 0;                      /* a clean stop: the next start has nothing half done (A8) */
            pass_load(i);
            continue;
        }
        if (in)
            kprintf("svc: %s: %s (pid %d) ended, status %d, during request %u for pid %d\n", pt->name, p->name,
                    p->pid, p->exit_code, in->type, in->from);
        else
            kprintf("svc: %s: %s (pid %d) ended, status %d%s\n", pt->name, p->name, p->pid, p->exit_code,
                    was_running ? "" : ", before it opened its port");
        s->midreq = in != 0;                    /* the next server is told (fsd marks ext2 "needs a check") */
        if (in && p->exit_code == 130)
            kprintf("svc: %s: killed in the middle of a request: the change may be half done\n", pt->name);
        if (s->essential && !s->served)
            panic("svc: an essential server ended before it served");
        if (s->essential && p->exit_code == SVC_EXIT_DAMAGED)
            panic("svc: an essential server's saved table is damaged");   /* no retry can cure it: not counted */
        if (count_end(i))
            continue;
        if (was_running) {
            kprintf("svc: %s: start %u when next used\n", pt->name, s->starts + 1);
            pass_load(i);
        } else {
            s->state = SV_STOPPED;
            s->pid = 0;
            fail_queue(i);                      /* a start that failed once is likely to fail again */
        }
    }
}

static int mine(void)                   /* the entry whose started (or running) server current is, -1 if none */
{
    for (int i = 0; i < NSVC; i++)
        if ((svcs[i].state == SV_STARTING || svcs[i].state == SV_RUNNING) && svcs[i].pid == current->pid)
            return i;
    return -1;
}

int svc_is(const char *name)
{
    int i = mine();
    return i >= 0 && eq(ports[i].name, name);
}

int svc_pid(const char *name)
{
    for (int i = 0; i < NSVC; i++)
        if (svcs[i].state == SV_RUNNING && ports[i].used && eq(ports[i].name, name))
            return svcs[i].pid;
    return 0;
}

/* SVC_SAVE / SVC_LOAD: the small table a server leaves for its next start (diskd's slots and claims, D11 5.4). Only
 * the process the kernel started for an entry; the table is kept while the entry exists. */
long svc_data(u32 op, void *buf, u32 len)
{
    int i = mine();
    if (i < 0)
        return -EPERM;
    struct svc *s = &svcs[i];
    if (op == SVC_OPENS)                        /* after the hand-over, before the first receive (D11 5.4) */
        return s->state == SV_RUNNING ? vfs_opens(ports[i].name, PORT_ID(i), buf, len / 4) : -EPERM;
    if (op == SVC_SAVE) {
        if (len > SVC_SAVED)
            return -E2BIG;
        if (!s->saved && !(s->saved = kmalloc(SVC_SAVED)))
            return -ENOMEM;
        memcpy(s->saved, buf, len);
        s->savedlen = len;
        return 0;
    }
    if (op != SVC_LOAD)
        return -EINVAL;
    if (len > s->savedlen)
        len = s->savedlen;
    if (len)
        memcpy(buf, s->saved, len);
#ifdef SVC_DAMAGE_TEST
    if (len > 8)                                /* a test-only build (tools/svc-test.sh): the table is damaged */
        ((u8 *)buf)[len / 2] ^= 0x5A;
#endif
    return (long)len;
}

/* SVC_REGISTER and SVC_REMOVE (root; strings already copied into the kernel), SVC_SELF */
long svc_sys(u32 op, const char *name, const char *text)
{
    int i = name ? port_slot(name) : -1;
    struct svc *s = i >= 0 && i < NSVC && svcs[i].state != SV_FREE ? &svcs[i] : 0;
    if ((op & 0xFF) == SVC_SELF) {
        i = mine();
        return i < 0 ? 0 : (long)(svcs[i].starts | (svcs[i].midreq ? SVC_MIDREQ : 0));
    }
    if (current->uid != 0)
        return -EPERM;
    if ((op & 0xFF) == SVC_REMOVE) {
        if (!s)
            return -ENOENT;
        if (s->state == SV_LOADING || s->state == SV_STARTING)
            return -EBUSY;
        if (s->state != SV_RUNNING) {          /* a running server keeps the port as an ordinary one until it ends */
            fail_queue(i);
            ports[i].used = 0;
        }
        kfree(s->saved);
        s->saved = 0;
        s->state = SV_FREE;
        return 0;
    }
    if ((op & 0xFF) != SVC_REGISTER)
        return -EINVAL;
    char t[SVC_TEXT];                           /* the words, NUL-separated */
    int n = 0, k = 0, j = 0;
    while (text[j]) {
        while (text[j] == ' ')
            j++;
        if (!text[j])
            break;
        if (n++ == SVC_ARGS)
            return -E2BIG;
        while (text[j] && text[j] != ' ')
            t[k++] = text[j++];
        t[k++] = 0;
    }
    if (!n || (t[0] != '@' && t[0] != '/'))
        return -EINVAL;                         /* a built-in image or an absolute path */
    if (i >= NSVC)
        return -EBUSY;                          /* an ordinary port has that name */
    if (!s) {
        /* a held slot being given back (umount, then mount at once: the old server still has the name until it
         * ends), or none free: wait up to 2 s */
        for (int tries = 0; ; tries++) {
            for (i = 0; i < NSVC && ports[i].used; i++)
                ;
            if ((i < NSVC && port_slot(name) < 0) || tries == 20 || current->killed)
                break;
            sleep_until(ticks + 10);
            schedule();
        }
        if (i == NSVC || port_slot(name) >= 0)
            return -EBUSY;
        u32 gen = (ports[i].gen + 1) & 0x7FFFFF;
        memset(&ports[i], 0, sizeof ports[i]);
        ports[i].gen = gen;
        ports[i].used = 1;
        for (int c = 0; c < 15 && name[c]; c++)
            ports[i].name[c] = name[c];
        s = &svcs[i];
        memset(s, 0, sizeof *s);
        s->state = SV_STOPPED;
        s->essential = eq(name, "disk") || eq(name, "fs");
    }
    if (s->nwords != n || differ(s->prog, t, (u32)k)) {
        s->starts = 0;                          /* a new program: its counters start again, and it has not served */
        s->ends = 0;                            /* (boot's root check swaps -fsck for -root: D17 4) */
        s->served = 0;
    }
    if (s->state == SV_GIVENUP) {               /* registered again: tried again */
        s->state = SV_STOPPED;
        s->ends = 0;
    }
    memset(s->prog, 0, sizeof s->prog);
    memcpy(s->prog, t, (u32)k);
    s->nwords = (u8)n;
    s->nice = (s8)(op >> 8);
    return 0;
}
