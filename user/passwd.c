// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* passwd [user]: change a password (set-user-id root). Users change their own after giving the old one; root
 * changes anyone's. An empty new password removes the password. */
int main(int argc, char **argv)
{
    u32 me = (u32)sys_getuid();
    struct pw p;
    const char *who = argc > 1 ? argv[1] : 0;
    if (who ? pw_byname(who, &p) : pw_byuid(me, &p)) { fprintf(2, "passwd: no such user\n"); return 1; }
    if (me != 0 && p.uid != me) { fprintf(2, "passwd: only root can change other users' passwords\n"); return 1; }
    char old[100], n1[100], n2[100];
    if (me != 0 && p.hash[0]) {
        if (read_secret("Current password: ", old, sizeof old) || !pw_check(old, p.hash)) { sys_sleep(100); fprintf(2, "passwd: wrong password\n"); return 1; }
    }
    if (read_secret("New password: ", n1, sizeof n1) || read_secret("Again: ", n2, sizeof n2)) return 1;
    if (strcmp(n1, n2)) { fprintf(2, "passwd: the two did not match\n"); return 1; }
    char hash[100] = "";
    if (n1[0]) {
        char salt[12]; u32 t = sys_hrtime() ^ ((u32)sys_time() << 7);
        for (int i = 0; i < 8; i++) { salt[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[t % 36]; t = t * 1103515245u + 12345u; }
        salt[8] = 0;
        pw_hash(n1, salt, hash);
    }
    /* rewrite /etc/passwd through a new file, then rename it into place */
    static char old_file[4096], new_file[4600];
    int fd = sys_open("/etc/passwd", O_RDONLY), n = 0, r;
    if (fd < 0) { fprintf(2, "passwd: cannot read /etc/passwd\n"); return 1; }
    while (n < (int)sizeof old_file - 1 && (r = sys_read(fd, old_file + n, sizeof old_file - 1 - (u32)n)) > 0) n += r;
    sys_close(fd); old_file[n] = 0;
    u32 o = 0;
    for (char *l = old_file; *l; ) {
        char *e = strchr(l, '\n'); u32 len = e ? (u32)(e - l) : strlen(l);
        char *c1 = memchr(l, ':', len);
        if (c1 && (u32)(c1 - l) == strlen(p.name) && !strncmp(l, p.name, strlen(p.name))) {
            char *c2 = memchr(c1 + 1, ':', len - (u32)(c1 + 1 - l));
            if (c2) {                                      /* name: new hash, then the rest of the line from the next colon */
                u32 nl = strlen(p.name), hl = strlen(hash), rest = len - (u32)(c2 - l);
                memcpy(new_file + o, p.name, nl); o += nl; new_file[o++] = ':';
                memcpy(new_file + o, hash, hl); o += hl;
                memcpy(new_file + o, c2, rest); o += rest; new_file[o++] = '\n';
            }
        } else { memcpy(new_file + o, l, len); o += len; new_file[o++] = '\n'; }
        if (!e) break;
        l = e + 1;
    }
    fd = sys_open("/etc/passwd.new", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || sys_write(fd, new_file, o) != (int)o) { fprintf(2, "passwd: %s\n", zwhy("cannot write the new file")); return 1; }
    sys_close(fd);
    sys_chmod("/etc/passwd.new", 0644);
    if (sys_rename("/etc/passwd.new", "/etc/passwd") < 0) { fprintf(2, "passwd: cannot replace /etc/passwd\n"); return 1; }
    printf("passwd: password for %s %s\n", p.name, hash[0] ? "changed" : "removed");
    return 0;
}
