// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* pgrep [-l] [-x] name   /   pkill [-x] name: find processes by program name (a part of it; -x the whole name).
 * pkill ends them. Never matches itself or the shell that ran it. */
int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    int kill_them = !strcmp(me, "pkill"), list = 0, exact = 0, i = 1, found = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'l') list = 1; else if (*p == 'x') exact = 1; else { fprintf(2, "%s: unknown option -%c\n", me, *p); return 2; } }
    if (i >= argc) { fprintf(2, "usage: %s [-lx] name\n", me); return 2; }
    int self = sys_getpid(); struct uproc p; int parent = -1;
    if (sys_procinfo(0, &p) == 0) { }
    for (u32 k = 0; k < 256; k++) {      /* until procinfo says the table ends */
        if (sys_procinfo(k, &p) < 0 || p.state == 0) continue;
        if ((int)p.pid == self) parent = (int)p.ppid;
    }
    for (u32 k = 0; k < 256; k++) {      /* until procinfo says the table ends */
        if (sys_procinfo(k, &p) < 0 || p.state == 0 || p.pid == 0 || (int)p.pid == self || (int)p.pid == parent || p.state == 5) continue;
        if (exact ? strcmp(p.name, argv[i]) : !strstr(p.name, argv[i])) continue;
        found++;
        if (kill_them) sys_kill((int)p.pid);
        else if (list) printf("%u %s\n", p.pid, p.name); else printf("%u\n", p.pid);
    }
    return found ? 0 : 1;
}
