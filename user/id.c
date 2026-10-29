// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* id [user]: uid, gid and groups */
int main(int argc, char **argv)
{
    u32 uid = (u32)sys_getuid(), gid = (u32)sys_getgid();
    char b[1];
    if (argc > 1) {
        struct pw p;
        if (pw_byname(argv[1], &p)) { dprintf(2, "id: %s: no such user\n", argv[1]); return 1; }
        uid = p.uid; gid = p.gid;
    }
    dprintf(1, "uid=%u(%s) gid=%u(%s)", uid, pw_name(uid, b), gid, gr_name(gid, b));
    if (argc == 1 && (u32)sys_geteuid() != uid) dprintf(1, " euid=%u(%s)", (u32)sys_geteuid(), pw_name((u32)sys_geteuid(), b));
    dprintf(1, "\n");
    return 0;
}
