/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nfscd: the NFS client of one mount (DESIGN D23, docs/d23-nfs.md), the server of the service fs.nfsN that mount
 * registers for `mount -t nfs HOST:/EXPORT DIR`:
 *     /sbin/nfscd nfsN HOST:/EXPORT [CODES]
 * This file is its OS layer (net/nfs/nfs.h's callbacks over the port, one TCP socket, evwait and the system calls);
 * the NFS client itself is the core in net/nfs/, shared with the PC tests (tools/host/nfs-test.sh).
 *
 * The start order (4.1, binding): /etc/passwd and /etc/group (the supplementary groups, 3.3), the port, SVC_LOAD (the
 * saved root, 4.1), SVC_OPENS (the numbers the kernel holds after a restart, 4.4), and only then sockets: the first
 * socket call may start netd (a load in this process, the one wait of the loop, bounded by D11's 30 s).
 *
 * The loop (4.3): one evwait on the port (POLLPORT | POLLPRI, and POLLIN while a request can be held), the socket
 * (POLLIN; POLLOUT while bytes wait or a connect is under way) and a name lookup's socket, with the core's next timer
 * as the timeout. Messages are held here by id (the core never sees a message): a request the core has no slot for
 * waits in a small queue of its own (decision (e) of the main session: messages stay with nfscd, FS_CLOSE and a
 * cancelled message go to the core at once). POLLPRI (D23 5.2): every held message newly marked UMSG_CANCEL is told to
 * the core (nfs_cancel). The link to a message is cut at its reply. Killed (evwait answers -EINTR): nfs_kill answers
 * everything -EIO, then the process ends.
 *
 * Only root and the kernel may send requests (4.3; the core checks UMSG_KERNEL or uid 0).
 */
#include "libz.h"
#include "socket.h"
#include "../kernel/fsproto.h"
#include "../net/nfs/nfs.h"

typedef unsigned char u8;
typedef unsigned short u16;

#define NHOLD 40                                 /* the large profile's 32 slots and a queue of 8 */
#define NWAIT 8                                  /* messages received while the core had no free slot */
#define CAP_SMALL (160u << 10)                   /* the heap's cap: small profile (D23 9.2: ~150 KB at most) */
#define LARGE_FAST (2u << 20)                    /* netd's rule (D19 7.3): the large profile with 2 MB of fast RAM free */

static const char *dev = "nfs";
static char pname[16];
static int port = -1;

/* ---- the OS layer for the core ---- */
uint32_t nfs_now(void) { return (uint32_t)sys_ticks() * 10u; }

void nfs_log(const char *text)
{
    char b[200];
    u32 n = strlen(text);
    if (n > sizeof b - 1) n = sizeof b - 1;
    memcpy(b, text, n);
    b[n++] = '\n';
    sys_write(2, b, n);                          /* one write: a whole line in the kernel log (D11 step 8) */
}
static void say(const char *a, const char *b)
{
    char t[160];
    u32 n = 0;
    for (const char *q = dev; *q && n < 16; ) t[n++] = *q++;
    t[n++] = ':'; t[n++] = ' ';
    for (const char *q = a; *q && n < 100; ) t[n++] = *q++;
    for (const char *q = b ? b : ""; *q && n < sizeof t - 1; ) t[n++] = *q++;
    t[n] = 0;
    nfs_log(t);
}

static u32 heap_used, heap_cap;
void *nfs_alloc(uint32_t n)
{
    if (heap_used + n > heap_cap) return 0;
    void *p = rheap_alloc(n);
    if (p) heap_used += rheap_size(p);
    return p;
}
void nfs_free(void *q)
{
    heap_used -= rheap_size(q);
    rheap_free(q);
}

void nfs_save(const void *buf, uint32_t n) { sys_svc_save(buf, n); }

/* the messages nfscd holds: id -> message (the core knows only the id) */
static struct held { u32 id; struct umsg *m; u8 given, told; } hold[NHOLD];
static u32 next_id = 1;
static int nheld(void) { int n = 0; for (int i = 0; i < NHOLD; i++) if (hold[i].m) n++; return n; }

void nfs_answer(uint32_t id, long result)
{
    for (int i = 0; i < NHOLD; i++)
        if (hold[i].m && hold[i].id == id) {
            struct umsg *m = hold[i].m;
            hold[i].m = 0;                       /* the link is cut before the reply (4.3) */
            sys_reply(m, (int)result);
            return;
        }
}

/* ---- the connection ---- */
static char host[64];
static int sock = -1, conn_wait, conn_done, conn_err, want_port, srcport;
static int rsock = -1;                           /* a name lookup under way (SOCK_RESOLVE, D19 4.5) */
static u32 addr;                                 /* the NAS's address (network order: the machine's) */

static void sock_close(int *s) { if (*s >= 0) { sys_close(*s); *s = -1; } }

static void tell_conn(int err)
{
    conn_done = 1;
    conn_err = err;
    if (err < 0) {                               /* the core says "no answer on port N"; the reason is here */
        char t[48];
        sprintf(t, "connect to port %d: ", want_port);
        say(t, zstrerror(err) ? zstrerror(err) : "failed");
    }
}

/* the same privileged source port on every reconnect (3.2, 3.4: the NAS's duplicate request cache matches it):
 * every connection closes by abort (SO_LINGER {1, 0}: a reset, no FIN_WAIT or TIME_WAIT), so its port is free again
 * at once; another port only when that one is really taken (by another program) */
static int bind_connect(void)
{
    struct sockaddr_in a;
    struct linger lg = { 1, 0 };
    int on = 1;
    sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0) return -zerrno;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)srcport);
    if (bind(sock, (struct sockaddr *)&a, sizeof a) < 0) return -zerrno;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)want_port);
    a.sin_addr.s_addr = addr;
    if (connect(sock, (struct sockaddr *)&a, sizeof a) == 0) return 0;
    return zerrno == EINPROGRESS ? 1 : -zerrno;
}
static void do_connect(void)
{
    for (int k = 0; k < 359; k++) {
        int r = bind_connect();
        if (r == 1) { conn_wait = 1; return; }
        if (r == 0) { tell_conn(0); return; }
        sock_close(&sock);
        if (r != -EADDRINUSE) { tell_conn(r); return; }
        srcport = srcport >= 1023 ? 665 : srcport + 1;
    }
    tell_conn(-EADDRINUSE);
}

/* the host as registered: a dotted address, or a name looked up at each connect (4.1: a NAS whose address changed is
 * still found; an fstab line works before the network is up) */
int nfs_connect(int p)
{
    sock_close(&sock);
    sock_close(&rsock);
    conn_wait = conn_done = 0;
    want_port = p;
    struct in_addr in;
    if (inet_aton(host, &in)) { addr = in.s_addr; do_connect(); return 0; }
    rsock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    struct usockreq r;
    memset(&r, 0, sizeof r);
    r.buf = (u32)host;
    r.len = strlen(host);
    if (rsock < 0 || sys_sock(SOCK_RESOLVE, rsock, &r) < 0) {
        sock_close(&rsock);
        tell_conn(-EHOSTUNREACH);
    }
    return 0;                                    /* the answer comes through the loop, never from in here */
}

void nfs_disconnect(void)
{
    sock_close(&sock);                           /* an abort: SO_LINGER {1, 0} (bind_connect) */
    sock_close(&rsock);
    conn_wait = conn_done = 0;
}

long nfs_send(const void *buf, long n)
{
    if (sock < 0 || conn_wait) return 0;
    int k = send(sock, buf, (u32)n, 0);
    if (k < 0) return zerrno == EAGAIN ? 0 : -ECONNRESET;
    return k;
}

/* ---- the users' groups (3.3): read at the start; a gpasswd change counts from the next mount ---- */
#define NUSERS 32
static struct ug { u16 uid, n; u16 g[NFS_NGROUPS]; } ug[NUSERS];
static int nug;

static char *slurp(const char *path)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct ustat st;
    char *b = 0;
    if (!sys_fstat(fd, &st) && st.size < 32768 && (b = rheap_alloc(st.size + 1))) {
        int n = sys_read(fd, b, st.size);
        b[n > 0 ? n : 0] = 0;
    }
    sys_close(fd);
    return b;
}
static char *field(char **p)                     /* the next ':'-separated field of a line, in place */
{
    char *s = *p, *e = s;
    while (*e && *e != ':' && *e != '\n') e++;
    *p = *e == ':' ? e + 1 : e;
    if (*e == ':') *e = 0;
    return s;
}
static int uid_of(const char *pw, const char *name, u32 nlen)
{
    for (const char *l = pw; *l; ) {
        const char *c = l;
        while (*c && *c != ':' && *c != '\n') c++;
        if ((u32)(c - l) == nlen && !memcmp(l, name, nlen) && *c == ':') {
            c++; while (*c && *c != ':' && *c != '\n') c++;         /* the hash */
            if (*c == ':' && c[1] >= '0' && c[1] <= '9') return atoi(c + 1);
        }
        while (*l && *l != '\n') l++;
        if (*l) l++;
    }
    return -1;
}
static void read_groups(void)
{
    char *pw = slurp("/etc/passwd"), *gr = pw ? slurp("/etc/group") : 0;
    for (char *l = gr; l && *l; ) {
        char *end = l;
        while (*end && *end != '\n') end++;
        char save = *end; *end = 0;
        char *p = l;
        field(&p); field(&p);
        char *gs = field(&p), *mem = p;
        u32 gid = (u32)atoi(gs);
        while (*gs >= '0' && *gs <= '9' && *mem) {
            char *e = mem;
            while (*e && *e != ',') e++;
            int uid = uid_of(pw, mem, (u32)(e - mem));
            if (uid >= 0) {
                int k = 0;
                while (k < nug && ug[k].uid != uid) k++;
                if (k == nug && nug < NUSERS) { ug[nug].uid = (u16)uid; ug[nug].n = 0; nug++; }
                if (k < nug && ug[k].n < NFS_NGROUPS) ug[k].g[ug[k].n++] = (u16)gid;
            }
            mem = *e ? e + 1 : e;
        }
        *end = save;
        l = *end ? end + 1 : end;
    }
    rheap_free(pw);
    rheap_free(gr);
}

/* ---- requests ---- */
static void give(struct held *h)
{
    struct umsg *m = h->m;
    struct nfs_req r;
    memset(&r, 0, sizeof r);
    r.id = h->id;
    r.type = m->type;
    for (int i = 0; i < 4; i++) r.a[i] = m->a[i];
    r.uid = m->uid; r.gid = m->gid; r.umask = m->umask; r.kflags = m->kflags;
    if (m->kflags & UMSG_GROUPS) {               /* the caller's own supplementary groups, as the kernel holds them */
        const struct umsg_groups *g = UMSG_GROUPS_AT(m);
        r.ngroups = g->n < NGROUPS ? g->n : NGROUPS;
        for (u32 i = 0; i < r.ngroups; i++) r.groups[i] = g->g[i];
    } else {                                     /* a root program's own request: the user's groups from /etc/group */
        for (int k = 0; k < nug; k++)
            if (ug[k].uid == m->uid) {
                r.ngroups = ug[k].n;
                for (int i = 0; i < ug[k].n; i++) r.groups[i] = ug[k].g[i];
            }
    }
    h->given = 1;
    h->told = (m->kflags & UMSG_CANCEL) != 0;
    nfs_request(&r);
}

static void take_messages(void)
{
    for (int k = 0; k < 8; k++) {                /* at most 8 a turn, as netd */
        int i = 0;
        while (i < NHOLD && hold[i].m) i++;
        if (i == NHOLD) return;
        struct umsg *m = sys_recv(port, 0);
        if (!m || (int)m == -1 || (int)m < 0) return;
        hold[i].m = m;
        if (!next_id) next_id = 1;
        hold[i].id = next_id++;
        hold[i].given = 0;
        uint32_t t;
        if (m->type == FS_CLOSE || (m->kflags & UMSG_CANCEL) || (nfs_poll(&t) & NFS_P_TAKE)) give(&hold[i]);
    }
}

static void feed_waiting(void)                   /* the oldest first */
{
    uint32_t t;
    for (;;) {
        struct held *o = 0;
        for (int i = 0; i < NHOLD; i++)
            if (hold[i].m && !hold[i].given && (!o || (int)(hold[i].id - o->id) < 0)) o = &hold[i];
        if (!o || !(nfs_poll(&t) & NFS_P_TAKE)) return;
        give(o);
    }
}

static void cancels(void)                        /* POLLPRI (5.2): the held messages newly marked */
{
    for (int i = 0; i < NHOLD; i++) {
        struct held *h = &hold[i];
        if (!h->m || !(h->m->kflags & UMSG_CANCEL)) continue;
        if (!h->given) give(h);                  /* waiting for a slot: the core answers -EINTR at once */
        else if (!h->told) { h->told = 1; nfs_cancel(h->id); }
    }
}

static u32 seed(void)
{
    u32 s = sys_hrtime() ^ ((u32)sys_getpid() << 16) ^ (u32)sys_ticks() * 2654435761u;
    int fd = sys_open("/dev/random", O_RDONLY | O_NONBLOCK);
    u32 r = 0;
    if (fd >= 0) {
        if (sys_read(fd, &r, 4) == 4) s ^= r;
        sys_close(fd);
    }
    return s;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) { dprintf(1, "usage: /sbin/nfscd nfsN HOST:/EXPORT [CODES] (started by mount -t nfs)\n"); return 2; }
    dev = argv[1];
    const char *src = argv[2], *colon = strchr(src, ':'), *codes = argc > 3 ? argv[3] : 0;
    if (!colon || colon[1] != '/' || colon == src || (u32)(colon - src) >= sizeof host) {
        say("not HOST:/EXPORT: ", src);
        return 2;
    }
    memcpy(host, src, (u32)(colon - src));
    struct nfs_opts o;
    if (nfs_codes_parse(codes, &o)) { say("unknown option codes: ", codes); return 2; }
    strcpy(pname, "fs."); strncpy(pname + 3, dev, 12); pname[15] = 0;
    read_groups();
    port = sys_port_create(pname);
    if (port < 0) { say("cannot create the port ", pname); return 1; }
    static u8 saved[NFS_SAVEMAX];
    static uint32_t opens[160];
    int ns = sys_svc_load(saved, sizeof saved), no = sys_svc_opens((u32 *)opens, sizeof opens);
    u32 mi[4];
    struct nfs_conf c;
    memset(&c, 0, sizeof c);
    c.dev = dev;
    c.export_path = colon + 1;
    c.codes = codes;
    c.large = sys_meminfo(mi) >= 0 && mi[1] >= LARGE_FAST;
    if (c.large) {
        c.cache_bytes = mi[1] / 8 < (1u << 20) ? mi[1] / 8 : 1u << 20;
        heap_cap = c.cache_bytes + (1u << 20);
    } else heap_cap = CAP_SMALL;
    c.seed = seed();
    c.saved = ns > 0 ? saved : 0;
    c.nsaved = ns > 0 ? (u32)ns : 0;
    c.opens = opens;
    c.nopens = no > 0 ? no : 0;
    srcport = 665 + (int)(c.seed % 359);
    int e = nfs_init(&c);
    if (e) { say(e == -ENOMEM ? "out of memory" : "cannot start: bad export or options", 0); return 1; }
    {
        char t[120];
        strcpy(t, "serving "); strcat(t, src);          /* SVC_TEXT keeps it under 80 */
        say(t, c.large ? " (large profile)" : " (small profile)");
    }
    static u8 buf[2048];
    for (;;) {
        uint32_t tmo;
        int b = nfs_poll(&tmo);
        if (b & NFS_P_DONE) break;
        struct pollfd pf[3];
        int n = 0, ip = -1, is = -1, ir = -1;
        ip = n; pf[n].fd = port; pf[n].events = POLLPORT | POLLPRI | (nheld() < NHOLD ? POLLIN : 0); pf[n++].revents = 0;
        if (sock >= 0) {
            is = n; pf[n].fd = sock;
            pf[n].events = conn_wait ? POLLOUT : (short)(POLLIN | ((b & NFS_P_OUT) ? POLLOUT : 0));
            pf[n++].revents = 0;
        }
        if (rsock >= 0) { ir = n; pf[n].fd = rsock; pf[n].events = POLLIN; pf[n++].revents = 0; }
        int ticks = conn_done ? 0 : tmo == ~0u ? -1 : (int)((tmo + 9) / 10);
        int r = sys_evwait(pf, (u32)n, ticks);
        if (r == -EINTR) {                       /* killed (nfscd catches no signal) */
            nfs_kill();
            return 1;
        }
        if (ir >= 0 && pf[ir].revents) {         /* the name's answer: up to 4 addresses */
            u32 a[4];
            int k = recv(rsock, a, sizeof a, 0);
            sock_close(&rsock);
            if (k >= 4) { addr = a[0]; do_connect(); }
            else { say("cannot find the address of ", host); tell_conn(-EHOSTUNREACH); }
        }
        if (is >= 0 && sock >= 0 && pf[is].revents) {
            if (conn_wait) {
                int err = 0;
                socklen_t l = sizeof err;
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &l);
                conn_wait = 0;
                if (err) sock_close(&sock);
                if (err == EADDRINUSE || err == -EADDRINUSE) {      /* the source port taken: the next */
                    srcport = srcport >= 1023 ? 665 : srcport + 1;
                    do_connect();
                } else tell_conn(err > 0 ? -err : err);
            } else if (pf[is].revents & (POLLIN | POLLHUP | POLLERR)) {
                int k = recv(sock, buf, sizeof buf, 0);
                if (k > 0) nfs_input(buf, k);
                else if (k == 0) { sock_close(&sock); nfs_input(0, 0); }
                else if (zerrno != EAGAIN) { sock_close(&sock); nfs_input(0, -ECONNRESET); }
            }
        }
        if (conn_done) { conn_done = 0; nfs_connected(conn_err); }
        if (pf[ip].revents & POLLPRI) cancels();
        if (pf[ip].revents & POLLIN) take_messages();
        feed_waiting();
        nfs_tick();
    }
    return 0;
}
