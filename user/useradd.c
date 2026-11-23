// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"

/* useradd [-u uid] [-g group] [-d home] [-s shell] [-c comment] [-G group,...] [-M] name: add a user to /etc/passwd
 * (root only); -G also makes them a member of those groups (-G wheel: may use sudo).
 * The home directory is made (owned by the user, mode 700) unless -M; the password is empty until passwd sets it.
 * /etc/passwd is replaced safely (pw_replace). */

static const char USAGE[] = "usage: useradd [-u uid] [-g group] [-d home] [-s shell] [-c comment] [-G group,...] [-M] name\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    const char *group = "users", *home = 0, *shell = "/bin/sh", *comment = "", *name = 0, *extra = 0;
    long uid = -1; int mkhome = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && !a[2] && strchr("ugdscG", a[1])) {
            if (i + 1 >= argc) { dprintf(2, "useradd: -%c needs a value\n", a[1]); return 2; }
            const char *v = argv[++i];
            switch (a[1]) {
            case 'u': { char *e; unsigned long n = strtoul(v, &e, 10); if (*e || !*v || n > 65535) { dprintf(2, "useradd: %s: not a user number\n", v); return 2; } uid = (long)n; break; }
            case 'g': group = v; break;
            case 'd': home = v; break;
            case 's': shell = v; break;
            case 'c': comment = v; break;
            case 'G': extra = v; break;
            }
        } else if (!strcmp(a, "-M")) mkhome = 0;
        else if (a[0] == '-') zbadopt(a, USAGE);
        else if (!name) name = a;
        else { dprintf(2, "useradd: one name at a time\n"); return 2; }
    }
    if (!name) zusage(USAGE);
    if (sys_getuid() != 0) { dprintf(2, "useradd: only root may add users\n"); return 1; }
    char hbuf[64];
    if (!home) { if (strlen(name) + 7 > sizeof hbuf) return 2; strcpy(hbuf, "/home/"); strcat(hbuf, name); home = hbuf; }
    struct pw_add na = { name, group, home, shell, comment, extra, "", uid };
    u32 nuid, ngid;
    int rc = pw_add_at("useradd", "/etc/passwd", "/etc/group", &na, &nuid, &ngid);
    if (rc) return rc;
    uid = (long)nuid;
    struct gr g;
    if (gr_bygid(ngid, &g)) snprintf(g.name, sizeof g.name, "%u", ngid);
    if (mkhome) {
        struct ustat st;
        if (sys_stat(home, &st) == 0) dprintf(1, "useradd: %s exists already; left as it is\n", home);
        else if (sys_mkdir(home) < 0 || sys_chown(home, (u32)uid, ngid) < 0 || sys_chmod(home, 0700) < 0) {
            dprintf(2, "useradd: %s: %s (the user was added)\n", home, zwhy("cannot make the home directory"));
            return 1;
        }
    }
    dprintf(1, "useradd: %s added (user %ld, group %s); set a password with: passwd %s\n", name, uid, g.name, name);
    return 0;
}
