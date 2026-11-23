// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
static const char USAGE[] = "usage: time command [argument...]\n";
/* time command [argument...]: run a command and print how long it took (wall-clock time, to standard error) */
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    if (argc > 1 && !strcmp(argv[1], "--")) argv++, argc--;
    else if (argc > 1 && argv[1][0] == '-') zbadopt(argv[1], USAGE);
    if (argc < 2) zusage(USAGE);
    char b[ZPATH_MAX], *path = argv[1];                     /* with a slash as it is: the kernel judges its length */
    if (!strchr(argv[1], '/')) {
        if (strlen(argv[1]) + 6 > sizeof b) { dprintf(2, "time: %s: file name too long\n", argv[1]); return 127; }
        strcpy(b, "/bin/"); strcat(b, argv[1]); path = b;
    }
    u32 t0 = sys_hrtime();
    int pid = sys_spawn(path, argv + 1, 0);
    if (pid < 0) { dprintf(2, "time: %s: %s\n", argv[1], zwhy("cannot run")); return 127; }
    int st = sys_wait(pid);             /* the command is in our job's group, the terminal's foreground (DESIGN D16) */
    u32 dt = sys_hrtime() - t0;                       /* E-clock ticks, 709379 per second on PAL */
    u32 ms = (u32)(((unsigned long long)dt * 1000) / 709379);
    dprintf(2, "\nreal\t%um%u.%03us\n", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return st;
}
