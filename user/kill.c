// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* kill [-s NAME | -NAME | -N] pid...: end a process. -STOP and -CONT stop and continue it (job control); any other
 * signal ends it (there are no signal handlers). kill -l lists the names. */
int main(int argc, char **argv)
{
    int i = 1, op = 2, rc = 0;                           /* 0 stop, 1 continue, 2 terminate */
    if (i < argc && !strcmp(argv[i], "-l")) { printf("HUP INT QUIT KILL TERM STOP TSTP CONT\n"); return 0; }
    if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        const char *s = argv[i] + 1;
        if (!strcmp(s, "s") && i + 1 < argc) s = argv[++i];
        if (!strcmp(s, "STOP") || !strcmp(s, "TSTP") || !strcmp(s, "SIGSTOP") || !strcmp(s, "19") || !strcmp(s, "20")) op = 0;
        else if (!strcmp(s, "CONT") || !strcmp(s, "SIGCONT") || !strcmp(s, "18")) op = 1;
        i++;
    }
    if (i >= argc) { fprintf(2, "usage: kill [-s NAME] pid...\n"); return 2; }
    for (; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (pid <= 0 || (op == 2 ? sys_kill(pid) : sys_jobctl(pid, op)) < 0) { fprintf(2, "kill: %s: %s\n", argv[i], zwhy("no such process")); rc = 1; }
    }
    return rc;
}
