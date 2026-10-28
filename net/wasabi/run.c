/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid's commands and processes (DESIGN D24 4, 5.1, 5.5, 7): RUN and its two slots, detached commands, FREE, SLOTS,
 * PS, KILL, REBOOT and its guard, RESTART and QUIT. */
#include "wasabi_int.h"

#define SIGINT  2
#define SIGKILL 9
#define WHO_SLOT (-10)                          /* poll's who: -10 - (slot * 2 + 0 out / 1 err) */
#define WHO_REBOOT (-30)

int runs_busy(void)
{
    int n = 0;
    for (int i = 0; i < WSB_SLOTS; i++)
        if (g.s[i].pid)
            n++;
    return n;
}

static int spawn_sh(const char *cmd, int in, int out, int err)
{
    const char *argv[] = { "/bin/sh", "-c", cmd, 0 };
    return wsb_os_spawn(argv, in, out, err);
}

/* SIGs to the command's process group and to every descendant found by ppid (5.1) */
void tree_kill(int pid, int sig)
{
    int set[16], n = 1;
    struct wsb_proc p;
    set[0] = pid;
    wsb_os_kill(-pid, sig);
    for (int pass = 0; pass < 4; pass++)
        for (unsigned i = 0; n < 16; i++) {
            int r = wsb_os_proc(i, &p), have = 0, parent = 0;
            if (r < 0)
                break;
            if (r)
                continue;
            for (int j = 0; j < n; j++) {
                if (set[j] == (int)p.pid) have = 1;
                if (set[j] == (int)p.ppid) parent = 1;
            }
            if (parent && !have && (int)p.pid != g.self)
                set[n++] = (int)p.pid;
        }
    for (int j = 0; j < n; j++)
        wsb_os_kill(set[j], sig);
}

static void slot_close(struct slot *s)
{
    if (s->out >= 0) { wsb_os_close(s->out); fd_give(1); s->out = -1; }
    if (s->err >= 0) { wsb_os_close(s->err); fd_give(1); s->err = -1; }
}

static void copycmd(char *d, const char *s)
{
    uint32_t n = 0;
    while (s[n] && n < 47) {
        d[n] = s[n] == '\n' || s[n] == '\t' ? ' ' : s[n];
        n++;
    }
    d[n] = 0;
}

static int kid_add(int pid, int stuck, int num, uint32_t started, const char *cmd)
{
    for (int i = 0; i < WSB_BG; i++)
        if (!g.k[i].pid) {
            g.k[i].pid = pid;
            g.k[i].stuck = stuck;
            g.k[i].num = num;
            g.k[i].started = started;
            copycmd(g.k[i].cmd, cmd);
            return 0;
        }
    return -1;
}
static int kids_free(void)
{
    int n = 0;
    for (int i = 0; i < WSB_BG; i++)
        if (!g.k[i].pid)
            n++;
    return n;
}

static void run_detached(struct conn *c, const char *cmd)
{
    char name[32], note[96];
    int fd = -EEXIST, pid;
    if (!kids_free()) {
        qerr(c, 0, 0, "too many background commands");
        return;
    }
    if (fd_take(1)) {
        qerr(c, 0, 0, "busy: all descriptors in use");
        return;
    }
    for (int t = 0; t < 50 && fd == -EEXIST; t++) {
        struct wsb_buf b = { name, 0, sizeof name };
        bs(&b, "/tmp/wasabi-bg-");
        bu(&b, ++g.bgseq);
        fd = wsb_os_open(name, WSB_O_WRITE, 0644);
    }
    if (fd < 0) {
        fd_give(1);
        qerr(c, fd, 1, "cannot make the output file in /tmp");
        return;
    }
    pid = spawn_sh(cmd, g.nullfd, fd, fd);
    wsb_os_close(fd);
    fd_give(1);
    if (pid < 0) {
        wsb_os_unlink(name);
        qerr(c, pid, 0, "could not start /bin/sh");
        return;
    }
    kid_add(pid, 0, 0, g.now, cmd);
    struct wsb_buf b = { note, 0, sizeof note };
    bs(&b, "[started in the background; its output goes to ");
    bs(&b, name);
    bs(&b, "]\n");
    uint8_t ex[8] = { 0 };
    q(c, WT_STDERR, note, b.n);
    q(c, WT_EXIT, ex, 8);
}

void cmd_run(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0, flags;
    char *cmd;
    int i, np, pid, a[2] = { -1, -1 }, b[2] = { -1, -1 };
    if (wsb_get32(p, len, &off, &flags)) {
        qerr(c, 0, 0, "bad RUN header");
        return;
    }
    cmd = wsb_os_alloc(WSB_SMALL);
    if (!cmd) {
        qerr(c, -ENOMEM, 0, "out of memory");
        return;
    }
    if (wsb_getstr(p, len, &off, cmd, WSB_SMALL)) {
        wsb_os_free(cmd);
        qerr(c, 0, 0, "bad RUN header");
        return;
    }
    if (flags & WR_DETACH) {
        run_detached(c, cmd);
        wsb_os_free(cmd);
        return;
    }
    for (i = 0; i < WSB_SLOTS && g.s[i].pid; i++)
        ;
    if (i == WSB_SLOTS) {
        wsb_os_free(cmd);
        qerr(c, 0, 0, "all run slots are busy");
        return;
    }
    np = flags & WR_MERGE ? 1 : 2;
    if (fd_take(2 * np)) {
        wsb_os_free(cmd);
        qerr(c, 0, 0, "busy: all descriptors in use");
        return;
    }
    int e = wsb_os_pipe(a);
    if (!e && np == 2)
        e = wsb_os_pipe(b);
    if (e) {
        if (a[0] >= 0) { wsb_os_close(a[0]); wsb_os_close(a[1]); }
        fd_give(2 * np);
        wsb_os_free(cmd);
        qerr(c, e, 0, "no pipe for the command");
        return;
    }
    pid = spawn_sh(cmd, g.nullfd, a[1], np == 2 ? b[1] : a[1]);
    wsb_os_close(a[1]);
    if (np == 2)
        wsb_os_close(b[1]);
    fd_give(np);
    struct slot *s = &g.s[i];
    if (pid < 0) {
        wsb_os_close(a[0]);
        if (np == 2)
            wsb_os_close(b[0]);
        fd_give(np);
        wsb_os_free(cmd);
        qerr(c, pid, 0, "could not start /bin/sh");
        return;
    }
    memset(s, 0, sizeof *s);
    s->pid = pid;
    s->out = a[0];
    s->err = np == 2 ? b[0] : -1;
    s->conn = (int)(c - g.c);
    s->freer = -1;
    s->started = g.now;
    copycmd(s->cmd, cmd);
    wsb_os_free(cmd);
    c->job = J_RUN;
    c->slot = i;
    if (flags & WR_SLOT) {
        uint8_t n4[4];
        put32(n4, (uint32_t)i + 1);
        q(c, WT_SLOT, n4, 4);
    }
}

/* one read of a slot's pipe into its client's queue (or away): bytes, 0 its end (closed), -EAGAIN */
static long slot_read(struct slot *s, int which)
{
    static uint8_t sink[512];
    int *fd = which ? &s->err : &s->out;
    struct conn *c = s->conn >= 0 ? &g.c[s->conn] : 0;
    uint8_t *d;
    uint32_t room;
    long n;
    if (*fd < 0)
        return 0;
    if (c) {
        room = qroom(c);
        if (room > WSB_SMALL)
            room = WSB_SMALL;
        if (room < 64)
            return -EAGAIN;
        d = qres(c, room);
        n = wsb_os_read(*fd, d, (long)room);
        if (n > 0)
            qcommit(c, which ? WT_STDERR : WT_STDOUT, (uint32_t)n);
    } else
        n = wsb_os_read(*fd, sink, sizeof sink);
    if (n == 0 || (n < 0 && n != -EAGAIN)) {
        wsb_os_close(*fd);
        fd_give(1);
        *fd = -1;
        return 0;
    }
    return n;
}

int run_poll(struct wsb_pollfd *f, int *who, int n, uint32_t *tmo)
{
    for (int i = 0; i < WSB_SLOTS; i++) {
        struct slot *s = &g.s[i];
        if (!s->pid || s->ended)
            continue;
        if (s->conn >= 0 && qroom(&g.c[s->conn]) < 64)
            continue;                           /* back-pressure: the client reads first */
        for (int w = 0; w < 2; w++) {
            int fd = w ? s->err : s->out;
            if (fd < 0)
                continue;
            f[n].fd = fd;
            f[n].events = WSB_POLLIN;
            who[n++] = WHO_SLOT - (i * 2 + w);
        }
    }
    if (g.rebootout >= 0) {
        f[n].fd = g.rebootout;
        f[n].events = WSB_POLLIN;
        who[n++] = WHO_REBOOT;
    }
    (void)tmo;
    return n;
}

static void reboot_read(void)
{
    char t[64];
    long n = wsb_os_read(g.rebootout, t, sizeof t);
    if (n <= 0 && n != -EAGAIN) {
        wsb_os_close(g.rebootout);
        fd_give(1);
        g.rebootout = -1;
        return;
    }
    for (long i = 0; i < n && g.rtn + 1 < sizeof g.rtext; i++)
        g.rtext[g.rtn++] = t[i] == '\n' ? ' ' : t[i];
    g.rtext[g.rtn] = 0;
}

void run_ready(int k, int who, short rev)
{
    (void)k;
    (void)rev;
    if (who == WHO_REBOOT) {
        if (g.rebootout >= 0)
            reboot_read();
        return;
    }
    int x = WHO_SLOT - who, i = x / 2;
    if (i < 0 || i >= WSB_SLOTS || !g.s[i].pid)
        return;
    slot_read(&g.s[i], x & 1);
}

static void answer_free(int ci, const char *what)
{
    struct conn *c;
    if (ci < 0)
        return;
    c = &g.c[ci];
    if (c->fd < 0 || c->job != J_FREE)
        return;
    q(c, WT_DATA, what, (uint32_t)strlen(what));
    qend(c);
    conn_jobdone(c);
}

/* the slot's command has ended and its pipes are dry (or are given up on): EXIT, the slot is free */
static int slot_finish(struct slot *s)
{
    struct conn *c = s->conn >= 0 ? &g.c[s->conn] : 0;
    if (c) {
        uint8_t ex[8];
        put32(ex, (uint32_t)s->status);
        put32(ex + 4, 0);
        if (q(c, WT_EXIT, ex, 8))
            return -1;                          /* no room yet */
        conn_jobdone(c);
    }
    slot_close(s);
    answer_free(s->freer, "stopped");
    s->pid = 0;
    return 0;
}

/* a command that did not end after SIGKILL (a process waiting on a hung server, G4): the slot is let go of */
static void slot_letgo(int i)
{
    struct slot *s = &g.s[i];
    struct conn *c = s->conn >= 0 ? &g.c[s->conn] : 0;
    if (kid_add(s->pid, 1, i + 1, s->started, s->cmd))
        say("a let-go command is no longer reaped (the table is full):", s->cmd);
    if (c) {
        qerr(c, 0, 0, "the command was let go of with 'wasabi free' - it may still be running on Zenix");
        conn_jobdone(c);
    }
    slot_close(s);
    answer_free(s->freer, "freed");
    s->pid = 0;
}

static void reboot_start(void)
{
    const char *argv[] = { "/bin/reboot", 0, 0 };
    int a[2];
    if (g.rebootarg == 1) argv[1] = "amigaos";
    if (g.rebootarg == 2) argv[1] = "zenix";
    g.rebootpend = 0;
    g.rtn = 0;
    g.rtext[0] = 0;
    if (fd_take(2) || wsb_os_pipe(a)) {
        if (g.rebootconn >= 0) {
            qerr(&g.c[g.rebootconn], 0, 0, "could not start /bin/reboot: no descriptor or pipe");
            conn_jobdone(&g.c[g.rebootconn]);
        }
        return;
    }
    g.rebootpid = wsb_os_spawn(argv, g.nullfd, a[1], a[1]);
    wsb_os_close(a[1]);
    fd_give(1);
    if (g.rebootpid < 0) {
        wsb_os_close(a[0]);
        fd_give(1);
        if (g.rebootconn >= 0) {
            qerr(&g.c[g.rebootconn], g.rebootpid, 0, "could not start /bin/reboot");
            conn_jobdone(&g.c[g.rebootconn]);
        }
        g.rebootpid = 0;
        return;
    }
    g.rebootout = a[0];
    g.rebootdone = 0;
}

static void exit_now(struct conn *c, int status)
{
    qok(c);
    conn_jobdone(c);
    g.quit = status;
    g.quitby = g.now + WSB_DRAIN_MS;
    farewell(status == WSB_ST_RESTART ? "restarting" : "stopping");
    for (int i = 0; i < WSB_CONNS; i++)
        if (g.c[i].fd >= 0)
            g.c[i].closing = 1;
}

void run_tick(uint32_t *tmo)
{
    int alive = 0;
    for (int i = 0; i < WSB_SLOTS; i++) {
        struct slot *s = &g.s[i];
        if (!s->pid)
            continue;
        if (!s->ended) {
            int st = wsb_os_wait(s->pid);
            if (st != -EAGAIN) {
                s->ended = 1;
                s->status = st < 0 ? 255 : st;
                s->killat = s->letgo = 0;
            }
        }
        if (s->ended) {
            /* what is left in the pipes, without waiting: a background job holding them gets no more (5.1) */
            for (int w = 0; w < 2; w++) {
                int fd = w ? s->err : s->out;
                for (int rounds = 0; fd >= 0 && rounds < 8; rounds++) {
                    struct wsb_pollfd pf = { fd, WSB_POLLIN, 0 };
                    if (wsb_os_poll(&pf, 1, 0) <= 0 || !(pf.revents & (WSB_POLLIN | WSB_POLLHUP))) {
                        wsb_os_close(fd);
                        fd_give(1);
                        if (w) s->err = -1; else s->out = -1;
                        break;
                    }
                    if (slot_read(s, w) == -EAGAIN)
                        break;                  /* the client's queue is full: on the next turn */
                    fd = w ? s->err : s->out;
                }
            }
            if (s->out < 0 && s->err < 0)
                slot_finish(s);
            if (s->pid && (s->conn < 0 || qroom(&g.c[s->conn]) >= 64))
                *tmo = 0;                       /* more to drain now (a full queue waits for POLLOUT instead) */
            continue;
        }
        alive++;
        if (s->killat && DUE(s->killat)) {
            tree_kill(s->pid, SIGKILL);
            s->killat = 0;
            s->letgo = g.now + WSB_CHILD_MS;
        } else if (s->letgo && DUE(s->letgo)) {
            slot_letgo(i);
            continue;
        }
        if (s->killat) TMO(tmo, LEFT(s->killat));
        if (s->letgo) TMO(tmo, LEFT(s->letgo));
    }
    for (int i = 0; i < WSB_BG; i++)
        if (g.k[i].pid) {
            if (wsb_os_wait(g.k[i].pid) != -EAGAIN)
                g.k[i].pid = 0;
            else
                alive++;
        }
    /* REBOOT: /bin/reboot once the other connections are closed (at most WSB_DRAIN_MS); if it comes back, ERR */
    if (g.rebootpend) {
        int others = 0;
        for (int i = 0; i < WSB_CONNS; i++)
            if (g.c[i].fd >= 0 && i != g.rebootconn)
                others++;
        if (!others || DUE(g.rebootby))
            reboot_start();
        else
            TMO(tmo, LEFT(g.rebootby));
    }
    if (g.rebootpid > 0) {
        alive++;
        if (!g.rebootdone) {
            int st = wsb_os_wait(g.rebootpid);
            if (st != -EAGAIN) {
                g.rebootdone = 1;
                g.rebootst = st;
            }
        }
        if (g.rebootdone) {
            if (g.rebootout >= 0) {
                struct wsb_pollfd pf = { g.rebootout, WSB_POLLIN, 0 };
                if (wsb_os_poll(&pf, 1, 0) > 0 && (pf.revents & (WSB_POLLIN | WSB_POLLHUP)))
                    reboot_read();
                else {
                    wsb_os_close(g.rebootout);
                    fd_give(1);
                    g.rebootout = -1;
                }
                *tmo = 0;
            }
            if (g.rebootout < 0) {
                struct conn *c = g.rebootconn >= 0 ? &g.c[g.rebootconn] : 0;
                char m[sizeof g.rtext + 40];
                struct wsb_buf b = { m, 0, sizeof m };
                while (g.rtn && g.rtext[g.rtn - 1] == ' ')
                    g.rtext[--g.rtn] = 0;
                if (g.rtn)
                    bs(&b, g.rtext);
                else {
                    bs(&b, "reboot came back (status ");
                    bi(&b, g.rebootst);
                    bc(&b, ')');
                }
                say("REBOOT:", m);
                if (c && c->fd >= 0) {
                    qerr(c, 0, 0, m);
                    conn_jobdone(c);
                }
                g.rebootpid = 0;
                g.rebootconn = -1;
            }
        }
    }
    /* RESTART and QUIT --force: when the commands have ended, or at the deadline */
    for (int i = 0; i < WSB_CONNS; i++) {
        struct conn *c = &g.c[i];
        if (c->fd < 0 || c->job != J_EXIT)
            continue;
        if (!runs_busy() || DUE(c->until))
            exit_now(c, (int)c->xflags);
        else
            TMO(tmo, LEFT(c->until));
    }
    if (alive)
        TMO(tmo, WSB_CHILD_MS);
}

void run_conn_gone(int ci)
{
    for (int i = 0; i < WSB_SLOTS; i++) {
        struct slot *s = &g.s[i];
        if (!s->pid)
            continue;
        if (s->freer == ci)
            s->freer = -1;
        if (s->conn != ci)
            continue;
        s->conn = -1;
        if (!s->ended && !s->killat && !s->letgo) {     /* its client went away: SIGINT, SIGKILL after 3 s */
            tree_kill(s->pid, SIGINT);
            s->killat = g.now + WSB_KILL_MS;
        }
    }
}

void run_stopall(void)
{
    for (int i = 0; i < WSB_SLOTS; i++)
        if (g.s[i].pid) {
            slot_close(&g.s[i]);
            g.s[i].pid = 0;
        }
    if (g.rebootout >= 0) {
        wsb_os_close(g.rebootout);
        fd_give(1);
        g.rebootout = -1;
    }
}

void cmd_slots(struct conn *c)
{
    char *t = wsb_os_alloc(1024);
    struct wsb_buf b = { t, 0, 1024 };
    if (!t) { qerr(c, -ENOMEM, 0, "out of memory"); return; }
    for (int i = 0; i < WSB_SLOTS; i++)
        if (g.s[i].pid) {
            bu(&b, (uint32_t)i + 1);
            bs(&b, " running ");
            bu(&b, (g.now - g.s[i].started) / 1000);
            bs(&b, g.s[i].conn >= 0 ? " 1 " : " 0 ");
            bs(&b, g.s[i].cmd);
            bc(&b, '\n');
        }
    for (int i = 0; i < WSB_BG; i++)
        if (g.k[i].pid && g.k[i].stuck) {
            bu(&b, (uint32_t)g.k[i].num);
            bs(&b, " stuck ");
            bu(&b, (g.now - g.k[i].started) / 1000);
            bs(&b, " 0 ");
            bs(&b, g.k[i].cmd);
            bc(&b, '\n');
        }
    qlines(c, t, b.n, 0);
}

void cmd_free(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0, n, flags = 0;
    if (wsb_get32(p, len, &off, &n)) {
        qerr(c, 0, 0, "bad FREE header");
        return;
    }
    wsb_get32(p, len, &off, &flags);
    if (n < 1 || n > WSB_SLOTS || !g.s[n - 1].pid || g.s[n - 1].ended) {
        qerr(c, 0, 0, "no command in that slot - 'wasabi slots' lists them");
        return;
    }
    struct slot *s = &g.s[n - 1];
    if (flags & WF_ASKONLY) {
        tree_kill(s->pid, SIGINT);
        q(c, WT_DATA, "asked", 5);
        qend(c);
        return;
    }
    if (s->freer >= 0) {
        qerr(c, 0, 0, "that slot is already being freed");
        return;
    }
    tree_kill(s->pid, SIGINT);
    if (!s->killat && !s->letgo)
        s->killat = g.now + WSB_KILL_MS;
    s->freer = (int)(c - g.c);
    c->job = J_FREE;
    c->slot = (int)n - 1;
}

static const char *pstate(const struct wsb_proc *p)
{
    if ((int)p->pid == g.self) return "run";
    if (p->state == 1) return "ready";
    if (p->state == 7) return "stop";
    return "wait";
}

void cmd_ps(struct conn *c)
{
    char *t = wsb_os_alloc(4096);
    struct wsb_buf b = { t, 0, 4096 };
    struct wsb_proc p;
    if (!t) { qerr(c, -ENOMEM, 0, "out of memory"); return; }
    for (unsigned i = 0;; i++) {
        int r = wsb_os_proc(i, &p);
        if (r < 0)
            break;
        if (r)
            continue;
        p.name[15] = 0;
        bx8(&b, p.pid);
        bs(&b, " p ");
        bi(&b, -p.nice);
        bc(&b, ' ');
        bs(&b, pstate(&p));
        bs(&b, " 0 -1 ");
        bs(&b, p.name);
        bc(&b, '\t');
        bs(&b, p.name);
        bc(&b, '\n');
    }
    qlines(c, t, b.n, 0);
}

static int is_server(const char *n)
{
    return !strcmp(n, "netd") || !strcmp(n, "diskd") || !strcmp(n, "fsd") || !strcmp(n, "zfsd") || !strcmp(n, "nfscd");
}

void cmd_kill(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0, flags, want = 0;
    char t[64];
    struct wsb_proc pr, hit;
    int bypid = 0, hits = 0;
    if (wsb_get32(p, len, &off, &flags) || wsb_getstr(p, len, &off, t, sizeof t)) {
        qerr(c, 0, 0, "bad KILL header");
        return;
    }
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X') && t[2]) {
        bypid = 1;
        for (const char *s = t + 2; *s; s++) {
            int d = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (*s | 32) - 'a' + 10 : -1;
            if (d < 0 || want > 0x0FFFFFFFu) { bypid = 0; break; }
            want = want << 4 | (uint32_t)d;
        }
    }
    for (unsigned i = 0;; i++) {
        int r = wsb_os_proc(i, &pr);
        if (r < 0)
            break;
        if (r)
            continue;
        pr.name[15] = 0;
        if (bypid ? pr.pid == want : !strcmp(pr.name, t)) {
            hit = pr;
            hits++;
        }
    }
    if (!hits) {
        qerr(c, 0, 0, "no process by that name");
        return;
    }
    if (hits > 1) {
        qerr(c, 0, 0, "several processes have that name - use its 0x number from 'wasabi ps'");
        return;
    }
    if ((int)hit.pid == g.self) {
        qerr(c, 0, 0, "that is wasabid itself - use restart or reboot");
        return;
    }
    if (hit.pid <= 1 || !strcmp(hit.name, "boot") || !strcmp(hit.name, "init")) {
        qerr(c, 0, 0, "boot and init are never killed");
        return;
    }
    if (is_server(hit.name) && !(flags & WK_FORCE)) {
        qerr(c, 0, 0, "a server: only with --force (killing netd cuts this connection; it starts again at the next use)");
        return;
    }
    int r = wsb_os_kill((int)hit.pid, flags & WK_FORCE ? SIGKILL : SIGINT);
    if (r == -ESRCH)
        qerr(c, 0, 0, "that process is already gone");
    else if (r < 0)
        qerr(c, r, 0, "kill failed");
    else
        qok(c);
}

void cmd_reboot(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0, flags = 0;
    struct wsb_proc pr;
    wsb_get32(p, len, &off, &flags);
    if ((flags & WB_AMIGAOS) && (flags & WB_ZENIX)) {
        qerr(c, 0, 0, "AmigaOS or Zenix next, not both");
        return;
    }
    if (g.rebootpend || g.rebootpid) {
        qerr(c, 0, 0, "a reboot is already on its way");
        return;
    }
    if (!(flags & WB_NOGUARD))
        for (unsigned i = 0;; i++) {
            int r = wsb_os_proc(i, &pr);
            if (r < 0)
                break;
            pr.name[15] = 0;
            if (!r && wsb_guarded(pr.name)) {
                char m[160];
                struct wsb_buf b = { m, 0, sizeof m };
                bs(&b, pr.name);
                bs(&b, " is running (pid ");
                bu(&b, pr.pid);
                bs(&b, "): a reboot now could leave its work half done - wait for it, or skip the guard");
                qerr(c, 0, 0, m);
                return;
            }
        }
    qok(c);
    farewell("rebooting");
    for (int i = 0; i < WSB_CONNS; i++)
        if (g.c[i].fd >= 0 && &g.c[i] != c)
            g.c[i].closing = 2;
    g.rebootarg = flags & WB_AMIGAOS ? 1 : flags & WB_ZENIX ? 2 : 0;
    g.rebootpend = 1;
    g.rebootby = g.now + WSB_DRAIN_MS;
    g.rebootconn = (int)(c - g.c);
    c->job = J_REBOOT;
}

void cmd_exit(struct conn *c, const uint8_t *p, uint32_t len, int restart)
{
    uint32_t off = 0, flags = 0;
    wsb_get32(p, len, &off, &flags);
    if (restart && !g.init) {
        qerr(c, 0, 0, "not supervised: nobody would start me again");
        return;
    }
    if (runs_busy() && !(flags & WQ_FORCE)) {
        qerr(c, 0, 0, "a command is still running - wait for it to finish, stop it with 'wasabi free N' "
                      "('wasabi slots' lists them), or pass --force");
        return;
    }
    if (runs_busy()) {                          /* --force: SIGINT, SIGKILL after 3 s, then go */
        for (int i = 0; i < WSB_SLOTS; i++)
            if (g.s[i].pid && !g.s[i].ended) {
                tree_kill(g.s[i].pid, SIGINT);
                if (!g.s[i].killat && !g.s[i].letgo)
                    g.s[i].killat = g.now + WSB_KILL_MS;
            }
        c->job = J_EXIT;
        c->xflags = restart ? WSB_ST_RESTART : WSB_ST_STOP;
        c->until = g.now + WSB_KILL_MS + 2 * WSB_CHILD_MS;
        return;
    }
    exit_now(c, restart ? WSB_ST_RESTART : WSB_ST_STOP);
}
