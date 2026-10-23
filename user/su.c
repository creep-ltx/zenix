// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* su [-] [user]: run a shell as another user (root by default). Set-user-id root. Root needs no password; others
 * give the target's password. The shell gets the target's USER, LOGNAME, HOME, SHELL and PATH=/bin, and no IFS;
 * with - it also starts in the target's home directory. */
int main(int argc, char **argv)
{
    int login_env = 0, i = 1;
    const char *target = "root";
    if (i < argc && !strcmp(argv[i], "-")) { login_env = 1; i++; }
    if (i < argc) target = argv[i];
    struct pw p;
    if (pw_byname(target, &p)) { fprintf(2, "su: %s: no such user\n", target); return 1; }
    if (sys_getuid() != 0 && p.hash[0]) {
        char pass[100];
        if (read_secret("Password: ", pass, sizeof pass) || !pw_check(pass, p.hash)) { sys_sleep(100); fprintf(2, "su: wrong password\n"); return 1; }
    }
    sys_setgid(p.gid);
    if (sys_setuid(p.uid) < 0) { fprintf(2, "su: not permitted\n"); return 1; }
    /* the target's environment, not the caller's: a PATH, HOME or IFS the caller set must not steer a shell that runs
     * as someone else (as root: the caller's ~/.shrc or their own programs first in PATH) */
    setenv("USER", p.name, 1); setenv("LOGNAME", p.name, 1); setenv("HOME", p.home, 1); setenv("SHELL", p.shell, 1);
    setenv("PATH", "/bin", 1);
    unsetenv("IFS"); unsetenv("ENV"); unsetenv("CDPATH");
    if (login_env) sys_chdir(p.home);
    char *av[] = { p.shell, 0 };
    int pid = sys_spawn(p.shell, av, 0);
    if (pid < 0) { fprintf(2, "su: cannot start %s\n", p.shell); return 1; }
    sys_setfg(0);
    return sys_wait(pid);
}
