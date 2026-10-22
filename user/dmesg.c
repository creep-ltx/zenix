/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"

/* dmesg: print the kernel's message log (what the kernel printed on the serial port since boot) */
int main(void)
{
    static char buf[512];
    u32 off = 0;
    int n;
    while ((n = sys_klog(off, buf, sizeof buf)) > 0) {
        sys_write(1, buf, (u32)n);
        off += (u32)n;
    }
    return 0;
}
