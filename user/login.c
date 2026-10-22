// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* login [-f] [user]: start a session. Asks for the name and password, then runs the user's shell as that user and
 * returns when it ends (init starts login again). With -f the password is not asked (root's own call, or the
 * /etc/autologin convenience: a file naming the user to log in without asking). One session at a time. */
static int session(const struct pw *p)
{
    setenv("USER", p->name, 1); setenv("LOGNAME", p->name, 1); setenv("HOME", p->home, 1); setenv("SHELL", p->shell, 1);
    setenv("PATH", "/bin", 1);
    sys_session(p->uid, p->gid);                      /* workspace shells started later belong to this user: needs root, so before dropping it */
    sys_setgid(p->gid);
    sys_setuid(p->uid);
    sys_chdir(p->home);
    char *argv[] = { p->shell, 0 };
    int pid = sys_spawn(p->shell, argv, 0);
    if (pid < 0) { fprintf(2, "login: cannot start %s\n", p->shell); return 1; }
    sys_setfg(0);
    sys_wait(pid);
    return 0;
}

int main(int argc, char **argv)
{
    int force = 0, i = 1;
    char name[32] = "", pass[100];
    struct pw p;
    if (i < argc && !strcmp(argv[i], "-f")) { force = 1; i++; }
    if (i < argc) strncpy(name, argv[i], 31);
    if (!name[0] && !force) {                                   /* autologin: the first line of /etc/autologin */
        int fd = sys_open("/etc/autologin", O_RDONLY);
        if (fd >= 0) { int n = sys_read(fd, name, 31); sys_close(fd); if (n < 0) n = 0; name[n] = 0; char *nl = strchr(name, '\n'); if (nl) *nl = 0; if (name[0]) force = 1; }
    }
    for (int tries = 0; tries < 5; ) {
        if (!name[0]) {
            fputs("zenix login: ", 1);
            int n = 0; char c;
            while (sys_read(0, &c, 1) == 1 && c != '\n' && c != '\r') { if ((c == 8 || c == 127)) { if (n) { n--; fputs("\b \b", 1); } } else if (n < 31 && c >= ' ') { name[n++] = c; sys_write(1, &c, 1); } }
            name[n] = 0; fputs("\n", 1);
            if (!name[0]) continue;
        }
        if (pw_byname(name, &p) == 0) {
            if (force || !p.hash[0]) return session(&p);
            if (read_secret("Password: ", pass, sizeof pass) == 0 && pw_check(pass, p.hash)) return session(&p);
        } else {
            read_secret("Password: ", pass, sizeof pass);       /* same delay and questions for an unknown name */
        }
        fputs("Login incorrect\n", 1);
        sys_sleep(100);
        tries++; name[0] = 0; force = 0;
    }
    return 1;
}
