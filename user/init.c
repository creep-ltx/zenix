/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"

/* init: print /etc/motd, mount what /etc/fstab lists, then keep a login prompt on the virtual consoles, like a
 * Unix init with a getty on each terminal. Console 1 (/dev/tty1) gets its login at once; console N (2..10) the first
 * time it is shown (Amiga+FN: the kernel tells init through sys_waitcon, like Linux's autovt). A login that lets a user
 * in starts the shell and steps aside (login --init, DESIGN D18): the shell becomes init's child, and its end (the user
 * logged out) starts a new login on its console. /etc/autologin applies to console 1 only (login.c).
 * The servers nobody calls (network listeners such as wasabid) are init's too (DESIGN D24 7): the lines of
 * /etc/daemons are started after `ifup -a` and started again when they end, with D11's numbers. */
#define NCON 10
static int pid[NCON + 1];               /* the session on console n, its login and then its shell; 0 = none */

static void start(int n, int again)
{
    char dev[12] = "/dev/tty";
    if (n == 10) { dev[8] = '1'; dev[9] = '0'; dev[10] = 0; }
    else { dev[8] = (char)('0' + n); dev[9] = 0; }
    int fd = sys_open(dev, O_RDWR);
    if (fd < 0) { printf("init: cannot open %s\n", dev); oflush(); return; }
    if (again) fprintf(fd, "init: session ended, starting another\n");
    char *argv[] = { "/bin/login", "--init", 0 };
    int io[3] = { fd, fd, fd };
    pid[n] = sys_spawn("/bin/login", argv, io);
    if (pid[n] < 0) {                   /* said on that console and on init's own output (flushed: init never ends) */
        const char *why = zstrerror(pid[n]); if (!why) why = "failed";
        fprintf(fd, "init: cannot start /bin/login: %s\n", why);
        printf("init: cannot start /bin/login on %s: %s\n", dev, why);
        oflush();
        pid[n] = 0;
    }
    sys_close(fd);
}

/* /etc/console (D14 section 11): "colours N" (2, 4, 8 or 16; "colors" too) sets the text console's colours, read
 * once here, before anything else takes chip RAM; no file, no such line or "auto": the machine's default. Read at
 * every start of init, so a change counts from the next boot. */
static void console_setup(void)
{
    char b[512];
    int fd = sys_open("/etc/console", O_RDONLY), n = fd < 0 ? 0 : sys_read(fd, b, sizeof b - 1);
    if (fd >= 0) sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++)
            ;
        if (*e) *e++ = 0;
        char *w = l, *v;
        while (*w == ' ' || *w == '\t') w++;
        if (!*w || *w == '#') continue;
        for (v = w; *v && *v != ' ' && *v != '\t'; v++)
            ;
        if (*v) *v++ = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (strcmp(w, "colours") && strcmp(w, "colors")) { printf("init: /etc/console: unknown setting %s\n", w); continue; }
        if (!strncmp(v, "auto", 4)) continue;
        int c = atoi(v), r = c ? sys_console(CON_COLOURS, c) : -EINVAL;
        if (r == -EINVAL) printf("init: /etc/console: colours %s: 2, 4, 8 or 16 (or auto)\n", v);
        else if (r < 0) printf("init: /etc/console: colours %d: %s, staying at %d\n", c, zstrerror(r), sys_console(CON_COLOURS, 0));
        else if (r != c) printf("init: /etc/console: colours %d: this machine shows %d\n", c, r);
    }
    oflush();
}

/* /etc/network/interfaces has an "auto" line (its first word is auto): read in small pieces, init lives for ever */
static int auto_iface(void)
{
    char b[128];
    int fd = sys_open("/etc/network/interfaces", O_RDONLY), n, k = 0;   /* k: letters of "auto" seen, -1: not this line */
    if (fd < 0) return 0;
    while ((n = sys_read(fd, b, sizeof b)) > 0)
        for (int i = 0; i < n; i++) {
            char c = b[i];
            if (c == '\n') k = 0;
            else if (k == 4 && (c == ' ' || c == '\t')) { sys_close(fd); return 1; }
            else if (k >= 0 && k < 4 && c == "auto"[k]) k++;
            else if (!(k == 0 && (c == ' ' || c == '\t'))) k = -1;
        }
    sys_close(fd);
    return 0;
}

/* /etc/daemons (DESIGN D24 7): "NAME NICE PROGRAM [ARGUMENTS]" a line (svctab's layout and limits: the program text
 * at most 79 characters, six words), at most NDAEMON lines, root's and not writable by others (else ignored). Each
 * runs as root, standard input /dev/null, output and errors /dev/kmsg (the kernel log: never a console), and is
 * started again when it ends, as D11 does for services: status 0 is a stop (not again until the next boot), any other
 * end is counted and waits 0, 1, 2, 4 s before the next start, five ends within 60 s give it up; status 75 is init's
 * own "start me again at once" (no wait, but counted). The waits are an alarm whose handler only makes sys_waitcon
 * return (-EINTR, no SA_RESTART): it arms itself again one tick later, so an alarm that came just before the wait
 * still ends it; the loop cancels it after every wait. */
#define NDAEMON 4
static struct daemon {
    int pid;                            /* running; 0 none */
    unsigned char state;                        /* 0 unused, 1 runs or waits for its start, 2 stopped, 3 given up */
    unsigned char nends;                        /* ends counted in this window */
    signed char nice;
    u32 due, win;                       /* the tick of its next start; of the first end of the window */
    char name[16], text[80];
} dm[NDAEMON];
static int kmsg = -1;

static volatile int alarm_wanted;       /* set only around sys_waitcon: the handler arms again only then */
static void on_alarm(int sig) { (void)sig; if (alarm_wanted) sys_alarm(1); }

static void daemons_load(void)
{
    char chunk[128], line[128];
    int fd = sys_open("/etc/daemons", O_RDONLY), n, k = 0, nd = 0, skip = 0;
    struct ustat st;
    if (fd < 0) return;
    if (sys_fstat(fd, &st) < 0 || st.uid != 0 || (st.mode & 022)) {
        sys_close(fd);
        printf("init: /etc/daemons is not root's or others may write it: ignored\n");
        oflush();
        if ((fd = sys_open("/dev/kmsg", O_WRONLY)) >= 0) {
            fprintf(fd, "init: /etc/daemons is not root's or others may write it: ignored\n");
            sys_close(fd);
        }
        return;
    }
    if ((kmsg = sys_open("/dev/kmsg", O_WRONLY)) < 0) {
        sys_close(fd);
        printf("init: /etc/daemons: no /dev/kmsg for their output (%s): none started\n", zstrerror(kmsg));
        oflush();
        kmsg = -1;
        return;
    }
    while ((n = sys_read(fd, chunk, sizeof chunk)) > 0 || k) {
        for (int i = 0; i < n || (n <= 0 && i == 0); i++) {
            char c = n > 0 ? chunk[i] : '\n';   /* a last line without a newline */
            if (c != '\n') {
                if (k < (int)sizeof line - 1) line[k++] = c; else skip = 1;
                continue;
            }
            line[k] = 0;
            if (skip) {                         /* a long comment is still a comment */
                char *f = line;
                while (*f == ' ' || *f == '\t') f++;
                if (*f == '#') skip = 0;
            }
            k = 0;
            char *name = "(a line over 127 characters)", *prog;
            int nice, words = 0, r = skip ? -1 : svc_line(line, &name, &nice, &prog);
            skip = 0;
            if (!r) continue;
            if (r > 0)
                for (char *p = prog; *p; ) { while (*p == ' ') p++; if (*p) words++; while (*p && *p != ' ') p++; }
            if (r < 0 || strlen(name) > 15 || strlen(prog) > 79 || words > 6 || prog[0] != '/' || nice < -20 || nice > 19) {
                fprintf(kmsg, "init: /etc/daemons: line skipped: %s\n", name);
                continue;
            }
            if (nd == NDAEMON) { fprintf(kmsg, "init: /etc/daemons: more than %d daemons: %s ignored\n", NDAEMON, name); continue; }
            struct daemon *d = &dm[nd++];
            strcpy(d->name, name);
            strcpy(d->text, prog);
            d->nice = (signed char)nice;
            d->state = 1;
            d->due = sys_ticks();
        }
        if (n <= 0) break;
    }
    sys_close(fd);
}

static void daemon_start(struct daemon *d)
{
    char t[80], *argv[7];
    int n = 0;
    strcpy(t, d->text);
    for (char *p = t; *p && n < 6; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[n++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    argv[n] = 0;
    int null = sys_open("/dev/null", O_RDONLY), io[3] = { null, kmsg, kmsg };
    d->pid = null < 0 ? null : sys_spawn(argv[0], argv, io);
    if (null >= 0) sys_close(null);
    if (d->pid > 0) {
        sys_setpriority(PRIO_PROCESS, d->pid, d->nice);
        fprintf(kmsg, "init: %s: started %s (pid %d)\n", d->name, d->text, d->pid);
        return;
    }
    const char *why = zstrerror(d->pid);
    fprintf(kmsg, "init: %s: cannot start %s: %s\n", d->name, argv[0], why ? why : "failed");
    d->pid = 0;
}

/* one of them ended with status st: a stop, or an end counted against the window */
static void daemon_ended(struct daemon *d, int st)
{
    int pid = d->pid;
    char prog[80], *e;
    strcpy(prog, d->text);
    for (e = prog; *e && *e != ' '; e++)
        ;
    *e = 0;
    d->pid = 0;
    if (pid > 0 && st == 0) {
        d->state = 2;
        fprintf(kmsg, "init: %s: %s (pid %d) stopped (status 0): not started again until the next boot\n", d->name, prog, pid);
        return;
    }
    u32 now = sys_ticks();
    if (!d->nends || now - d->win >= 6000) { d->win = now; d->nends = 0; }
    d->nends++;
    if (pid > 0)
        fprintf(kmsg, "init: %s: %s (pid %d) ended, status %d%s\n", d->name, prog, pid, st,
                st == 75 ? " (start me again)" : "");
    if (d->nends >= 5) {
        d->state = 3;
        fprintf(kmsg, "init: %s: given up after 5 ends in 60 s (the next boot starts it again)\n", d->name);
        return;
    }
    static const unsigned short wait[4] = { 0, 100, 200, 400 };
    d->due = now + (st == 75 ? 0 : wait[d->nends - 1]);
}

/* start what is due; the ticks to the next start (0: none waits) */
static u32 daemons_due(void)
{
    u32 next = 0;
    for (int i = 0; i < NDAEMON; i++) {
        struct daemon *d = &dm[i];
        if (d->state != 1 || d->pid)
            continue;
        int left = (int)(d->due - sys_ticks());
        if (left <= 0) {
            daemon_start(d);
            if (!d->pid)
                daemon_ended(d, -1);        /* it could not start: counted, as D11 counts a failed start */
            i--;                            /* again: started, or waiting */
            continue;
        }
        if (!next || (u32)left < next)
            next = (u32)left;
    }
    return next;
}

int main(void)
{
    console_setup();
    int fd = sys_open("/etc/motd", 0);
    if (fd >= 0) {
        char b[256]; int n;
        while ((n = sys_read(fd, b, sizeof b)) > 0) sys_write(1, b, (u32)n);
        sys_close(fd);
    }
    int fs = sys_open("/etc/fstab", 0);                 /* mount what /etc/fstab lists (see mount(8)) */
    if (fs >= 0) {
        sys_close(fs);
        char *ma[] = { "/bin/mount", "-a", 0 };
        int mp = sys_spawn("/bin/mount", ma, 0);
        if (mp >= 0) sys_wait(mp);
    }
    if (auto_iface()) {                                 /* the network (DESIGN D19 6.3): started, never waited for */
        char *ia[] = { "/bin/ifup", "-a", 0 };
        sys_spawn("/bin/ifup", ia, 0);
    }
    daemons_load();                                     /* DESIGN D24 7: started in the loop, never waited for */
    if (kmsg >= 0) {
        struct sigaction sa = { on_alarm, 0, 0 };       /* no SA_RESTART: the alarm ends sys_waitcon */
        sigaction(SIGALRM, &sa, 0);
    }
    sys_setfg(0);
    int again = 0;
    for (;;) {
        while (!pid[1]) {                               /* console 1 always has a login */
            if (again) sys_sleep(50);                   /* a login that fails at once must not keep the CPU busy */
            start(1, again);
            if (!pid[1]) sys_sleep(500);
        }
        again = 1;
        u32 next = kmsg >= 0 ? daemons_due() : 0;
        alarm_wanted = next != 0;
        if (next) sys_alarm(next);
        int r = sys_waitcon();                          /* a child ended (taken: r is its status), or a console wants a
                                                         * login; -EINTR: a daemon's start is due (nothing was taken,
                                                         * so the loops below find nothing; never a "continue": a
                                                         * child's own exit(-4) also reads -4) */
        alarm_wanted = 0;
        sys_alarm(0);                                   /* always: its handler may have armed it again */
        if (r >= ST_CONSOLE + 1 && r <= ST_CONSOLE + NCON) {
            int n = r - ST_CONSOLE;
            if (!pid[n]) start(n, 0);
            continue;
        }
        for (int n = 1; n <= NCON; n++) {               /* which session's process ended? */
            if (!pid[n]) continue;
            int st = sys_waitnb(pid[n]);
            if (st == -2) continue;                     /* still running */
            if (st == -1) st = r;                       /* the one waitcon took (each call takes one: exactly one -1) */
            if (st & ST_HANDED) {                       /* login handed its shell to us: the session goes on in it */
                int sh = st & ~ST_HANDED;
                if (sys_waitnb(sh) == -2) { pid[n] = sh; continue; }
            }                                           /* else (or the shell already ended): the session is over */
            pid[n] = 0;
            if (n > 1) { sys_sleep(50); start(n, 1); }  /* console 1: the loop above */
        }
        for (int i = 0; i < NDAEMON; i++) {             /* a daemon's end, the same way */
            if (!dm[i].pid) continue;
            int st = sys_waitnb(dm[i].pid);
            if (st == -2) continue;
            daemon_ended(&dm[i], st == -1 ? r : st);
        }
    }
}
