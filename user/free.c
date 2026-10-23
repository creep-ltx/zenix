/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
int main(void)
{
    u32 m[4];
    sys_meminfo(m);
    printf("chip RAM free: %u KB\nfast RAM free: %u KB\n", m[0] / 1024, m[1] / 1024);
    return 0;
}
