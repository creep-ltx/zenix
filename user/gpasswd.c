// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* gpasswd -a user group | -d user group: add a user to a group's member list in /etc/group, or take them out (root
 * only). The file is replaced safely. Example: gpasswd -a adam wheel lets adam use sudo. */
int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[1], "-a") && strcmp(argv[1], "-d"))) { dprintf(2, "usage: gpasswd -a user group\n       gpasswd -d user group\n"); return 2; }
    int add = argv[1][1] == 'a';
    const char *user = argv[2], *group = argv[3];
    if (sys_getuid() != 0) { dprintf(2, "gpasswd: only root may change groups\n"); return 1; }
    struct pw p; struct gr g;
    if (add && pw_byname(user, &p)) { dprintf(2, "gpasswd: %s: no such user\n", user); return 1; }
    if (gr_byname(group, &g)) { dprintf(2, "gpasswd: %s: no such group\n", group); return 1; }
    int member = gr_has(g.members, user);
    if (add && member) { dprintf(1, "gpasswd: %s is in %s already\n", user, group); return 0; }
    if (!add && !member) { dprintf(2, "gpasswd: %s is not a member of %s\n", user, group); return 1; }
    if (gr_edit("gpasswd", group, user, add)) return 1;
    dprintf(1, "gpasswd: %s %s %s\n", user, add ? "added to" : "removed from", group);
    return 0;
}
