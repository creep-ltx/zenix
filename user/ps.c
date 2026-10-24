/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* PRI: the level the process runs at now (0 highest .. 31; a server serving a better client runs at that client's
 * level), NI: its nice value (-20 .. 19). DESIGN D10. */
int main(void)
{
    static const char *st[] = { "free", "ready", "sleep", "wait", "io", "zombie", "msg", "stopped" };
    struct uproc p;
    printf("  PID  PPID PRI  NI STATE    MEM(K) NAME\n");
    for (u32 i = 0; i < 256; i++) {      /* until procinfo says the table ends */
        if (sys_procinfo(i, &p) < 0 || p.state == 0) continue;
        if (p.pid == 0) printf("%5u %5u   - %3d %-8s %6u %s\n", p.pid, p.ppid, p.nice, st[p.state], p.mem / 1024, p.name);
        else printf("%5u %5u %3u %3d %-8s %6u %s\n", p.pid, p.ppid, p.pri, p.nice, st[p.state], p.mem / 1024, p.name);
    }
    return 0;
}
