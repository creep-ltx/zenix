// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* priotest fair|prio|wake|inherit: checks of the scheduler's priorities (DESIGN D10), timed with the E-clock. Each
 * prints "priotest MODE: PASS ..." or "priotest MODE: FAIL ..." and exits 0 or 1. tools/prio-test.sh runs them.
 *   fair     two spinners of the same priority take turns only at the end of a 50 ms slice
 *   prio     a spinner at nice 0 gets the CPU and keeps it; one at nice 5 does not run meanwhile
 *   wake     a nice 0 process waking from a one-tick sleep runs at once, although a nice 5 spinner is busy
 *   inherit  a nice 0 client calling a nice 10 server is not held up by a nice 5 spinner: the server works at the
 *            client's level while it serves it (QNX-style priority inheritance), and drops back after the reply
 * The children share one block of the parent's memory (no MMU: the address is passed as an argument). */
#define E_MS   709u                     /* E-clock ticks per millisecond (PAL; NTSC is 1% faster: within the limits) */
#define GAP    (2 * E_MS)               /* a pause this long in a spinner's own clock readings: it was off the CPU */
#define MAXS   64

struct rec { volatile u32 n, done; u32 start[MAXS], end[MAXS]; };
struct shared { struct rec r[2]; volatile u32 ready, serving_pri; };

static struct shared *sh;

static void spin(struct rec *r, u32 ms)
{
    u32 t0 = sys_hrtime(), last = t0;
    r->start[0] = t0;
    r->n = 1;
    for (;;) {
        u32 t = sys_hrtime();
        if (t - last > GAP && r->n < MAXS) {
            r->end[r->n - 1] = last;
            r->start[r->n++] = t;
        }
        last = t;
        if (t - t0 >= ms * E_MS)
            break;
    }
    r->end[r->n - 1] = last;
    r->done = 1;
}

static u32 my_pri(void)
{
    struct uproc p;
    int me = sys_getpid();
    for (u32 i = 0; i < 64; i++)
        if (sys_procinfo(i, &p) == 0 && p.state && (int)p.pid == me) return p.pri;
    return 99;
}

static u32 pri_of(int pid)
{
    struct uproc p;
    for (u32 i = 0; i < 64; i++)
        if (sys_procinfo(i, &p) == 0 && p.state && (int)p.pid == pid) return p.pri;
    return 99;
}

static void server(void)
{
    int port = sys_port_create("priot");
    if (port < 0) { fprintf(2, "priotest: cannot create the port\n"); sys_exit(1); }
    sh->ready = 1;
    struct umsg *m = sys_recv(port, 1);
    int first = 1;
    while (m && m != (struct umsg *)-1) {
        if (m->type == 9) { sys_reply(m, 0); break; }
        if (first) { sh->serving_pri = my_pri(); first = 0; }
        u32 t0 = sys_hrtime();
        while (sys_hrtime() - t0 < E_MS) ;         /* a millisecond of work for the client */
        m = sys_replyrecv(m, 0, port);
    }
    sys_exit(0);
}

/* the children: priotest -c ROLE ADDRESS NICE MS */
static int child(char **argv)
{
    sh = (struct shared *)strtoul(argv[3], 0, 16);
    if (setpriority(PRIO_PROCESS, 0, atoi(argv[4])) < 0) { fprintf(2, "priotest: setpriority: %s\n", zwhy("refused")); return 1; }
    if (argv[2][0] == 's') server();
    spin(&sh->r[argv[2][0] - '0'], (u32)atoi(argv[5]));
    return 0;
}

static int start(const char *role, int nice, u32 ms)
{
    char a[12], n[8], t[8];
    snprintf(a, sizeof a, "%x", (u32)sh);
    snprintf(n, sizeof n, "%d", nice);
    snprintf(t, sizeof t, "%u", ms);
    char *argv[] = { "priotest", "-c", (char *)role, a, n, t, 0 };
    int pid = sys_spawn("/bin/priotest", argv, 0);
    if (pid < 0) { fprintf(2, "priotest: cannot start a child: %s\n", zwhy("failed")); sys_exit(1); }
    return pid;
}

static int result(const char *mode, int ok, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("priotest %s: %s %s\n", mode, ok ? "PASS" : "FAIL", buf);
    return ok ? 0 : 1;
}

static u32 ms_of(u32 e) { return e / E_MS; }

static int t_fair(void)
{
    int a = start("0", 0, 1500), b = start("1", 0, 1500);
    sys_wait(a);
    sys_wait(b);
    struct rec *r0 = &sh->r[0], *r1 = &sh->r[1];
    /* judged only while both run: from the later start to the earlier end (loading the second one takes a while, and
     * the one left alone at the end runs on); a stretch cut by either edge is left out */
    u32 w0 = (int)(r1->start[0] - r0->start[0]) > 0 ? r1->start[0] : r0->start[0];
    u32 w1 = (int)(r1->end[r1->n - 1] - r0->end[r0->n - 1]) < 0 ? r1->end[r1->n - 1] : r0->end[r0->n - 1];
    u32 lo = 0xFFFFFFFFu, hi = 0, mid = 0;
    int turns = 1;
    for (int k = 0; k < 2; k++) {
        struct rec *r = &sh->r[k], *o = &sh->r[1 - k];
        for (u32 i = 0; i < r->n; i++) {
            if ((int)(r->start[i] - w0) <= 0 || (int)(w1 - r->end[i]) <= 0)
                continue;
            u32 len = ms_of(r->end[i] - r->start[i]);
            if (len < lo) lo = len;
            if (len > hi) hi = len;
            mid++;
            /* taking turns: between this stretch and the next, a stretch of the other one starts */
            if (i + 1 < r->n && (int)(w1 - r->start[i + 1]) > 0) {
                int seen = 0;
                for (u32 j = 0; j < o->n; j++)
                    if ((int)(o->start[j] - r->end[i]) >= 0 && (int)(r->start[i + 1] - o->start[j]) >= 0) seen = 1;
                if (!seen) turns = 0;
            }
        }
    }
    int ok = r0->n < MAXS && r1->n < MAXS && mid >= 10 && lo >= 35 && hi <= 70 && turns;
    if (!ok) {                                      /* what happened, for the log */
        u32 t0 = r0->start[0];
        for (int k = 0; k < 2; k++)
            for (u32 i = 0; i < sh->r[k].n; i++)
                printf("  spinner %d: %u..%u ms\n", k, ms_of(sh->r[k].start[i] - t0), ms_of(sh->r[k].end[i] - t0));
    }
    return result("fair", ok, "(%u whole stretches while both ran: %u..%u ms, want 35..70; %s)", mid, mid ? lo : 0, hi,
                  turns ? "they take turns" : "NOT taking turns");
}

static int t_prio(void)
{
    int lp = start("0", 5, 1500);
    sys_sleep(20);                                  /* the nice 5 spinner is running */
    int hp = start("1", 0, 500);
    sys_wait(hp);
    sys_wait(lp);
    struct rec *l = &sh->r[0], *h = &sh->r[1];
    u32 hs = h->start[0], he = h->end[h->n - 1];
    int overlap = 0;
    for (u32 i = 0; i < l->n; i++)
        if ((int)(l->end[i] - hs) > 0 && (int)(he - l->start[i]) > 0) overlap = 1;
    int ok = !overlap && h->n == 1 && (int)(hs - l->start[0]) > 0 && (int)(l->end[l->n - 1] - he) > 0;
    return result("prio", ok, "(nice 0 ran %u ms in %u stretch(es); the nice 5 one %s meanwhile)", ms_of(he - hs), h->n,
                  overlap ? "RAN" : "did not run");
}

static int t_wake(void)
{
    int lp = start("0", 5, 1500);
    sys_sleep(20);
    u32 worst = 0, sum = 0;
    for (int i = 0; i < 30; i++) {
        u32 t = sys_hrtime();
        sys_sleep(1);
        u32 d = sys_hrtime() - t;
        sum += d;
        if (d > worst) worst = d;
    }
    int busy = !sh->r[0].done;
    sys_wait(lp);
    int ok = busy && ms_of(worst) <= 25;
    return result("wake", ok, "(30 one-tick sleeps: worst %u ms, mean %u ms, want <= 25; the spinner %s)", ms_of(worst),
                  ms_of(sum / 30), busy ? "was busy all along" : "had ENDED");
}

static int t_inherit(void)
{
    int sp = start("s", 10, 0);
    for (int i = 0; i < 200 && !sh->ready; i++) sys_sleep(1);
    int port = sys_port_find("priot");
    if (port < 0) return result("inherit", 0, "(the server did not start)");
    int lp = start("0", 5, 1500);
    sys_sleep(10);                                  /* the spinner is running; the server waits in recv */
    struct umsg m;
    u32 t0 = sys_hrtime();
    int bad = 0;
    for (int i = 0; i < 50; i++) { m.type = 1; if (sys_call(port, &m) != 0) bad++; }
    u32 took = sys_hrtime() - t0;
    int busy = !sh->r[0].done;
    u32 after = pri_of(sp);                         /* back in recv: its own level again */
    m.type = 9;
    sys_call(port, &m);
    sys_wait(sp);
    sys_wait(lp);
    u32 mine = my_pri();
    int ok = !bad && busy && ms_of(took) < 400 && sh->serving_pri == mine && after == 24;
    return result("inherit", ok, "(50 calls in %u ms, want < 400, the spinner %s; server level %u while serving (client %u), %u after)",
                  ms_of(took), busy ? "busy all along" : "had ENDED", sh->serving_pri, mine, after);
}

/* audit G1: a port number kept from a server that ended must not reach the port that takes its slot next. The
 * server ends; a new one creates its port again (the first free slot: the same one); the old number is refused. */
static int t_stale(void)
{
    struct umsg m;
    int sp = start("s", 0, 0);
    for (int i = 0; i < 200 && !sh->ready; i++) sys_sleep(1);
    int old = sys_port_find("priot");
    if (old < 0) return result("stale", 0, "(the first server did not start)");
    m.type = 9;
    sys_call(old, &m);                              /* it ends */
    sys_wait(sp);
    sh->ready = 0;
    sp = start("s", 0, 0);
    for (int i = 0; i < 200 && !sh->ready; i++) sys_sleep(1);
    int now = sys_port_find("priot");
    m.type = 1;
    int r_old = sys_call(old, &m);
    m.type = 1;
    int r_now = now >= 0 ? sys_call(now, &m) : -1;
    m.type = 9;
    if (now >= 0) sys_call(now, &m);
    sys_wait(sp);
    int same_slot = now >= 0 && (old & 0xFF) == (now & 0xFF);
    int ok = same_slot && r_old < 0 && r_now == 0;
    return result("stale", ok, "(the slot %s; the old number %s, the new one %s)", same_slot ? "was reused" : "was NOT reused",
                  r_old < 0 ? "refused" : "REACHED the new port", r_now == 0 ? "works" : "FAILED");
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "-c")) return child(argv);
    if (argc != 2) { fprintf(2, "usage: priotest fair|prio|wake|inherit|stale\n"); return 2; }
    sh = sys_memalloc(sizeof *sh);
    if (!sh) { fprintf(2, "priotest: out of memory\n"); return 1; }
    memset(sh, 0, sizeof *sh);
    if (!strcmp(argv[1], "fair")) return t_fair();
    if (!strcmp(argv[1], "prio")) return t_prio();
    if (!strcmp(argv[1], "wake")) return t_wake();
    if (!strcmp(argv[1], "inherit")) return t_inherit();
    if (!strcmp(argv[1], "stale")) return t_stale();
    fprintf(2, "usage: priotest fair|prio|wake|inherit|stale\n");
    return 2;
}
