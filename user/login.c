// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* login [-f] [user]: start a session on this console. Asks for the name and password, then runs the user's shell as
 * that user and returns when it ends (init starts login again). With -f the password is not asked (root's own call,
 * or the /etc/autologin convenience on console 1: a file naming the user to log in without asking). Every console has
 * its own login (init), so several sessions can be open at once. */
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

static int session(const struct pw *p)
{
    update_result();
    setenv("USER", p->name, 1); setenv("LOGNAME", p->name, 1); setenv("HOME", p->home, 1); setenv("SHELL", p->shell, 1);
    setenv("PATH", "/bin", 1);
    sys_setgid(p->gid);
    sys_setuid(p->uid);
    sys_chdir(p->home);
    char *argv[] = { (char *)p->shell, 0 };          /* spawn only reads argv */
    int pid = sys_spawn(p->shell, argv, 0);
    if (pid < 0) { fprintf(2, "login: cannot start %s: %s\n", p->shell, zwhy("failed")); return 1; }
    sys_setfg(0);
    sys_wait(pid);
    return 0;
}

int main(int argc, char **argv)
{
    int force = 0, autologin = 0, i = 1;
    char name[32] = "", pass[100];
    struct pw p;
    if (i < argc && !strcmp(argv[i], "-f")) { force = 1; i++; }
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
            fputs("zenix login: ", 1);
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
                    fputs("login: warning: root has no password and /etc/autologin logs in without asking; set passwords with passwd and remove /etc/autologin before trusting this system\n", 1);
            }
            if (force || !p.hash[0]) return session(&p);
            if (read_secret("Password: ", pass, sizeof pass) == 0 && pw_check(pass, p.hash)) return session(&p);
        } else {
            read_secret("Password: ", pass, sizeof pass);       /* same delay and questions for an unknown name */
        }
        fputs("Login incorrect\n", 1);
        sys_sleep(100);
        tries++; name[0] = 0; force = autologin = 0;
    }
    return 1;
}
