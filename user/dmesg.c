/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"

/* dmesg: print the kernel's message log (what the kernel printed on the serial port since boot).
 * dmesg -p: the log of the previous boot, if it survived the reset (audit A12; KLOG_PREV in the offset). */
#define KLOG_PREV 0x80000000u

int main(int argc, char **argv)
{
    static char buf[512];
    u32 off = 0, flag = 0;
    int n;
    if (argc == 2 && !strcmp(argv[1], "-p"))
        flag = KLOG_PREV;
    else if (argc > 1) {
        fprintf(2, "usage: dmesg [-p]\n");
        return 2;
    }
    while ((n = sys_klog(flag | off, buf, sizeof buf)) > 0) {
        sys_write(1, buf, (u32)n);
        off += (u32)n;
    }
    if (flag && !off) {
        fprintf(2, "dmesg: no log from a previous boot\n");
        return 1;
    }
    return 0;
}
