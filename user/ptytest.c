// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* ptytest: checks of the pseudo-terminals (DESIGN D20 7.2, D38 step 1): names and inode numbers, the line discipline
 * through a master (echo, editing, cooked and raw, ^C to the foreground), the window size and SIGWINCH, the owner,
 * hang-up (SIGHUP, end of file, -EIO, POLLHUP), numbers not reused while a process names the pty, and an interactive
 * shell on a pty running echo and ls. Each check prints "ptytest NAME: PASS (...)" or "... FAIL (...)"; the exit
 * status is the number of failures. Run as root (tools/pty-test.sh). The children are this program again, started
 * with an argument that begins with '_'. */

static char self[64];
static int fails;

static void result(const char *name, int ok, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dprintf(1, "ptytest %s: %s (%s)\n", name, ok ? "PASS" : "FAIL", buf);
    if (!ok)
        fails++;
}

static int spawn_on(const char *a1, const char *a2, int fd)
{
    char *argv[4] = { self, (char *)a1, (char *)a2, 0 };
    int io[3] = { fd, fd, fd };
    return sys_spawn(self, argv, fd >= 0 ? io : 0);
}

/* what the master read so far (for the checks of a conversation); kept whole, the printable part shown on failure */
static char got[2048];
static u32 ngot;

/* read the master until NEEDLE has arrived (at most SECS seconds) -> 1 found */
static int until(int m, const char *needle, int secs)
{
    u32 t0 = (u32)sys_ticks();
    for (;;) {
        got[ngot] = 0;
        if (strstr(got, needle))
            return 1;
        int left = secs * 100 - (int)((u32)sys_ticks() - t0);
        if (left <= 0)
            return 0;
        struct pollfd p = { m, POLLIN, 0 };
        if (poll(&p, 1, left * 10) <= 0)
            continue;
        if (ngot >= sizeof got - 1) {           /* keep the last half */
            memmove(got, got + sizeof got / 2, ngot - sizeof got / 2);
            ngot -= sizeof got / 2;
        }
        int n = sys_read(m, got + ngot, sizeof got - 1 - ngot);
        if (n <= 0)
            return 0;
        ngot += (u32)n;
    }
}
static const char *shown(void)          /* the tail of what was read, control bytes as '.' */
{
    static char b[64];
    u32 s = ngot > 60 ? ngot - 60 : 0, k = 0;
    for (u32 i = s; i < ngot; i++)
        b[k++] = got[i] >= 32 && got[i] < 127 ? got[i] : '.';
    b[k] = 0;
    return b;
}
static void put(int m, const char *s) { sys_write(m, s, strlen(s)); }

static int pty_of(int m)                /* N of /dev/pts/N from the master, -1 */
{
    const char *n = ptsname(m);
    return n ? n[9] - '0' : -1;
}

/* ---- the children (on a pty's slave as 0, 1 and 2) ---- */

static volatile int flag;
static void on_sig(int sig) { flag = sig; }
static void wait_flag(void)
{
    for (int i = 0; i < 300 && !flag; i++)
        sys_sleep(1);
}

static int child_talk(void)
{
    char b[64];
    sys_setfg(sys_getpid());            /* the pty's foreground: SIGWINCH and ^C come here */
    dprintf(1, "ready\n");
    int n = sys_read(0, b, sizeof b - 1);
    b[n > 0 ? n : 0] = 0;
    dprintf(1, "line:%s", b);           /* "hello\n" */
    n = sys_read(0, b, sizeof b - 1);
    b[n > 0 ? n : 0] = 0;
    dprintf(1, "line:%s", b);           /* "ac\n": the backspace edited the line */
    int sz = sys_ttyinfo();
    dprintf(1, "size:%dx%d\n", sz >> 16, sz & 0xFFFF);
    signal(SIGWINCH, on_sig);
    dprintf(1, "winch-ready\n");
    wait_flag();
    sz = sys_ttyinfo();
    struct winsize w;
    ioctl(0, TIOCGWINSZ, &w);
    dprintf(1, "winch:%d:%dx%d:%dx%d\n", flag, sz >> 16, sz & 0xFFFF, w.ws_row, w.ws_col);
    sys_ttymode(TTY_RAW);
    dprintf(1, "raw-ready\n");
    n = 0;
    while (n < 3) {
        int k = sys_read(0, b + n, (u32)(3 - n));
        if (k <= 0)
            break;
        n += k;
    }
    b[n] = 0;
    dprintf(1, "raw:%s\n", b);
    sys_ttymode(0);
    flag = 0;
    signal(SIGINT, on_sig);
    dprintf(1, "int-ready\n");
    wait_flag();
    dprintf(1, "sig:%d\n", flag);
    dprintf(1, "hup-ready\n");
    n = sys_read(0, b, sizeof b);       /* the master closes: SIGHUP's default action ends us here */
    return 50 + n;
}

/* a process that names the pty as its controlling tty and outlives the hang-up (SIGHUP ignored) */
static int child_linger(void)
{
    signal(SIGHUP, SIG_IGN);
    dprintf(1, "linger\n");
    for (int i = 0; i < 6000; i++)      /* at most 60 s; the test kills it sooner */
        sys_sleep(1);
    return 0;
}

/* as user 1000: open the slave named, then a pty of its own and its owner op */
static int child_user(const char *path)
{
    sys_setuid(1000);
    int fd = sys_open(path, O_RDWR);
    if (fd < 0)
        return zerrno;
    sys_close(fd);
    return 0;
}
static int child_own(void)
{
    sys_setuid(1000);
    int m = posix_openpt(O_RDWR);
    if (m < 0)
        return 1;
    int s = sys_open(ptsname(m), O_RDWR);           /* the opener owns it */
    if (s < 0)
        return 2;
    int r = sys_ttyctl(m, TTYC_OWNER, 0);
    return r == -EPERM ? 0 : 3;
}

/* ---- the checks ---- */

static void names(void)
{
    struct ustat st;
    int m = posix_openpt(O_RDWR);
    result("open", m >= 0, "posix_openpt %d", m);
    if (m < 0)
        return;
    const char *n = ptsname(m);
    result("ptsname", n && !strcmp(n, "/dev/pts/0"), "%s", n ? n : "none");
    int r = sys_fstat(m, &st);
    result("master-ino", r == 0 && st.ino == 0x100 && (st.mode & 0170000) == 020000, "ino %u", st.ino);
    int s = sys_open("/dev/pts/0", O_RDWR);
    r = s >= 0 ? sys_fstat(s, &st) : s;
    result("slave-ino", r == 0 && st.ino == 11, "fd %d ino %u (never a console's 1..10)", s, st.ino);
    result("isatty", sys_isatty(m) == 1 && sys_isatty(s) == 1, "%d %d", sys_isatty(m), sys_isatty(s));
    struct winsize w;
    r = ioctl(s, TIOCGWINSZ, &w);
    result("default-size", r == 0 && w.ws_row == 24 && w.ws_col == 80, "%ux%u", w.ws_row, w.ws_col);
    int m2 = posix_openpt(O_RDWR);
    result("second", pty_of(m2) == 1, "pts/%d while pts/0 is open", pty_of(m2));
    sys_close(m2);
    m2 = posix_openpt(O_RDWR);
    result("freed", pty_of(m2) == 1, "pts/%d again after its close", pty_of(m2));
    sys_close(m2);
    result("no-such-slave", sys_open("/dev/pts/5", O_RDWR) == -ENOENT, "pts/5 never opened: ENOENT");
    sys_close(s);                       /* the slave closed, the master open: -EIO, POLLHUP */
    char b[8];
    r = sys_read(m, b, sizeof b);
    struct pollfd p = { m, POLLIN, 0 };
    poll(&p, 1, 0);
    result("master-eio", r == -EIO && (p.revents & POLLHUP), "read %d, revents %x", r, p.revents);
    r = sys_write(m, "x\n", 2);
    result("master-write-eio", r == -EIO, "the session ended: a write to the master is EIO, never a wait: %d", r);
    sys_close(m);
    result("hung-up-slave", sys_open("/dev/pts/0", O_RDWR) == -ENOENT, "pts/0 freed with its master");
}

static void talk(void)
{
    int m = posix_openpt(O_RDWR);
    int s = sys_open(ptsname(m), O_RDWR);
    int pid = spawn_on("_talk", 0, s);
    sys_close(s);
    ngot = 0;
    result("onlcr", until(m, "ready\r\n", 10), "the child's LF arrives as CR LF: %s", shown());
    put(m, "hello\n");
    result("echo", until(m, "hello\r\n", 10), "the typed line is echoed: %s", shown());
    result("cooked", until(m, "line:hello\r\n", 10), "the child read the whole line: %s", shown());
    put(m, "ab\bc\n");
    result("erase", until(m, "line:ac\r\n", 10), "backspace edited the line: %s", shown());
    result("size", until(m, "size:24x80", 10), "ttyinfo answers the pty's size: %s", shown());
    until(m, "winch-ready", 10);
    struct winsize w = { 30, 100, 0, 0 };
    int r = ioctl(m, TIOCSWINSZ, &w);
    result("winch", r == 0 && until(m, "winch:28:30x100:30x100", 10), "set on the master: SIGWINCH, ttyinfo, TIOCGWINSZ: %s",
           shown());
    until(m, "raw-ready\r\n", 10);
    u32 at = ngot;
    put(m, "xyz");
    int ok = until(m, "raw:xyz", 10);
    got[ngot] = 0;
    char *e = strstr(got + at, "raw:xyz");
    int echoed = 0;
    for (char *q = got + at; e && q < e; q++)
        echoed |= *q == 'x';
    result("raw", ok && e && !echoed, "three bytes without a newline, no echo: %s", shown());
    until(m, "int-ready", 10);
    put(m, "\003");
    result("intr", until(m, "sig:2", 10), "^C from the master: SIGINT to the foreground: %s", shown());
    until(m, "hup-ready", 10);
    sys_close(m);
    int st = sys_wait(pid);
    result("hangup", st == 128 + SIGHUP, "the master's close: the child ended by SIGHUP, status %d", st);
}

static void owner(void)
{
    int m = posix_openpt(O_RDWR);
    const char *n = ptsname(m);
    char path[16];
    strcpy(path, n ? n : "/dev/pts/9");
    int st = sys_wait(spawn_on("_user", path, -1));
    result("owner-refused", st == EACCES, "user 1000 opens root's pty: %d", st);
    int r = sys_ttyctl(m, TTYC_OWNER, 1000);
    st = sys_wait(spawn_on("_user", path, -1));
    result("owner-set", r == 0 && st == 0, "after TTYC_OWNER 1000: %d (op %d)", st, r);
    st = sys_wait(spawn_on("_own", 0, -1));
    result("owner-user", st == 0, "a user opens a pty of its own; TTYC_OWNER is root's: %d", st);
    r = sys_ttyctl(0, TTYC_SWINSZ, (u32)&(struct winsize){ 10, 10, 0, 0 });
    result("console-size", r == -EINVAL, "a console's size is the screen's: %d", r);
    sys_close(m);
}

static int alive(int pid)
{
    struct uproc p;
    for (u32 i = 0; i < 64; i++)
        if (sys_procinfo(i, &p) == 0 && p.state && p.state != 5 && (int)p.pid == pid)
            return 1;
    return 0;
}

static void reuse(void)
{
    int m = posix_openpt(O_RDWR);
    int n = pty_of(m);
    int s = sys_open(ptsname(m), O_RDWR);
    int pid = spawn_on("_linger", 0, s);
    sys_close(s);
    ngot = 0;
    until(m, "linger", 10);
    sys_close(m);                       /* hung up; the lingering child still names it */
    sys_sleep(20);
    int m2 = posix_openpt(O_RDWR);
    int n2 = pty_of(m2);
    result("no-reuse", alive(pid) && n2 >= 0 && n2 != n, "pts/%d still named by pid %d: the next is pts/%d", n, pid, n2);
    sys_close(m2);
    sys_killsig(pid, SIGKILL);
    sys_wait(pid);
    m2 = posix_openpt(O_RDWR);
    n2 = pty_of(m2);
    result("reuse", n2 == n, "after it ended pts/%d is free again: pts/%d", n, n2);
    sys_close(m2);
}

static int find_named(const char *name)
{
    struct uproc p;
    for (u32 i = 0; i < 64; i++)
        if (sys_procinfo(i, &p) == 0 && p.state && p.state != 5 && !strcmp(p.name, name))
            return (int)p.pid;
    return 0;
}

static void shell(void)
{
    int m = posix_openpt(O_RDWR);
    int s = sys_open(ptsname(m), O_RDWR);
    char *argv[] = { "/bin/sh", 0 };
    int io[3] = { s, s, s };
    int before = find_named("sleep");
    int pid = sys_spawn("/bin/sh", argv, io);
    sys_close(s);
    ngot = 0;
    put(m, "echo hello-$((6*7))\n");
    result("sh-echo", until(m, "hello-42", 20), "an interactive shell on the pty: %s", shown());
    put(m, "ls /\n");
    int ok = until(m, "etc", 20) && until(m, "bin", 5);
    result("sh-ls", ok, "ls / through the pty: %s", shown());
    put(m, "sleep 300 &\n");
    until(m, "[1]", 10);
    sys_sleep(50);
    int job = find_named("sleep");
    sys_close(m);
    int st = sys_wait(pid);
    sys_sleep(50);
    result("sh-hup", st == 128 + SIGHUP && job && job != before && !alive(job),
           "the master's close: the shell ended by SIGHUP (%d), its job %d ended too", st, job);
}

static const char USAGE[] = "usage: ptytest\n       ptytest size\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    strncpy(self, argv[0], sizeof self - 1);
    if (argc > 1 && argv[1][0] == '_') {
        if (!strcmp(argv[1], "_talk")) return child_talk();
        if (!strcmp(argv[1], "_linger")) return child_linger();
        if (!strcmp(argv[1], "_user")) return child_user(argc > 2 ? argv[2] : "");
        if (!strcmp(argv[1], "_own")) return child_own();
        return 99;
    }
    if (argc > 1 && !strcmp(argv[1], "size")) {    /* the terminal's size (TIOCGWINSZ), as stty size prints it */
        struct winsize w;
        if (ioctl(0, TIOCGWINSZ, &w) < 0) { dprintf(2, "ptytest: size: not a terminal\n"); return 1; }
        dprintf(1, "%d %d\n", w.ws_row, w.ws_col);
        return 0;
    }
    if (self[0] != '/')
        strcpy(self, "/bin/ptytest");
    names();
    talk();
    owner();
    reuse();
    shell();
    dprintf(1, "ptytest: %d failed\n", fails);
    return fails;
}
