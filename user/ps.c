/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* PRI: the level the process runs at now (0 highest .. 31; a server serving a better client runs at that client's
 * level), NI: its nice value (-20 .. 19). DESIGN D10. MEM: the memory it owns; SHR: the shared code it runs, one copy
 * for every process running the same program (DESIGN D13), not counted in MEM. */
int main(void)
{
    static const char *st[] = { "free", "ready", "sleep", "wait", "io", "zombie", "msg", "stopped" };
    struct uproc p;
    dprintf(1, "  PID  PPID PRI  NI STATE    MEM(K) SHR(K) NAME\n");
    for (u32 i = 0; i < 256; i++) {      /* until procinfo says the table ends */
        if (sys_procinfo(i, &p) < 0 || p.state == 0) continue;
        if (p.pid == 0) dprintf(1, "%5u %5u   - %3d %-8s %6u %6u %s\n", p.pid, p.ppid, p.nice, st[p.state], p.mem / 1024,
                               p.shared / 1024, p.name);
        else dprintf(1, "%5u %5u %3u %3d %-8s %6u %6u %s\n", p.pid, p.ppid, p.pri, p.nice, st[p.state], p.mem / 1024,
                    p.shared / 1024, p.name);
    }
    return 0;
}
