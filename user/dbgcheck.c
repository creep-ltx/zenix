// SPDX-License-Identifier: GPL-2.0-only
/* dbgcheck - the debugger's system call, raw (DESIGN D54 section 21; tools/zdb-test.sh). Run as root:
 *   dbgcheck          every check, one line each ending in "ok" or "FAILED"
 *   dbgcheck as1000 PROG ARG...   run PROG as uid 1000
 * and its helpers: dbgcheck sleep | sleepuser (setuid 1000 first) | asuser PID PID2 (setuid 1000, then the user's
 * checks against root's sleeper PID and the nodebug sleeper PID2). /tests/dbgtest is the program debugged. */
#include "libz.h"
typedef unsigned short u16;

static int fails;
static void ok(const char *what, int good)
{
    oprintf("dbgcheck: %s %s\n", what, good ? "ok" : "FAILED");
    oflush();
    fails += !good;
}

static struct udbgstop st;
static int dwait(int pid)
{
    st.size = sizeof st;
    return sys_debug(DBG_WAIT, pid, (u32)&st);
}

static struct udbgregs R;
static u32 pc(int pid) { R.size = sizeof R; return sys_debug(DBG_GETREGS, pid, (u32)&R) ? 0 : R.pc; }
static u16 word(int pid, u32 a)
{
    u16 w = 0;
    struct udbgmem m = { a, 2, (u32)&w };
    sys_debug(DBG_PEEK, pid, (u32)&m);
    return w;
}

static int start(char **argv, const int *stdio)    /* under the debugger: stopped at its entry */
{
    if (sys_debug(DBG_NEXT, 0, 0))
        return -1;
    int pid = sys_spawn(argv[0], argv, stdio);
    if (pid < 0 || dwait(pid) || st.why != DBGW_EXEC)
        return -1;
    return pid;
}

static int sleeper(const char *how)
{
    char *v[] = { "/tests/dbgcheck", (char *)how, 0 };
    return sys_spawn(v[0], v, 0);
}

static int asuser(int rootpid, int nodebugpid)
{
    sys_setuid(1000);
    ok("a user's attach to root's process refused", sys_debug(DBG_ATTACH, rootpid, 0) == -EPERM);
    ok("a user's attach to pid 1 refused", sys_debug(DBG_ATTACH, 1, 0) == -EPERM);
    ok("a user's attach to a nodebug process of its own id refused", sys_debug(DBG_ATTACH, nodebugpid, 0) == -EPERM);
    /* /tmp/sid: a copy of /bin/id, root's, set-user-id; /tmp/sidx the same without read for others */
    int p[2];
    sys_pipe(p);
    int io[3] = { -1, p[1], -1 };
    char *v[] = { "/tmp/sid", 0 };
    int pid = start(v, io);
    sys_close(p[1]);
    int good = pid > 0 && sys_debug(DBG_CONT, pid, 0) == 0 && dwait(pid) == 0 && st.why == DBGW_EXIT;
    char b[128];
    int n = good ? sys_read(p[0], b, sizeof b - 1) : 0;
    b[n > 0 ? n : 0] = 0;
    sys_close(p[0]);
    ok("a set-user-id program started under the debugger runs as the caller", good && strstr(b, "uid=1000") != 0);
    if (!good || !strstr(b, "uid=1000"))
        oprintf("dbgcheck: (its output: %s)\n", b);
    char *v2[] = { "/tmp/sidx", 0 };
    int e = sys_debug(DBG_NEXT, 0, 0);
    pid = e ? e : sys_spawn(v2[0], v2, 0);
    ok("a program the caller cannot read is refused under the debugger", pid == -EACCES);
    if (pid > 0)
        sys_wait(pid);
    return fails;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "sleep")) for (;;) sys_sleep(100);
    if (argc > 1 && !strcmp(argv[1], "sleepuser")) { sys_setuid(1000); for (;;) sys_sleep(100); }
    if (argc > 3 && !strcmp(argv[1], "asuser")) return asuser(atoi(argv[2]), atoi(argv[3]));
    if (argc > 2 && !strcmp(argv[1], "as1000")) {  /* run a program as uid 1000 (the crash lines are not root's) */
        sys_setuid(1000);
        int pid = sys_spawn(argv[2], argv + 2, 0);
        return pid < 0 ? 127 : sys_wait(pid);
    }

    /* the user's rules, from a child that becomes uid 1000 */
    int rs = sleeper("sleep"), us = sleeper("sleepuser");
    sys_sleep(20);
    char a1[12], a2[12];
    snprintf(a1, sizeof a1, "%d", rs);
    snprintf(a2, sizeof a2, "%d", us);
    char *av[] = { "/tests/dbgcheck", "asuser", a1, a2, 0 };
    int ap = sys_spawn(av[0], av, 0);
    int ast = ap > 0 ? sys_wait(ap) : -1;
    ok("the user's checks ran", ast == 0);
    ok("root's attach to pid 1 refused", sys_debug(DBG_ATTACH, 1, 0) == -EPERM);
    ok("root may attach to a nodebug process", sys_debug(DBG_ATTACH, us, 0) == 0 && dwait(us) == 0 &&
       st.why == DBGW_ATTACH);
    ok("a second debugger for it refused", sys_debug(DBG_ATTACH, us, 0) == -EPERM);

    /* the table: four at once, a fifth EAGAIN (us is the first) */
    int s[4];
    for (int i = 0; i < 3; i++) {
        s[i] = sleeper("sleep");
        sys_sleep(5);
        sys_debug(DBG_ATTACH, s[i], 0);
    }
    s[3] = rs;
    ok("a fifth traced process: EAGAIN", sys_debug(DBG_ATTACH, rs, 0) == -EAGAIN);

    /* pokes outside the target: the kernel's memory, and the debugger's own */
    u32 x = 0;
    struct udbgmem m = { 0x2000, 4, (u32)&x };
    ok("a poke into the kernel's memory: EFAULT", sys_debug(DBG_POKE, us, (u32)&m) == -EFAULT);
    m.addr = (u32)&fails;
    ok("a poke into another process's memory: EFAULT", sys_debug(DBG_POKE, us, (u32)&m) == -EFAULT);
    m.addr = 0x2000;
    ok("a peek at the kernel's memory: EFAULT", sys_debug(DBG_PEEK, us, (u32)&m) == -EFAULT);

    /* SIGCONT from someone else leaves a debugger's stop */
    kill(us, SIGCONT);
    sys_sleep(10);
    ok("SIGCONT does not end a debugger's stop", dwait(us) == 0 && st.why == DBGW_ATTACH);
    /* detach: it runs on; kill the sleepers */
    for (int i = 0; i < 3; i++)
        sys_debug(DBG_DETACH, s[i], 0);
    sys_debug(DBG_DETACH, us, 0);
    ok("a detached process runs on", dwait(us) == -ESRCH && kill(us, 0) == 0);
    for (int i = 0; i < 3; i++) kill(s[i], SIGKILL);
    kill(us, SIGKILL);
    kill(rs, SIGKILL);
    for (int i = 0; i < 3; i++) sys_wait(s[i]);
    sys_wait(us);
    sys_wait(rs);

    /* a raw step on TRAP #0: no panic, then the program ends as without a debugger */
    char *tv[] = { "/tests/dbgtest", "trap", 0 };
    int t = start(tv, 0), k = 0, good = t > 0;
    while (good && k++ < 2000 && word(t, pc(t)) != 0x4E40)
        good = sys_debug(DBG_STEP, t, 0) == 0 && dwait(t) == 0 && st.why == DBGW_STEP;
    u32 at = pc(t);
    good = good && word(t, at) == 0x4E40 && sys_debug(DBG_STEP, t, 0) == 0 && dwait(t) == 0 && st.why == DBGW_STEP;
    ok("a raw step over TRAP #0 stops after it (one or two instructions)", good && st.pc > at && st.pc <= at + 16);
    if (!good)
        oprintf("dbgcheck: (%d steps, pc 0x%x)\n", k, pc(t));
    good = good && sys_debug(DBG_CONT, t, 0) == 0 && dwait(t) == 0 && st.why == DBGW_EXIT && st.status == 4;
    ok("... and the program ends with its own status (4)", good);
    if (t > 0)
        sys_wait(t);

    /* a step asked of a target stopped in a blocking read; detached while it blocks again: it runs on */
    int p[2];
    sys_pipe(p);
    int io[3] = { p[0], -1, -1 };
    char *wv[] = { "/tests/dbgtest", "wait", 0 };
    t = start(wv, io);
    sys_close(p[0]);
    good = t > 0 && sys_debug(DBG_CONT, t, 0) == 0;
    sys_sleep(20);                                  /* it prints "ready" and blocks in its read */
    good = good && sys_debug(DBG_STOP, t, 0) == 0 && dwait(t) == 0 && st.why == DBGW_STOP;
    ok("a target blocked in a system call stops; its registers read", good && pc(t) != 0);
    good = good && sys_debug(DBG_STEP, t, 0) == 0;
    sys_sleep(10);
    good = good && sys_debug(DBG_DETACH, t, 0) == 0;
    sys_write(p[1], "x\n", 2);
    sys_close(p[1]);
    int ws = t > 0 ? sys_wait(t) : -1;
    ok("a step left pending at a detach: the program runs on to its end (status 3)", good && ws == 3);
    oprintf("dbgcheck: done, %d failed\n", fails);
    oflush();
    return fails != 0;
}
