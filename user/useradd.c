// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"

/* useradd [-u uid] [-g group] [-d home] [-s shell] [-c comment] [-G group,...] [-M] name: add a user to /etc/passwd
 * (root only); -G also makes them a member of those groups (-G wheel: may use sudo).
 * The home directory is made (owned by the user, mode 700) unless -M; the password is empty until passwd sets it.
 * /etc/passwd is replaced safely (pw_replace). */

static int name_ok(const char *n)                   /* [a-z_][a-z0-9_-]*, 1-31 characters */
{
    if (!*n || strlen(n) > 31 || !((*n >= 'a' && *n <= 'z') || *n == '_')) return 0;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}
static int field_ok(const char *s) { for (; *s; s++) if (*s == ':' || *s == '\n') return 0; return 1; }

int main(int argc, char **argv)
{
    const char *group = "users", *home = 0, *shell = "/bin/sh", *comment = "", *name = 0, *extra = 0;
    long uid = -1; int mkhome = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && !a[2] && strchr("ugdscG", a[1])) {
            if (i + 1 >= argc) { fprintf(2, "useradd: -%c needs a value\n", a[1]); return 2; }
            const char *v = argv[++i];
            switch (a[1]) {
            case 'u': { char *e; unsigned long n = strtoul(v, &e, 10); if (*e || !*v || n > 65535) { fprintf(2, "useradd: %s: not a user number\n", v); return 2; } uid = (long)n; break; }
            case 'g': group = v; break;
            case 'd': home = v; break;
            case 's': shell = v; break;
            case 'c': comment = v; break;
            case 'G': extra = v; break;
            }
        } else if (!strcmp(a, "-M")) mkhome = 0;
        else if (a[0] == '-') { fprintf(2, "usage: useradd [-u uid] [-g group] [-d home] [-s shell] [-c comment] [-G group,...] [-M] name\n"); return 2; }
        else if (!name) name = a;
        else { fprintf(2, "useradd: one name at a time\n"); return 2; }
    }
    if (!name) { fprintf(2, "usage: useradd [-u uid] [-g group] [-d home] [-s shell] [-c comment] [-G group,...] [-M] name\n"); return 2; }
    if (sys_getuid() != 0) { fprintf(2, "useradd: only root may add users\n"); return 1; }
    if (!name_ok(name)) { fprintf(2, "useradd: %s: a name is 1-31 of a-z 0-9 _ - and starts with a letter or _\n", name); return 2; }
    char hbuf[64];
    if (!home) { if (strlen(name) + 7 > sizeof hbuf) return 2; strcpy(hbuf, "/home/"); strcat(hbuf, name); home = hbuf; }
    if (!field_ok(home) || !field_ok(shell) || !field_ok(comment) || strlen(home) > 63 || strlen(shell) > 31 || strlen(comment) > 47 || home[0] != '/') {
        fprintf(2, "useradd: the home (absolute, at most 63), shell (31) and comment (47) may not hold ':' or a newline\n"); return 2;
    }
    struct pw p; struct gr g;
    if (!pw_byname(name, &p)) { fprintf(2, "useradd: %s exists already\n", name); return 1; }
    char *e; unsigned long gnum = strtoul(group, &e, 10);
    if ((*e || !*group) ? gr_byname(group, &g) : gr_bygid((u32)gnum, &g)) { fprintf(2, "useradd: %s: no such group\n", group); return 1; }
    if (extra) {                                             /* every extra group must exist before anything is written */
        char gl[128];
        if (strlen(extra) >= sizeof gl) { fprintf(2, "useradd: -G: too long\n"); return 2; }
        strcpy(gl, extra);
        for (char *t = gl, *c; t; t = c) {
            c = strchr(t, ','); if (c) *c++ = 0;
            struct gr x;
            if (!*t || gr_byname(t, &x)) { fprintf(2, "useradd: %s: no such group\n", t); return 1; }
        }
    }
    if (uid < 0) {                                           /* the lowest free number from 1000 */
        for (uid = 1000; uid <= 65535 && !pw_byuid((u32)uid, &p); uid++) ;
        if (uid > 65535) { fprintf(2, "useradd: no free user number\n"); return 1; }
    } else if (!pw_byuid((u32)uid, &p)) { fprintf(2, "useradd: user number %ld is %s's\n", uid, p.name); return 1; }

    int n = 0;
    char *old = pw_slurp("/etc/passwd", &n);
    if (!old) { fprintf(2, "useradd: cannot read /etc/passwd\n"); return 1; }
    u32 cap = (u32)n + 256;
    char *nf = malloc(cap);
    if (!nf) { fprintf(2, "useradd: out of memory\n"); return 1; }
    memcpy(nf, old, (u32)n);
    u32 o = (u32)n;
    if (o && nf[o - 1] != '\n') nf[o++] = '\n';
    o += (u32)snprintf(nf + o, cap - o, "%s::%ld:%lu:%s:%s:%s\n", name, uid, (unsigned long)g.gid, comment, home, shell);
    if (o > 65535) { fprintf(2, "useradd: /etc/passwd would pass 64 KB\n"); return 1; }
    if (pw_replace("useradd", "/etc/passwd", nf, o, 0644)) return 1;
    if (mkhome) {
        struct ustat st;
        if (sys_stat(home, &st) == 0) printf("useradd: %s exists already; left as it is\n", home);
        else if (sys_mkdir(home) < 0 || sys_chown(home, (u32)uid, g.gid) < 0 || sys_chmod(home, 0700) < 0) {
            fprintf(2, "useradd: %s: %s (the user was added)\n", home, zwhy("cannot make the home directory"));
            return 1;
        }
    }
    if (extra) {
        char gl[128]; strcpy(gl, extra);
        for (char *t = gl, *c; t; t = c) {
            c = strchr(t, ','); if (c) *c++ = 0;
            if (gr_edit("useradd", t, name, 1)) return 1;
        }
    }
    printf("useradd: %s added (user %ld, group %s); set a password with: passwd %s\n", name, uid, g.name, name);
    return 0;
}
