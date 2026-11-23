/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "klogread.h"

/* dmesg: print the kernel's message log (what the kernel printed on the serial port since boot).
 * dmesg -p: the log of the previous boot, if it survived the reset (audit A12; KLOG_PREV in the offset).
 * Read from where the log is held (klogread.h: after the ring has wrapped, never a chunk twice, audit F10). */

static int rd(unsigned off, void *b, unsigned n) { return sys_klog(off, b, n); }
static void out(const char *s, unsigned n) { sys_write(1, s, n); }

static const char USAGE[] = "usage: dmesg [-p]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    static char buf[512];
    u32 flag = 0;
    if (argc == 2 && !strcmp(argv[1], "-p"))
        flag = KLOG_PREV;
    else if (argc > 1) {
        if (argv[1][0] == '-') zbadopt(argv[1], USAGE);
        zusage(USAGE);
    }
    unsigned got = klog_dump(rd, flag, out, buf, sizeof buf);
    if (flag && !got) {
        dprintf(2, "dmesg: no log from a previous boot\n");
        return 1;
    }
    return 0;
}
