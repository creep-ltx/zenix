/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* test client: a few calls, a zero-copy string call, then N timed round trips */
int main(int argc, char **argv)
{
    int port = sys_port_find("ping");
    if (port < 0) { dprintf(1, "msgping: no server (run msgpingd &)\n"); return 1; }
    struct umsg m;
    m.type = 1; m.a[0] = 40; m.a[1] = 2;
    int r = sys_call(port, &m);
    dprintf(1, "msgping: 40+2 = %u (result %d)\n", m.a[2], r);
    char text[] = "zero copy: the server edits my buffer";
    m.type = 2; m.a[0] = (u32)text;
    sys_call(port, &m);
    dprintf(1, "msgping: %s\n", text);
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    if (n < 1) n = 1;
    u32 t0 = sys_hrtime();                              /* the E-clock, as bench uses: the 100 Hz tick is too coarse */
    for (int i = 0; i < n; i++) { m.type = 1; m.a[0] = (u32)i; m.a[1] = 1; sys_call(port, &m); }
    u32 e = sys_hrtime() - t0;
    u32 tenths = (u32)(((unsigned long long)e * 10000000ull) / 709379u / (u32)n);   /* 0.1 us per trip (PAL E-clock) */
    u32 total = (u32)(((unsigned long long)e * 1000000ull) / 709379u);
    dprintf(1, "msgping: %d round trips in %u us, %u.%u us each\n", n, total, tenths / 10, tenths % 10);
    m.type = 9; sys_call(port, &m);
    return 0;
}
