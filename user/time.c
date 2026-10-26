// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* time command [argument...]: run a command and print how long it took (wall-clock time, to standard error) */
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(2, "usage: time command [argument...]\n"); return 2; }
    char path[160];
    if (strchr(argv[1], '/')) snprintf(path, sizeof path, "%s", argv[1]); else snprintf(path, sizeof path, "/bin/%s", argv[1]);
    u32 t0 = sys_hrtime();
    int pid = sys_spawn(path, argv + 1, 0);
    if (pid < 0) { fprintf(2, "time: %s: %s\n", argv[1], zwhy("cannot run")); return 127; }
    int st = sys_wait(pid);             /* the command is in our job's group, the terminal's foreground (DESIGN D16) */
    u32 dt = sys_hrtime() - t0;                       /* E-clock ticks, 709379 per second on PAL */
    u32 ms = (u32)(((unsigned long long)dt * 1000) / 709379);
    fprintf(2, "\nreal\t%um%u.%03us\n", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return st;
}
