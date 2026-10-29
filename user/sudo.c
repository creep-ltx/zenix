// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* sudo [-u user] command [arg...], sudo [-u user] -s | -i: run one command (or a shell) as root or as USER.
 * Set-user-id root. Allowed: root, and the members of the group "wheel" (named in its member list in /etc/group, or
 * having it as their own group). They give their OWN password, three tries (an empty one asks nothing, a locked "*"
 * never passes). Like su, the command gets the target's USER, LOGNAME, HOME, SHELL and PATH=/bin, and no IFS, ENV or
 * CDPATH; TERM and the rest stay. A command name without '/' is looked for in /bin. -i also starts in the target's
 * home. The exit status is the command's; 1 when sudo refuses, 127 when the command is not found. */

static int usage(void) { dprintf(2, "usage: sudo [-u user] command [arg...]\n       sudo [-u user] -s | -i\n"); return 1; }

static int in_wheel(const struct pw *me)
{
    struct gr g;
    if (gr_byname("wheel", &g)) return 0;
    return me->gid == g.gid || gr_has(g.members, me->name);
}

int main(int argc, char **argv)
{
    const char *target = "root";
    int shell = 0, home = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-u") && i + 1 < argc) target = argv[++i];
        else if (!strcmp(argv[i], "-s")) shell = 1;
        else if (!strcmp(argv[i], "-i")) shell = home = 1;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else return usage();
    }
    if (shell ? i < argc : i >= argc) return usage();
    u32 ruid = sys_getuid();
    struct pw me, p;
    if (pw_byname(target, &p)) { dprintf(2, "sudo: %s: no such user\n", target); return 1; }
    if (ruid != 0) {
        if (pw_byuid(ruid, &me)) { dprintf(2, "sudo: user %u is not in /etc/passwd\n", ruid); return 1; }
        if (!in_wheel(&me)) { dprintf(2, "sudo: %s is not in the group wheel (root can add: gpasswd -a %s wheel)\n", me.name, me.name); return 1; }
        if (me.hash[0]) {
            int ok = 0;
            for (int t = 0; t < 3 && !ok; t++) {
                char pass[100], prompt[48];
                snprintf(prompt, sizeof prompt, "[sudo] password for %s: ", me.name);
                if (read_secret(prompt, pass, sizeof pass)) break;
                ok = pw_check(pass, me.hash);
                memset(pass, 0, sizeof pass);
                if (!ok) { sys_sleep(100); dprintf(2, "sudo: wrong password\n"); }
            }
            if (!ok) return 1;
        }
    }
    char path[64];
    const char *cmd = shell ? p.shell : argv[i];
    if (!strchr(cmd, '/')) {
        if (strlen(cmd) + 6 > sizeof path) { dprintf(2, "sudo: %s: command not found\n", cmd); return 127; }
        strcpy(path, "/bin/"); strcat(path, cmd); cmd = path;
    }
    struct ustat st;
    if (sys_stat(cmd, &st) < 0) { dprintf(2, "sudo: %s: command not found\n", shell ? cmd : argv[i]); return 127; }
    sys_setgid(p.gid);
    if (sys_setuid(p.uid) < 0) { dprintf(2, "sudo: not permitted (is sudo set-user-id root?)\n"); return 1; }
    /* the target's environment, not the caller's (see su.c) */
    setenv("USER", p.name, 1); setenv("LOGNAME", p.name, 1); setenv("HOME", p.home, 1); setenv("SHELL", p.shell, 1);
    setenv("PATH", "/bin", 1);
    unsetenv("IFS"); unsetenv("ENV"); unsetenv("CDPATH");
    if (home) sys_chdir(p.home);
    char *sav[] = { p.shell, 0 };
    char **av = shell ? sav : argv + i;
    int pid = sys_spawn(cmd, av, 0);
    if (pid < 0) { dprintf(2, "sudo: cannot start %s: %s\n", cmd, zwhy("error")); return 1; }
    int rc = sys_wait(pid);             /* the command is in our job's group: ^C reaches it (DESIGN D16) */
    return rc;
}
