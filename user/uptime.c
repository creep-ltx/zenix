/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
int main(void)
{
    u32 s = sys_uptime();               /* the kernel's own seconds count: no restart at 0 after 497 days (F9) */
    dprintf(1, "up %u:%02u:%02u\n", s / 3600, (s / 60) % 60, s % 60);
    return 0;
}
