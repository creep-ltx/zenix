// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* sigtest CHECK...: checks of the kernel's events and signals (DESIGN D16, docs/d16-events-and-signals.md). Each check
 * prints "sigtest CHECK: PASS (...)" or "sigtest CHECK: FAIL (...)"; the exit status is the number of failures.
 * tools/signal-test.sh runs them in the emulator. The children are this program again, started with an argument
 * that begins with '_'. */

static char self[64];                   /* this program's path, for the children */
static int fails;

static void result(const char *name, int ok, const char *fmt, ...)
{
    char buf[160];
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

static int child(const char *a1, const char *a2)
{
    char *argv[4] = { self, (char *)a1, (char *)a2, 0 };
    return sys_spawn(self, argv, 0);
}

/* ---- stopsrv: a stopped server sitting in receive is not run by a message (rule 2) ---- */
#define STOP_PORT "sigt.stop"
static int srv_main(void)
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
        m = sys_replyrecv(m, 0, port);
    }
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
    int srv = child("_srv", 0), port = -1;
    for (int i = 0; i < 100 && (port = sys_port_find(STOP_PORT)) < 0; i++)
        sys_sleep(1);
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
    struct umsg q;
    memset(&q, 0, sizeof q);
    q.type = 9;
    sys_call(port, &q);
    sys_wait(srv);
    result("stopsrv", s1 == 7 && done == -2 && s2 == 7 && st == 0,
           "server stopped %d, client waiting %d, still stopped %d, answered after SIGCONT %d", s1 == 7, done == -2,
           s2 == 7, st == 0);
}

int main(int argc, char **argv)
{
    if (!path_find(argv[0], self, sizeof self))
        strcpy(self, "/bin/sigtest");
    if (argc > 1 && argv[1][0] == '_') {
        const char *c = argv[1];
        if (!strcmp(c, "_srv")) return srv_main();
        if (!strcmp(c, "_cli")) return cli_main();
        return 99;
    }
    for (int i = 1; i < argc; i++) {
        const char *t = argv[i];
        if (!strcmp(t, "stopsrv")) t_stopsrv();
        else { printf("sigtest: unknown check %s\n", t); fails++; }
    }
    return fails;
}
