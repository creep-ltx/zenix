// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* id [user]: uid, gid and groups. Without a user: the process's own ids and the supplementary groups the kernel
 * holds (SYS_groups); with one: what /etc/passwd and /etc/group give that user (what login would set) */
int main(int argc, char **argv)
{
    u32 uid = (u32)sys_getuid(), gid = (u32)sys_getgid();
    unsigned short g[NGROUPS + 1];
    int n;
    char b[1];
    if (argc > 1) {
        struct pw p;
        if (pw_byname(argv[1], &p)) { dprintf(2, "id: %s: no such user\n", argv[1]); return 1; }
        uid = p.uid; gid = p.gid;
        n = gr_list(p.name, p.gid, g, NGROUPS + 1);
        if (n > NGROUPS + 1) n = NGROUPS + 1;
    } else {
        n = sys_getgroups(g + 1, NGROUPS);
        if (n < 0) n = 0;
        g[0] = (unsigned short)gid;
        int k = 1;                      /* the gid first, then the others (the list usually holds the gid too) */
        for (int i = 1; i <= n; i++) if (g[i] != gid) g[k++] = g[i];
        n = k;
    }
    dprintf(1, "uid=%u(%s) gid=%u(%s)", uid, pw_name(uid, b), gid, gr_name(gid, b));
    if (argc == 1 && (u32)sys_geteuid() != uid) dprintf(1, " euid=%u(%s)", (u32)sys_geteuid(), pw_name((u32)sys_geteuid(), b));
    for (int i = 0; i < n; i++) dprintf(1, "%s%u(%s)", i ? "," : " groups=", (u32)g[i], gr_name(g[i], b));
    dprintf(1, "\n");
    return 0;
}
