#include "libz.h"
int main(void)
{
    static const char *st[] = { "free", "ready", "sleep", "wait", "io", "zombie", "msg", "stopped" };
    struct uproc p;
    printf("  PID  PPID STATE    MEM(K) NAME\n");
    for (u32 i = 0; i < 256; i++) {      /* until procinfo says the table ends */
        if (sys_procinfo(i, &p) < 0 || p.state == 0) continue;
        printf("%5u %5u %-8s %6u %s\n", p.pid, p.ppid, st[p.state], p.mem / 1024, p.name);
    }
    return 0;
}
