// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* nice [-n N | -N] [command [argument...]]: run a command at a niceness N higher than this one's (default 10), on the
 * Unix scale -20 (most favoured) .. 19 (DESIGN D10). Without a command: print the niceness. A negative N (a raise)
 * needs root. */
static int num(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end) return 0;
    *out = v < -40 ? -40 : v > 40 ? 40 : (int)v;
    return 1;
}

static const char USAGE[] = "usage: nice [-n N] [command [argument...]]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int i = 1, inc = 10;
    if (i < argc && !strcmp(argv[i], "-n")) {
        if (i + 1 >= argc || !num(argv[i + 1], &inc)) { dprintf(2, "nice: -n needs a number\n"); return 125; }
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--")) {
        if (!num(argv[i] + 1, &inc)) zbadopt(argv[i], USAGE);
        i++;
    }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    zerrno = 0;
    int now = getpriority(PRIO_PROCESS, 0);
    if (now == -1 && zerrno) { dprintf(2, "nice: %s\n", zwhy("cannot read the niceness")); return 125; }
    if (i >= argc) { dprintf(1, "%d\n", now); return 0; }
    int want = now + inc;
    want = want < -20 ? -20 : want > 19 ? 19 : want;
    if (setpriority(PRIO_PROCESS, 0, want) < 0) {
        dprintf(2, "nice: cannot set niceness %d: %s\n", want, zwhy("refused"));
        return 125;
    }
    char path[ZPATH_MAX];
    if (!strchr(argv[i], '/') && !path_find(argv[i], path, sizeof path)) { dprintf(2, "nice: %s: not found\n", argv[i]); return 127; }
    int pid = sys_spawn(strchr(argv[i], '/') ? argv[i] : path, argv + i, 0);        /* a name with a slash as it is: the kernel judges its length */
    if (pid < 0) { dprintf(2, "nice: %s: %s\n", argv[i], zwhy("cannot run")); return 126; }
    int st = sys_wait(pid);             /* the command is in our job's group, the terminal's foreground (DESIGN D16) */
    return st;
}
