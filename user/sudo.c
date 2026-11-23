// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* sudo [-u user] command [arg...], sudo [-u user] -s | -i: run one command (or a shell) as root or as USER.
 * Set-user-id root. Allowed: root, and the members of the group "wheel" (named in its member list in /etc/group, or
 * having it as their own group). They give their OWN password, three tries (an empty one asks nothing, a locked "*"
 * never passes). Like su, the command gets the target's USER, LOGNAME, HOME, SHELL and PATH=/bin, and no IFS, ENV or
 * CDPATH; TERM and the rest stay. A command name without '/' is looked for in /bin. -i also starts in the target's
 * home. The exit status is the command's; 1 when sudo refuses, 2 for a wrong command line, 127 when the command is not
 * found.
 * After a right password sudo asks no more for 5 minutes in the same session (owner 2026-10-31, as Linux's sudo):
 * a timestamp in /var/run/sudo (root's, mode 700) per user and parent shell, holding the wall clock and the
 * seconds since boot; both must agree, so a restart or a moved clock ends it. sudo -k forgets it. */

static int usage(void) { dprintf(2, "usage: sudo [-k] [-u user] command [arg...]\n       sudo [-u user] -s | -i\n       sudo -k\n"); return 2; }

#define STAMP_DIR "/var/run/sudo"
#define STAMP_SECS 300

/* the session's timestamp file: the user and the parent (the shell sudo was typed in) */
static void stamp_path(char *buf, u32 n, u32 uid)
{
    u32 ppid = 0, me = (u32)sys_getpid();
    struct uproc pr;
    for (u32 i = 1; sys_procinfo(i, &pr) >= 0; i++)
        if (pr.state && pr.pid == me) {
            ppid = pr.ppid;
            break;
        }
    snprintf(buf, n, STAMP_DIR "/%u-%u", uid, ppid);
}

/* the directory is root's and nobody else's: else no timestamp is trusted or written */
static int stamp_dir_ok(void)
{
    struct ustat st;
    sys_mkdir("/var/run");
    sys_mkdir(STAMP_DIR);
    sys_chmod(STAMP_DIR, 0700);
    return sys_stat(STAMP_DIR, &st) == 0 && st.uid == 0 && (st.mode & 0777) == 0700 && (st.mode & 0170000) == 0040000;
}

static int stamp_fresh(u32 uid)
{
    char path[80], b[40];
    struct ustat st;
    if (!stamp_dir_ok())
        return 0;
    stamp_path(path, sizeof path, uid);
    if (sys_stat(path, &st) < 0 || st.uid != 0)
        return 0;
    int fd = sys_open(path, 0), n = fd >= 0 ? sys_read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0)
        sys_close(fd);
    if (n <= 0)
        return 0;
    b[n] = 0;
    u32 wall = 0, up = 0;
    char *q = b;
    while (*q >= '0' && *q <= '9') wall = wall * 10 + (u32)(*q++ - '0');
    if (*q++ != ' ')
        return 0;
    while (*q >= '0' && *q <= '9') up = up * 10 + (u32)(*q++ - '0');
    u32 nw = sys_time(), nu = sys_uptime();
    if (nu < up || nw < wall)
        return 0;
    u32 dw = nw - wall, du = nu - up;
    return du <= STAMP_SECS && (dw > du ? dw - du : du - dw) <= 5;
}

static void stamp_write(u32 uid)
{
    char path[80], b[40];
    if (!stamp_dir_ok())
        return;
    stamp_path(path, sizeof path, uid);
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return;
    int n = snprintf(b, sizeof b, "%u %u\n", sys_time(), sys_uptime());
    sys_write(fd, b, (u32)n);
    sys_close(fd);
    sys_chmod(path, 0600);
}

static void stamp_forget(u32 uid)
{
    char path[80];
    stamp_path(path, sizeof path, uid);
    sys_unlink(path);
}

/* the caller's own groups, as the kernel holds them (inherited: sudo is set-user-id root, which keeps them): a
 * gpasswd -a counts from the next login (or su), as on Unix */
static int in_wheel(void) { return in_group_named("wheel"); }

int main(int argc, char **argv)
{
    const char *target = "root";
    int shell = 0, home = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-u") && i + 1 < argc) target = argv[++i];
        else if (!strcmp(argv[i], "-s")) shell = 1;
        else if (!strcmp(argv[i], "-i")) shell = home = 1;
        else if (!strcmp(argv[i], "-k")) { stamp_forget(sys_getuid()); if (i + 1 >= argc) return 0; }
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else return usage();
    }
    if (shell ? i < argc : i >= argc) return usage();
    u32 ruid = sys_getuid();
    struct pw me, p;
    if (pw_byname(target, &p)) { dprintf(2, "sudo: %s: no such user\n", target); return 1; }
    if (ruid != 0) {
        if (pw_byuid(ruid, &me)) { dprintf(2, "sudo: user %u is not in /etc/passwd\n", ruid); return 1; }
        if (!in_wheel()) { dprintf(2, "sudo: %s is not in the group wheel (root can add: gpasswd -a %s wheel)\n", me.name, me.name); return 1; }
        if (me.hash[0] && !stamp_fresh(ruid)) {
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
            stamp_write(ruid);
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
    pw_initgroups("sudo", &p);          /* the target's groups, not the caller's */
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
