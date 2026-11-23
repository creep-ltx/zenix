/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* the running kernel's version (SYS_update INFO, DESIGN D15); a kernel without it is older than 2026-10-26 */
int main(int argc, char **argv)
{
    znoargs(argc, argv, "usage: uname\n");
    struct uupdate u;
    if (sys_update(UPD_INFO, (u32)&u, sizeof u) < 0) { dprintf(1, "Zenix 0.2 m68k (68020+, no MMU)\n"); return 0; }
    u.kver[15] = 0;
    dprintf(1, "Zenix %s (%u) m68k (68020+, no MMU)\n", u.kver, u.kvnum);
    return 0;
}
