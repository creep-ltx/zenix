// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* watch [-n seconds] command [argument...]: run a command again and again, clearing the screen each time (default every
 * 2 seconds). Ctrl+C ends it. */
int main(int argc, char **argv)
{
    int secs = 2, i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) { secs = atoi(argv[i + 1]); i += 2; }
    if (secs < 1) secs = 1;
    if (i >= argc) { fprintf(2, "usage: watch [-n seconds] command [argument...]\n"); return 2; }
    u32 len = 1;
    for (int k = i; k < argc; k++) len += strlen(argv[k]) + 1;
    char *cmd = xmalloc(len); cmd[0] = 0;                   /* the whole command, however long: nothing is cut */
    for (int k = i; k < argc; k++) { if (k > i) strcat(cmd, " "); strcat(cmd, argv[k]); }
    for (;;) {
        printf("\033[2J\033[HEvery %ds: %s\n\n", secs, cmd);
        char *sh[] = { "/bin/sh", "-c", cmd, 0 };
        int pid = sys_spawn("/bin/sh", sh, 0);
        if (pid < 0) { fprintf(2, "watch: cannot run sh\n"); return 1; }
        sys_wait(pid);                  /* the command is in watch's group (the job's): Ctrl+C ends both (DESIGN D16) */
        sys_sleep((u32)secs * 100);
    }
}
