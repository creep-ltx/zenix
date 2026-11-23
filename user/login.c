// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* login [--init] [-f] [-s PROGRAM] [user]: start a session on this console. Asks for the name and password, then runs
 * the user's shell (-s, root's: PROGRAM instead, dlogin's sessions) as that user. Run by hand, it returns when the shell ends. Run by init (--init), it steps aside once the shell
 * runs (the 2 MB diet, DESIGN D18): it ends and hands the shell to init, which then waits for the session itself and
 * starts a new login when it ends; so a session holds no login process. With -f the password is not asked (root's own
 * call, or the /etc/autologin convenience on console 1: a file naming the user to log in without asking). Every console
 * has its own login (init), so several sessions can be open at once. */
static int handover;
static const char *program;             /* -s: the session's program instead of the shell (dlogin's, root only) */
/* the last update's one-line result (zup, DESIGN D15), shown once after a login on any console, then removed */
static void update_result(void)
{
    int fd = sys_open("/var/lib/zpkg/result", O_RDONLY);
    if (fd < 0) return;
    char b[256]; int n = sys_read(fd, b, sizeof b);
    sys_close(fd);
    if (n > 0) sys_write(1, b, (u32)n);
    sys_unlink("/var/lib/zpkg/result");
}

/* the machine's name (DESIGN D35): the first line of /etc/hostname, "zenix" without one */
static char host[64] = "zenix";
static void read_hostname(void)
{
    int fd = sys_open("/etc/hostname", O_RDONLY);
    if (fd < 0) return;
    char b[64];
    int n = sys_read(fd, b, sizeof b - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    int k = 0;
    while (k < n && ((b[k] >= 'a' && b[k] <= 'z') || (b[k] >= '0' && b[k] <= '9') || b[k] == '-')) k++;
    if (k && (b[k] == 0 || b[k] == '\n')) { memcpy(host, b, (u32)k); host[k] = 0; }
}

static int session(const struct pw *p)
{
    update_result();
    setenv("HOSTNAME", host, 1);
    setenv("USER", p->name, 1); setenv("LOGNAME", p->name, 1); setenv("HOME", p->home, 1); setenv("SHELL", p->shell, 1);
    setenv("PATH", "/bin", 1);
    sys_ttyctl(0, TTYC_OWNER, p->uid);  /* a pty (DESIGN D20 7.2): the user may open its terminal again (/dev/tty,
                                         * sudo's prompt); a console answers -ENOTTY, nothing to do */
    sys_setgid(p->gid);
    pw_initgroups("login", p);          /* its gid and every group listing it in /etc/group */
    sys_setuid(p->uid);
    sys_chdir(p->home);
    const char *prog = program ? program : p->shell;
    /* -s PROGRAM may carry arguments, words split at spaces (dlogin's "/bin/sh /usr/lib/demo/session": the demo
     * floppy's session, DESIGN D25 revision 3, 7.3); at most 7 */
    char pbuf[128], *argv[8] = { (char *)prog, 0 };  /* spawn only reads argv */
    if (program && strchr(program, ' ') && strlen(program) < sizeof pbuf) {
        strcpy(pbuf, program);
        int n = 0;
        for (char *w = pbuf; *w && n < 7; ) {
            while (*w == ' ') *w++ = 0;
            if (!*w) break;
            argv[n++] = w;
            while (*w && *w != ' ') w++;
        }
        argv[n] = 0;
        if (!n) argv[0] = (char *)program;   /* nothing but spaces: as given (spawn says it is not found) */
        prog = argv[0];
    }
    int pid = sys_spawn(prog, argv, 0);
    if (pid < 0) { dprintf(2, "login: cannot start %s: %s\n", prog, zwhy("failed")); return 1; }
    if (handover)                       /* init's login: the shell becomes init's child, login ends (DESIGN D18) */
        sys_exit_handing(ST_HANDED | pid, pid);
    sys_wait(pid);                      /* the shell makes its own group the terminal's foreground (DESIGN D16) */
    return 0;
}

static const char USAGE[] = "usage: login [--init] [-f] [-s PROGRAM] [user]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int force = 0, autologin = 0, i = 1;
    char name[32] = "", pass[100];
    struct pw p;
    read_hostname();
    setpgid(0, 0);                      /* a group of its own (DESIGN D16 rule 7): `kill 0` in a session never reaches init */
    if (i < argc && !strcmp(argv[i], "--init")) { handover = 1; i++; }
    if (i < argc && !strcmp(argv[i], "-f")) { force = 1; i++; }
    if (i < argc && !strcmp(argv[i], "-s")) {                   /* the session's program (dlogin, DESIGN D38 revision 3) */
        if (i + 1 >= argc) zusage(USAGE);
        if (sys_getuid() != 0) { dprintf(2, "login: -s is root's\n"); return 1; }
        program = argv[i + 1];
        i += 2;
    }
    if (i < argc && argv[i][0] == '-') zbadopt(argv[i], USAGE);
    if (i < argc) strncpy(name, argv[i], 31);
    struct ustat ts;
    int console1 = sys_fstat(0, &ts) == 0 && (ts.mode & 0170000) == 020000 && ts.ino == 1;   /* a console's ino is its number */
    if (!name[0] && !force && console1) {                       /* autologin (console 1 only): the first line of /etc/autologin */
        int fd = sys_open("/etc/autologin", O_RDONLY);
        if (fd >= 0) { int n = sys_read(fd, name, 31); sys_close(fd); if (n < 0) n = 0; name[n] = 0; char *nl = strchr(name, '\n'); if (nl) *nl = 0; if (name[0]) force = autologin = 1; }
    }
    if (console1) {                                             /* the login prompt is up on console 1: this boot works */
        struct uupdate u;                                       /* (the confirmation zenixboot waits for, DESIGN D15) */
        if (sys_update(UPD_INFO, (u32)&u, sizeof u) == 0) sys_update(UPD_NOTE, UPDNOTE_CONFIRMED, u.kvnum);
    }
    for (int tries = 0; tries < 5; ) {
        if (!name[0]) {
            pw_fputs(host, 1); pw_fputs(" login: ", 1);
            /* the tty is in cooked mode here: it echoes the name, edits the line (Backspace, ^U) and echoes the
             * Return, so login only takes the characters (echoing them too printed the name twice) */
            int n = 0; char c;
            while (sys_read(0, &c, 1) == 1 && c != '\n' && c != '\r') { if (n < 31 && c >= ' ') name[n++] = c; }
            name[n] = 0;
            if (!name[0]) continue;
        }
        if (pw_byname(name, &p) == 0) {
            if (autologin) {                                    /* the shipped images: say plainly what that means */
                struct pw r;
                if (pw_byname("root", &r) == 0 && !r.hash[0])
                    pw_fputs("login: warning: root has no password and /etc/autologin logs in without asking; set passwords with passwd and remove /etc/autologin before trusting this system\n", 1);
            }
            if (force || !p.hash[0]) return session(&p);
            if (read_secret("Password: ", pass, sizeof pass) == 0 && pw_auth(name, pass, &p)) return session(&p);
        } else if (read_secret("Password: ", pass, sizeof pass) == 0) {
            pw_auth(name, pass, &p);                            /* the same questions and time for an unknown name */
        }
        pw_fputs("Login incorrect\n", 1);
        sys_sleep(PW_FAIL_DELAY);
        tries++; name[0] = 0; force = autologin = 0;
    }
    return 1;
}
