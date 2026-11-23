/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
int main(int argc, char **argv)
{
    znoargs(argc, argv, "usage: free\n");
    u32 m[4];
    sys_meminfo(m);
    dprintf(1, "chip RAM free: %u KB\nfast RAM free: %u KB\n", m[0] / 1024, m[1] / 1024);
    /* DESIGN D13: the code of programs with shared code, one copy each, and what the copies not loaded would take */
    dprintf(1, "shared code: %u KB, sharing saves %u KB\n", m[2] / 1024, m[3] / 1024);
    return 0;
}
