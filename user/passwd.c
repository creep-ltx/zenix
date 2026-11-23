// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
static const char USAGE[] = "usage: passwd [user]\n";
/* passwd [user]: change a password (set-user-id root). Users change their own after giving the old one; root
 * changes anyone's. An empty new password removes the password. */
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    if (argc > 1 && argv[1][0] == '-') zbadopt(argv[1], USAGE);
    u32 me = (u32)sys_getuid();
    struct pw p;
    const char *who = argc > 1 ? argv[1] : 0;
    if (who ? pw_byname(who, &p) : pw_byuid(me, &p)) { dprintf(2, "passwd: no such user\n"); return 1; }
    if (me != 0 && p.uid != me) { dprintf(2, "passwd: only root can change other users' passwords\n"); return 1; }
    char old[100], n1[100], n2[100];
    if (me != 0 && p.hash[0]) {
        if (read_secret("Current password: ", old, sizeof old) || !pw_check(old, p.hash)) { sys_sleep(100); dprintf(2, "passwd: wrong password\n"); return 1; }
    }
    if (read_secret("New password: ", n1, sizeof n1) || read_secret("Again: ", n2, sizeof n2)) return 1;
    if (strcmp(n1, n2)) { dprintf(2, "passwd: the two did not match\n"); return 1; }
    char hash[100];
    pw_new_hash(n1, hash);
    if (pw_set_hash_at("passwd", "/etc/passwd", p.name, hash)) return 1;     /* a new file, then renamed over (pw_replace) */
    dprintf(1, "passwd: password for %s %s\n", p.name, hash[0] ? "changed" : "removed");
    return 0;
}
