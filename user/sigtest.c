// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* sigtest CHECK...: checks of the kernel's events and signals (DESIGN D16, docs/d16-events-and-signals.md). Each check
 * prints "sigtest CHECK: PASS (...)" or "sigtest CHECK: FAIL (...)"; the exit status is the number of failures.
 * tools/signal-test.sh runs them in the emulator (docs/man/sigtest.txt lists them). The children are this program
 * again, started with an argument that begins with '_'. */

static char self[64];                   /* this program's path, for the children */
static int fails;

static void result(const char *name, int ok, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("sigtest %s: %s (%s)\n", name, ok ? "PASS" : "FAIL", buf);
    if (!ok)
        fails++;
}

static int state_of(int pid)            /* the process's state (procinfo), -1 if there is none */
{
    struct uproc p;
    for (u32 i = 0; i < 64; i++)
        if (sys_procinfo(i, &p) == 0 && p.state && (int)p.pid == pid)
            return (int)p.state;
    return -1;
}

static int child3(const char *a1, const char *a2, const int *stdio)
{
    char *argv[4] = { self, (char *)a1, (char *)a2, 0 };
    return sys_spawn(self, argv, stdio);
}
static int child(const char *a1, const char *a2) { return child3(a1, a2, 0); }

static volatile int hits[NSIG], order[8], norder;
static void count(int sig) { hits[sig]++; if (norder < 8) order[norder++] = sig; }
static void catch(int sig, sighandler_t h, u32 mask, int flags)
{
    struct sigaction a;
    a.sa_handler = h; a.sa_mask = mask; a.sa_flags = flags;
    sigaction(sig, &a, 0);
}
static void reset_hits(void) { for (int i = 0; i < NSIG; i++) hits[i] = 0; norder = 0; }

/* ---- stopsrv: a stopped server sitting in receive is not run by a message (rule 2) ---- */
#define STOP_PORT "sigt.stop"
static int srv_main(int slow)
{
    int port = sys_port_create(STOP_PORT);
    if (port < 0)
        return 1;
    struct umsg *m = sys_recv(port, 1);
    for (;;) {
        if ((int)m < 0 && (int)m > -4096)
            return 2;
        if (m->type == 9) {
            sys_reply(m, 0);
            return 0;
        }
        m->a[2] = m->a[0] + m->a[1];
        if (slow)
            sys_sleep(100);             /* a second before the reply (srvcall) */
        m = sys_replyrecv(m, 0, port);
    }
}
static int wait_port(void)
{
    int port = -1;
    for (int i = 0; i < 100 && (port = sys_port_find(STOP_PORT)) < 0; i++)
        sys_sleep(1);
    return port;
}
static void quit_srv(int port, int srv)
{
    struct umsg q;
    memset(&q, 0, sizeof q);
    q.type = 9;
    sys_call(port, &q);
    sys_wait(srv);
}
static int cli_main(void)
{
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = 1; m.a[0] = 40; m.a[1] = 2;
    int r = sys_call(sys_port_find(STOP_PORT), &m);
    return r == 0 && m.a[2] == 42 ? 0 : 1;
}
static void t_stopsrv(void)
{
    int srv = child("_srv", 0), port = wait_port();
    if (srv < 0 || port < 0) { result("stopsrv", 0, "the server did not start"); return; }
    sys_sleep(5);                                       /* it is in receive now */
    sys_jobctl(srv, 0);
    sys_sleep(10);
    int s1 = state_of(srv);
    int cli = child("_cli", 0);
    sys_sleep(100);                                     /* a second: a running server answers at once */
    int done = sys_waitnb(cli), s2 = state_of(srv);
    sys_jobctl(srv, 1);
    int st = done == -2 ? sys_wait(cli) : done;
    quit_srv(port, srv);
    result("stopsrv", s1 == 7 && done == -2 && s2 == 7 && st == 0,
           "server stopped %d, client waiting %d, still stopped %d, answered after SIGCONT %d", s1 == 7, done == -2,
           s2 == 7, st == 0);
}

/* ---- handler: a handler runs at the end of the call that sent it; nested ones wait for their mask ---- */
static void nest1(int sig)
{
    count(sig);
    if (hits[SIGUSR1] != 1)
        return;
    raise(SIGUSR2);                     /* blocked while this runs (sa_mask): after it */
    raise(SIGUSR1);                     /* itself: blocked too, after it, once */
    if (hits[SIGUSR2] == 0 && hits[SIGUSR1] == 1 && norder == 1)
        order[7] = 1;                   /* nothing ran inside */
}
static void t_handler(void)
{
    reset_hits();
    order[7] = 0;
    catch(SIGUSR2, count, 0, 0);
    catch(SIGUSR1, nest1, 1u << SIGUSR2, 0);
    zerrno = 77;
    volatile u32 a = 0x12345678, b = 0x9abcdef0;
    int r = kill(sys_getpid(), SIGUSR1);
    int ok = r == 0 && hits[SIGUSR1] == 2 && hits[SIGUSR2] == 1 && order[7] == 1 && zerrno == 77 &&
             a == 0x12345678 && b == 0x9abcdef0;
    result("handler", ok, "kill 0: %d, USR1 ran %d times, USR2 %d, nested ones waited %d, order %d %d %d, errno kept %d",
           r == 0, hits[SIGUSR1], hits[SIGUSR2], order[7], order[0], order[1], order[2], zerrno == 77);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
}

/* ---- block: a blocked signal waits; unblocking delivers it (a handler) or acts (a default) ---- */
static int blockterm_main(void)
{
    sigset_t s = 1u << SIGTERM;
    sigprocmask(SIG_BLOCK, &s, 0);
    raise(SIGTERM);
    printf("sigtest: still alive with SIGTERM blocked\n");
    sigprocmask(SIG_UNBLOCK, &s, 0);
    return 5;                           /* not reached */
}
static void t_block(void)
{
    reset_hits();
    catch(SIGUSR1, count, 0, 0);
    sigset_t s = 1u << SIGUSR1, old;
    sigprocmask(SIG_BLOCK, &s, &old);
    raise(SIGUSR1);
    raise(SIGUSR1);
    int during = hits[SIGUSR1];
    sigprocmask(SIG_SETMASK, &old, 0);
    int after = hits[SIGUSR1];
    sigset_t k = (1u << SIGKILL) | (1u << SIGSTOP), now;
    sigprocmask(SIG_BLOCK, &k, &now);
    sigprocmask(SIG_SETMASK, &old, &now);
    int c = child("_blockterm", 0), st = sys_wait(c);
    result("block", during == 0 && after == 1 && !(now & k) && st == 143,
           "blocked: not run %d, unblocked: run once %d, KILL/STOP never blocked %d, a blocked TERM ends it when "
           "unblocked: status %d", during == 0, after, !(now & k), st);
    signal(SIGUSR1, SIG_DFL);
}

/* ---- ignore: ignored ones do nothing and stay ignored in a child; caught ones are the default there ---- */
static int raise_main(const char *n) { raise(atoi(n)); return 0; }
static void t_ignore(void)
{
    signal(SIGUSR1, SIG_IGN);
    raise(SIGUSR1);
    int c1 = child("_raise", "10"), s1 = sys_wait(c1);
    catch(SIGUSR2, count, 0, 0);
    int c2 = child("_raise", "12"), s2 = sys_wait(c2);
    sighandler_t o = signal(SIGUSR1, SIG_DFL);
    result("ignore", s1 == 0 && s2 == 140 && o == SIG_IGN,
           "ignored here and in a child: %d, a caught one is the default in a child: status %d, signal() gives the old "
           "SIG_IGN %d", s1, s2, o == SIG_IGN);
    signal(SIGUSR2, SIG_DFL);
}

/* ---- kill: kill(pid, 0) only asks; the old form gives 130, TERM 143, KILL 137; -1 and nobody are refused ---- */
static int sleeper_main(void) { for (;;) sys_sleep(1000); return 0; }
static void t_kill(void)
{
    int c = child("_sleeper", 0);
    sys_sleep(5);
    int p0 = kill(c, 0), alive = state_of(c) > 0 && sys_waitnb(c) == -2;
    sys_kill(c);
    int old = sys_wait(c);
    c = child("_sleeper", 0);
    sys_sleep(5);
    kill(c, SIGTERM);
    int term = sys_wait(c);
    c = child("_sleeper", 0);
    sys_sleep(5);
    kill(c, SIGKILL);
    int kl = sys_wait(c);
    int none = kill(c, 0) == -1 && zerrno == ESRCH;
    int all = kill(-1, SIGTERM) == -1 && zerrno == EINVAL;
    int bad = kill(c, 99) == -1 && zerrno == EINVAL;
    result("kill", !p0 && alive && old == 130 && term == 143 && kl == 137 && none && all && bad,
           "kill 0 only asks %d, old form %d, TERM %d, KILL %d, gone: ESRCH %d, -1: EINVAL %d, 99: EINVAL %d",
           !p0 && alive, old, term, kl, none, all, bad);
}

/* ---- alarm: a CPU-bound program ends at its alarm (142); with a handler it goes on ---- */
static volatile int flag;
static void setflag(int sig) { (void)sig; flag = 1; }
static int spin_main(int handler)
{
    if (handler)
        signal(SIGALRM, setflag);
    sys_alarm(50);
    while (!flag)
        ;                               /* no system call: only the tick can take it */
    return 7;
}
static void t_alarm(void)
{
    u32 t0 = sys_ticks();
    int c = child("_spin", "0"), st = sys_wait(c);
    u32 dt = sys_ticks() - t0;
    t0 = sys_ticks();
    c = child("_spin", "1");
    int st2 = sys_wait(c);
    u32 dt2 = sys_ticks() - t0;
    int left = sys_alarm(500), left2 = sys_alarm(0), left3 = sys_alarm(0);
    result("alarm", st == 142 && dt >= 45 && dt < 300 && st2 == 7 && dt2 < 300 && left == 0 && left2 > 400 &&
           left3 == 0, "a CPU-bound program ends with status %d after %u ticks, with a handler it goes on: %d after %u, "
           "ticks left: %d %d %d", st, dt, st2, dt2, left, left2, left3);
}

/* ---- restart: SA_RESTART restarts a pipe read; without it the read gives EINTR; sleep is never restarted ---- */
static int writer_main(const char *ticks) { sys_sleep((u32)atoi(ticks)); sys_write(1, "x", 1); return 0; }
static int read_pipe_with_alarm(int flags, int *got, u32 *dt)
{
    int fds[2];
    sys_pipe(fds);
    int io[3] = { -1, fds[1], -1 };
    int c = child3("_writer", "150", io);
    sys_close(fds[1]);
    catch(SIGALRM, count, 0, flags);
    reset_hits();
    sys_alarm(50);
    u32 t0 = sys_ticks();
    char b[4];
    *got = sys_read(fds[0], b, 4);
    *dt = sys_ticks() - t0;
    sys_wait(c);
    sys_close(fds[0]);
    signal(SIGALRM, SIG_DFL);
    return hits[SIGALRM];
}
static void t_restart(void)
{
    int g1, g2;
    u32 d1, d2;
    int h1 = read_pipe_with_alarm(SA_RESTART, &g1, &d1);
    int h2 = read_pipe_with_alarm(0, &g2, &d2);
    catch(SIGALRM, count, 0, SA_RESTART);
    reset_hits();
    sys_alarm(30);
    u32 t0 = sys_ticks();
    int sr = sys3(7, 300, 0, 0);
    u32 d3 = sys_ticks() - t0;
    signal(SIGALRM, SIG_DFL);
    result("restart", h1 == 1 && g1 == 1 && d1 >= 140 && h2 == 1 && g2 == -EINTR && d2 < 100 && hits[SIGALRM] == 1 &&
           sr == -EINTR && d3 < 100, "SA_RESTART: the read went on (%d byte after %u ticks), without: %d after %u, "
           "sleep cut short (%d after %u ticks), never restarted", g1, d1, g2, d2, sr, d3);
}

/* ---- ttyread: a tty read is restarted after a handler (SA_RESTART); the line typed after it arrives whole ---- */
static void t_ttyread(void)
{
    catch(SIGALRM, count, 0, SA_RESTART);
    reset_hits();
    printf("sigtest: type a line\n");
    sys_alarm(20);
    char b[32];
    int n = sys_read(0, b, sizeof b - 1);
    b[n > 0 ? n : 0] = 0;
    signal(SIGALRM, SIG_DFL);
    result("ttyread", hits[SIGALRM] == 1 && n == 4 && !strcmp(b, "abc\n"),
           "the handler ran %d time(s) during the read, then it returned %d bytes", hits[SIGALRM], n);
}

/* ---- chld: SIGCHLD on a child's end (and stop); ignored: children are reaped, wait gives ECHILD ---- */
static int exit_main(void) { return 3; }
static void t_chld(void)
{
    reset_hits();
    catch(SIGCHLD, count, 0, SA_RESTART);
    int c = child("_exit", 0), st = sys_wait(c);
    int onexit = hits[SIGCHLD];
    c = child("_sleeper", 0);
    sys_sleep(5);
    sys_jobctl(c, 0);
    sys_sleep(10);
    int onstop = hits[SIGCHLD] - onexit;
    kill(c, SIGKILL);
    sys_wait(c);
    signal(SIGCHLD, SIG_IGN);
    c = child("_exit", 0);
    sys_sleep(20);
    int gone = state_of(c) == -1, w = sys_wait(-1);
    signal(SIGCHLD, SIG_DFL);
    result("chld", st == 3 && onexit == 1 && onstop == 1 && gone && w == -ECHILD,
           "a handler on an end %d and on a stop %d, ignored: the child reaped by itself %d, wait gives ECHILD %d",
           onexit, onstop, gone, w == -ECHILD);
}

/* ---- pipe: writing to a pipe nobody reads ends the writer (141); ignored, the write fails with EPIPE ---- */
static int pipew_main(const char *ign)
{
    if (atoi(ign))
        signal(SIGPIPE, SIG_IGN);
    int r = sys_write(1, "abc", 3);
    return r == -EPIPE ? 0 : 9;
}
static void t_pipe(void)
{
    int st[2];
    for (int k = 0; k < 2; k++) {
        int fds[2];
        sys_pipe(fds);
        sys_close(fds[0]);
        int io[3] = { -1, fds[1], -1 };
        int c = child3("_pipew", k ? "1" : "0", io);
        sys_close(fds[1]);
        st[k] = sys_wait(c);
    }
    int fds[2];
    sys_pipe(fds);
    sys_close(fds[0]);
    int io[3] = { -1, fds[1], -1 };
    char *zargv[] = { "zup", "-l", 0 };
    int z = sys_spawn("/sbin/zup", zargv, io);
    sys_close(fds[1]);
    int zs = z < 0 ? -1 : sys_wait(z);
    result("pipe", st[0] == 141 && st[1] == 0 && zs == 0,
           "default: the writer ends with %d, ignored: EPIPE %d, zup -l into a closed pipe: status %d (it ignores "
           "SIGPIPE)", st[0], st[1] == 0, zs);
}

/* ---- poll: a pipe, a port and a timeout together ---- */
static int sender_main(const char *ticks)
{
    sys_sleep((u32)atoi(ticks));
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = 1;
    return sys_call(sys_port_find("sigt.poll"), &m) == 5 ? 0 : 1;
}
static void t_poll(void)
{
    int port = sys_port_create("sigt.poll"), fds[2];
    sys_pipe(fds);
    struct pollfd p[3];
    p[0].fd = fds[0]; p[0].events = POLLIN;
    p[1].fd = port; p[1].events = POLLIN | POLLPORT;
    p[2].fd = 15; p[2].events = POLLIN;
    u32 t0 = sys_ticks();
    int r0 = poll(p, 2, 300);
    u32 d0 = sys_ticks() - t0;
    int io[3] = { -1, fds[1], -1 };
    int w = child3("_writer", "30", io);
    t0 = sys_ticks();
    int r1 = poll(p, 2, 5000);
    u32 d1 = sys_ticks() - t0;
    int pin = p[0].revents == POLLIN && p[1].revents == 0;
    char b[4];
    sys_read(fds[0], b, 4);
    sys_wait(w);
    int s = child("_sender", "30");
    t0 = sys_ticks();
    int r2 = poll(p, 2, 5000);
    u32 d2 = sys_ticks() - t0;
    int mport = p[1].revents == POLLIN && p[0].revents == 0;
    struct umsg *m = sys_recv(port, 0);
    if ((int)m > 0)
        sys_reply(m, 5);
    int ss = sys_wait(s);
    sys_close(fds[1]);
    int r3 = poll(p, 3, 0);
    int hup = (p[0].revents & POLLHUP) && p[2].revents == POLLNVAL;
    sys_close(fds[0]);
    result("poll", r0 == 0 && d0 >= 29 && r1 == 1 && pin && d1 >= 25 && r2 == 1 && mport && ss == 0 && r3 == 2 && hup,
           "timeout %d after %u ticks, pipe %d after %u, port %d after %u (the reply arrived %d), closed: HUP and an "
           "unused fd NVAL %d", r0, d0, pin, d1, mport, d2, ss == 0, hup);
}

/* ---- select: the tty, a pipe and a port at once: a typed line wakes it ---- */
static void t_select(void)
{
    int port = sys_port_create("sigt.sel"), fds[2];
    sys_pipe(fds);
    struct pollfd p[3];
    p[0].fd = 0; p[0].events = POLLIN;
    p[1].fd = fds[0]; p[1].events = POLLIN;
    p[2].fd = port; p[2].events = POLLIN | POLLPORT;
    printf("sigtest: type for select\n");
    u32 t0 = sys_ticks();
    int r = poll(p, 3, 20000);
    u32 dt = sys_ticks() - t0;
    char b[16];
    int n = r == 1 && p[0].revents == POLLIN ? sys_read(0, b, sizeof b) : -1;
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(0, &rs);
    FD_SET(fds[0], &rs);
    struct timeval tv = { 0, 200000 };
    int sr = select(fds[0] + 1, &rs, 0, 0, &tv);
    sys_close(fds[0]);
    sys_close(fds[1]);
    result("select", r == 1 && p[0].revents == POLLIN && !p[1].revents && !p[2].revents && n == 4 && sr == 0,
           "woken by the tty after %u ticks: %d (pipe %d, port %d), the line %d bytes, select's timeout %d", dt,
           p[0].revents == POLLIN, p[1].revents, p[2].revents, n, sr);
}

/* ---- group: process groups; kill(-g) reaches every member; an empty group is ESRCH ---- */
static void t_group(void)
{
    int a = child("_sleeper", 0), b = child("_sleeper", 0), c = child("_sleeper", 0);
    int r1 = setpgid(a, a), r2 = setpgid(b, a), r3 = setpgid(c, a);
    int g = getpgid(b), mine = getpgrp();
    int notmine = setpgid(1, 0) == -1 && zerrno == ESRCH;
    int k = kill(-a, SIGTERM);
    int sa = sys_wait(a), sb = sys_wait(b), sc = sys_wait(c);
    int empty = kill(-a, SIGTERM) == -1 && zerrno == ESRCH;
    int z = child("_exit", 0);
    sys_sleep(10);
    int zomb = setpgid(z, z) == 0;
    sys_wait(z);
    result("group", !r1 && !r2 && !r3 && g == a && mine != a && notmine && !k && sa == 143 && sb == 143 && sc == 143 &&
           empty && zomb, "joined %d, getpgid %d, own group apart %d, not a child: ESRCH %d, kill(-g) ended all three: "
           "%d %d %d, empty: ESRCH %d, a zombie child joins %d", !r1 && !r2 && !r3, g == a, mine != a, notmine, sa, sb,
           sc, empty, zomb);
}

/* ---- cont: a caught signal waits while stopped; SIGCONT continues and runs its handler ---- */
static int contchild_main(void)
{
    catch(SIGCONT, count, 0, SA_RESTART);
    catch(SIGUSR1, count, 0, SA_RESTART);
    while (!hits[SIGUSR1] || !hits[SIGCONT])
        sys_sleep(2);
    printf("sigtest: child order %d %d\n", order[0], order[1]);
    return 10 * order[0] + order[1] == 10 * SIGCONT + SIGUSR1 || 10 * order[0] + order[1] == 10 * SIGUSR1 + SIGCONT
           ? 0 : 1;
}
static void t_cont(void)
{
    int c = child("_contchild", 0);
    sys_sleep(20);
    sys_jobctl(c, 0);
    sys_sleep(10);
    kill(c, SIGUSR1);
    sys_sleep(30);
    int still = state_of(c) == 7 && sys_waitnb(c) == -2;
    kill(c, SIGCONT);
    int st = sys_wait(c);
    result("cont", still && st == 0, "a caught USR1 waits while stopped %d, SIGCONT runs both handlers: status %d",
           still, st);
}

/* ---- srvcall: a signal during a server call is delivered after the reply ---- */
static struct umsg callmsg;
static volatile int seen_reply;
static void at_usr1(int sig) { (void)sig; seen_reply = callmsg.a[2] == 42 ? 1 : 2; }
static int killer_main(const char *pid) { sys_sleep(30); return kill(atoi(pid), SIGUSR1) ? 1 : 0; }
static void t_srvcall(void)
{
    int srv = child("_slowsrv", 0), port = wait_port();
    if (port < 0) { result("srvcall", 0, "no server"); return; }
    catch(SIGUSR1, at_usr1, 0, 0);
    seen_reply = 0;
    char me[12];
    snprintf(me, sizeof me, "%d", sys_getpid());
    int k = child("_killer", me);
    memset(&callmsg, 0, sizeof callmsg);
    callmsg.type = 1; callmsg.a[0] = 40; callmsg.a[1] = 2;
    u32 t0 = sys_ticks();
    int r = sys_call(port, &callmsg);
    u32 dt = sys_ticks() - t0;
    int ks = sys_wait(k);
    quit_srv(port, srv);
    signal(SIGUSR1, SIG_DFL);
    result("srvcall", r == 0 && ks == 0 && seen_reply == 1 && dt >= 90,
           "the call returned %d after %u ticks, the handler ran after the reply %d", r, dt, seen_reply == 1);
}

/* ---- stack: a handler near the stack's end: refused with a message, never written below the guard ---- */
extern char _start[];
static u32 stack_bottom(void)           /* where the user stack begins: above the image and bss (ZEXE mem_size) */
{
    return (u32)_start + ((*(u32 *)(_start + 12) + 15) & ~15u);
}
static void deep(u32 limit)
{
    volatile char pad[64];
    pad[0] = 1;
    if ((u32)pad > limit)
        deep(limit);
    else
        raise(SIGUSR1);
    pad[1] = pad[0];
}
static int deep_main(const char *room)
{
    catch(SIGUSR1, count, 0, 0);
    deep(stack_bottom() + (u32)atoi(room));
    return hits[SIGUSR1] == 1 ? 0 : 1;
}
static void t_stack(void)
{
    int c = child("_deep", "1400"), ok1 = sys_wait(c);
    c = child("_deep", "300");
    int st = sys_wait(c);
    result("stack", ok1 == 0 && st == 139, "a handler 1.4 KB above the guard runs: %d, one that would leave less than "
           "512 bytes: killed %d (the kernel log says why)", ok1 == 0, st);
}

/* ---- forged: sigreturn's checks (section 5): the frame must be the process's, its PC even, its SR only the
 * condition codes ---- */
static void privileged(void)
{
    unsigned short sr;
    __asm__ volatile("move.w %%sr,%0" : "=d"(sr));      /* user mode: a privilege violation (vector 8) */
    sys_exit(0);                                        /* supervisor mode would get here */
}
static void spinforever(void) { for (;;) ; }
static int forge_main(const char *how)
{
    static u32 f[21];
    u32 sp;
    __asm__ volatile("move.l %%sp,%0" : "=r"(sp));
    memset(f, 0, sizeof f);
    f[0] = 84;
    f[16] = sp - 256;                                   /* a7 */
    f[17] = (u32)privileged;
    int k = atoi(how);
    if (k == 0) return sys_sigreturn((void *)0x10) ? 3 : 4;   /* outside the process */
    if (k == 1) f[18] = 0x2700;                         /* the S bit: must never be taken */
    if (k == 2) f[18] = 0x1000;                         /* the M bit */
    if (k == 3) f[17] |= 1;                             /* an odd PC */
    if (k == 4) { f[17] = (u32)spinforever; f[18] = 0x0700; sys_alarm(50); }   /* every interrupt masked: the tick
                                                         * must still come, so the alarm ends it */
    sys_sigreturn(f);
    return 5;
}
static void t_forged(void)
{
    int st[5];
    for (int k = 0; k < 5; k++) {
        char a[4];
        snprintf(a, sizeof a, "%d", k);
        int c = child("_forge", a);
        st[k] = sys_wait(c);
    }
    result("forged", st[0] == 139 && st[1] == 139 && st[2] == 139 && st[3] == 139 && st[4] == 142,
           "outside: %d, SR 0x2700: %d, SR 0x1000: %d, odd PC: %d, SR 0x0700 and a busy loop: the alarm %d", st[0],
           st[1], st[2], st[3], st[4]);
}

/* ---- jobber: for the shell's job control test: TSTP and CONT handlers (as less and edit) ---- */
static void on_tstp(int sig)
{
    (void)sig;
    printf("sigtest: TSTP handler ran\n");
    signal(SIGTSTP, SIG_DFL);
    sigset_t m = 1u << SIGTSTP;
    sigprocmask(SIG_UNBLOCK, &m, 0);
    raise(SIGTSTP);                     /* stop for real, as a program that restored its terminal first */
    signal(SIGTSTP, on_tstp);
}
static void on_cont(int sig) { (void)sig; printf("sigtest: CONT handler ran\n"); }
static int jobber_main(void)
{
    signal(SIGTSTP, on_tstp);
    signal(SIGCONT, on_cont);
    printf("sigtest: jobber ready\n");
    for (;;)
        sys_sleep(100);
    return 0;
}

/* ---- intr: SIGINT with a handler (the shell's ^C test): prints and ends with 3 ---- */
static void on_int(int sig) { (void)sig; printf("sigtest: INT handler ran\n"); sys_exit(3); }
static int intr_main(void)
{
    signal(SIGINT, on_int);
    printf("sigtest: waiting for ^C\n");
    for (;;)
        sys_sleep(100);
    return 0;
}

int main(int argc, char **argv)
{
    if (!path_find(argv[0], self, sizeof self))
        strcpy(self, "/bin/sigtest");
    if (argc > 1 && argv[1][0] == '_') {
        const char *c = argv[1], *a = argc > 2 ? argv[2] : "0";
        if (!strcmp(c, "_srv")) return srv_main(0);
        if (!strcmp(c, "_slowsrv")) return srv_main(1);
        if (!strcmp(c, "_cli")) return cli_main();
        if (!strcmp(c, "_blockterm")) return blockterm_main();
        if (!strcmp(c, "_raise")) return raise_main(a);
        if (!strcmp(c, "_sleeper")) return sleeper_main();
        if (!strcmp(c, "_spin")) return spin_main(atoi(a));
        if (!strcmp(c, "_writer")) return writer_main(a);
        if (!strcmp(c, "_exit")) return exit_main();
        if (!strcmp(c, "_pipew")) return pipew_main(a);
        if (!strcmp(c, "_sender")) return sender_main(a);
        if (!strcmp(c, "_contchild")) return contchild_main();
        if (!strcmp(c, "_killer")) return killer_main(a);
        if (!strcmp(c, "_deep")) return deep_main(a);
        if (!strcmp(c, "_forge")) return forge_main(a);
        if (!strcmp(c, "_jobber")) return jobber_main();
        if (!strcmp(c, "_intr")) return intr_main();
        return 99;
    }
    static const struct { const char *name; void (*fn)(void); } checks[] = {
        { "stopsrv", t_stopsrv }, { "handler", t_handler }, { "block", t_block }, { "ignore", t_ignore },
        { "kill", t_kill }, { "alarm", t_alarm }, { "restart", t_restart }, { "ttyread", t_ttyread },
        { "chld", t_chld }, { "pipe", t_pipe }, { "poll", t_poll }, { "select", t_select }, { "group", t_group },
        { "cont", t_cont }, { "srvcall", t_srvcall }, { "stack", t_stack }, { "forged", t_forged },
    };
    for (int i = 1; i < argc; i++) {
        u32 k = 0;
        while (k < sizeof checks / sizeof checks[0] && strcmp(argv[i], checks[k].name))
            k++;
        if (k < sizeof checks / sizeof checks[0])
            checks[k].fn();
        else {
            printf("sigtest: unknown check %s\n", argv[i]);
            fails++;
        }
    }
    return fails;
}
