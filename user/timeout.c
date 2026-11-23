// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
static const char USAGE[] = "usage: timeout [-s SIGNAL] seconds command [argument...]\n";
/* timeout [-s SIGNAL] seconds command [argument...]: run a command and send it SIGNAL (SIGTERM if none is named) if it
 * is still running after the time. Exit status 124 if the time ran out, else the command's own. */
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int i = 1, sig = SIGTERM;
    if (i + 1 < argc && !strcmp(argv[i], "-s")) {
        if ((sig = zsig_num(argv[i + 1])) <= 0) { dprintf(2, "timeout: %s: invalid signal\n", argv[i + 1]); return 2; }
        i += 2;
    }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    else if (i < argc && argv[i][0] == '-') zbadopt(argv[i], USAGE);
    if (argc - i < 2) zusage(USAGE);
    u32 limit = (u32)atoi(argv[i]) * 100;                 /* 100 ticks a second */
    char path[ZPATH_MAX];
    if (!path_find(argv[i + 1], path, sizeof path)) { dprintf(2, "timeout: %s: command not found\n", argv[i + 1]); return 127; }
    int pid = sys_spawn(path, argv + i + 1, 0);           /* in our job's group, the terminal's foreground (DESIGN D16) */
    if (pid < 0) { dprintf(2, "timeout: %s: %s\n", argv[i + 1], zwhy("cannot run")); return 127; }
    u32 start = sys_ticks();
    for (;;) {
        int st = sys_waitnb(pid);
        if (st != -2) return st;
        if (sys_ticks() - start >= limit) {
            kill(pid, sig);
            if (sig != SIGKILL && sig != SIGTERM) kill(pid, SIGCONT);   /* a stopped one must see it too */
            sys_wait(pid);
            return 124;
        }
        sys_sleep(5);
    }
}
