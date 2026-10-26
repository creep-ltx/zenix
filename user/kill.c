// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* kill [-s NAME | -NAME | -N] pid...: send a signal (DESIGN D16), SIGTERM if none is named. A pid below -1 is a process
 * group (-g), 0 the caller's own group; -0 only checks that the process exists. kill -l lists the names; kill -l N
 * names signal N (or an exit status 128 + N). The shell's builtin kill does the same and also takes %job. */
int main(int argc, char **argv)
{
    int i = 1, sig = SIGTERM, rc = 0;
    if (i < argc && !strcmp(argv[i], "-l")) {
        if (i + 1 < argc) {
            int k = atoi(argv[i + 1]);
            if (k > 128) k -= 128;
            const char *n = zsig_name(k);
            if (!n) { fprintf(2, "kill: %s: invalid signal\n", argv[i + 1]); return 1; }
            printf("%s\n", n);
            return 0;
        }
        for (int k = 1; k < NSIG; k++) printf("%s%s", zsig_name(k), k + 1 < NSIG ? " " : "\n");
        return 0;
    }
    if (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--")) {
        const char *s = argv[i] + 1;
        if (!strcmp(s, "s") && i + 1 < argc) s = argv[++i];
        if ((sig = zsig_num(s)) < 0) { fprintf(2, "kill: %s: invalid signal\n", s); return 2; }
        i++;
    }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    if (i >= argc) { fprintf(2, "usage: kill [-s SIGNAL | -SIGNAL] pid...\n"); return 2; }
    for (; i < argc; i++) {
        char *e;
        int pid = (int)strtol(argv[i], &e, 10);
        if (*e || !argv[i][0]) { fprintf(2, "kill: %s: not a process ID\n", argv[i]); rc = 1; continue; }
        if (kill(pid, sig) < 0) { fprintf(2, "kill: %s: %s\n", argv[i], zerrno == EPERM ? zstrerror(EPERM) : zwhy("no such process")); rc = 1; }
    }
    return rc;
}
