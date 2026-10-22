// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* timeout seconds command [argument...]: run a command and kill it if it is still running after the time. Exit status 124
 * if it had to be killed, else the command's own. */
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(2, "usage: timeout seconds command [argument...]\n"); return 2; }
    u32 limit = (u32)atoi(argv[1]) * 100;                 /* 100 ticks a second */
    char path[160];
    if (strchr(argv[2], '/')) snprintf(path, sizeof path, "%s", argv[2]); else snprintf(path, sizeof path, "/bin/%s", argv[2]);
    int pid = sys_spawn(path, argv + 2, 0);
    if (pid < 0) { fprintf(2, "timeout: %s: %s\n", argv[2], zwhy("cannot run")); return 127; }
    sys_setfg(pid);
    u32 start = sys_ticks();
    for (;;) {
        int st = sys_waitnb(pid);
        if (st != -2) { sys_setfg(0); return st; }
        if (sys_ticks() - start >= limit) { sys_kill(pid); sys_wait(pid); sys_setfg(0); return 124; }
        sys_sleep(5);
    }
}
