// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* pgrep [-l] [-x] name   /   pkill [-SIGNAL] [-x] name: find processes by program name (a part of it; -x the whole
 * name). pkill sends them SIGNAL (SIGTERM if none is named, DESIGN D16). Never matches itself or the shell that ran
 * it. */
int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    int kill_them = !strcmp(me, "pkill"), list = 0, exact = 0, i = 1, found = 0, sig = SIGTERM;
    const char *USAGE = kill_them ? "usage: pkill [-SIGNAL] [-x] name\n" : "usage: pgrep [-lx] name\n";
    zopt(argc, argv, USAGE);
    for (; zisopt(argc, argv, &i, USAGE); i++) {
        if (kill_them && zsig_num(argv[i] + 1) >= 0 && strcmp(argv[i], "-x")) { sig = zsig_num(argv[i] + 1); continue; }
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'l') list = 1; else if (*p == 'x') exact = 1; else zbadch(*p, USAGE); }
    }
    if (i >= argc) zusage(USAGE);
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
        if (kill_them) kill((int)p.pid, sig);
        else if (list) dprintf(1, "%u %s\n", p.pid, p.name); else dprintf(1, "%u\n", p.pid);
    }
    return found ? 0 : 1;
}
