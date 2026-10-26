// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"

/* userdel [-r] name: remove a user from /etc/passwd and from every member list in /etc/group (root only); -r also
 * removes the home directory (through rm -r). root (user 0) cannot be removed. Both files are replaced safely. */

/* rewrite FILE without the line whose first field is NAME (passwd), or with NAME taken out of the 4th field (group) */
static int rewrite(const char *file, const char *name, int group, int *found)
{
    int n = 0;
    char *old = pw_slurp(file, &n);
    if (!old) { fprintf(2, "userdel: cannot read %s\n", file); return -1; }
    char *nf = malloc((u32)n + 2);
    if (!nf) { fprintf(2, "userdel: out of memory\n"); return -1; }
    u32 o = 0, nl = strlen(name);
    for (char *l = old; *l; ) {
        char *e = strchr(l, '\n'); u32 len = e ? (u32)(e - l) : strlen(l);
        if (!group) {
            if (len > nl && l[nl] == ':' && !strncmp(l, name, nl)) *found = 1;      /* the user's line: left out */
            else { memcpy(nf + o, l, len); o += len; nf[o++] = '\n'; }
        } else {
            char line[256];
            if (len < sizeof line) {
                memcpy(line, l, len); line[len] = 0;
                char *f = line; int k = 0;                   /* the 4th field: name:pass:gid:members */
                for (; *f && k < 3; f++) if (*f == ':') k++;
                if (k == 3 && *f) gr_drop(f, name);
                u32 ll = strlen(line); memcpy(nf + o, line, ll); o += ll;
            } else { memcpy(nf + o, l, len); o += len; }    /* a line too long to edit is kept unchanged */
            nf[o++] = '\n';
        }
        if (!e) break;
        l = e + 1;
    }
    return pw_replace("userdel", file, nf, o, 0644);
}

int main(int argc, char **argv)
{
    int rmhome = 0; const char *name = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) rmhome = 1;
        else if (argv[i][0] == '-' || name) { fprintf(2, "usage: userdel [-r] name\n"); return 2; }
        else name = argv[i];
    }
    if (!name) { fprintf(2, "usage: userdel [-r] name\n"); return 2; }
    if (sys_getuid() != 0) { fprintf(2, "userdel: only root may remove users\n"); return 1; }
    struct pw p;
    if (pw_byname(name, &p)) { fprintf(2, "userdel: %s: no such user\n", name); return 1; }
    if (p.uid == 0) { fprintf(2, "userdel: %s is a root account: not removed\n", name); return 1; }
    int found = 0;
    if (rewrite("/etc/passwd", name, 0, &found) || !found) return 1;
    if (rewrite("/etc/group", name, 1, &found)) fprintf(2, "userdel: %s was removed, but /etc/group was not updated\n", name);
    if (rmhome) {
        if (!strcmp(p.home, "/") || p.home[0] != '/') fprintf(2, "userdel: home %s not removed (not a user's own directory)\n", p.home);
        else {
            char *av[] = { "rm", "-r", p.home, 0 };
            int pid = sys_spawn("/bin/rm", av, 0);
            if (pid < 0 || sys_wait(pid) != 0) fprintf(2, "userdel: %s: could not be removed completely\n", p.home);
        }
    }
    printf("userdel: %s removed%s\n", name, rmhome ? ", with the home directory" : "");
    return 0;
}
