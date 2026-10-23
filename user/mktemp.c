// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* mktemp [-d] [-p dir] [template]: create a file (or directory with -d) with a unique name and print it. The template
 * ends in X characters that are replaced (default /tmp/tmp.XXXXXX). */
int main(int argc, char **argv)
{
    int dir = 0, i = 1; const char *base = 0, *tmpl = "tmp.XXXXXX";
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-d")) dir = 1;
        else if (!strncmp(argv[i], "-p", 2)) base = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "/tmp");
        else { fprintf(2, "mktemp: unknown option %s\n", argv[i]); return 2; }
    }
    if (i < argc) tmpl = argv[i];
    char name[160];
    if (tmpl[0] == '/' || !base) { if (tmpl[0] != '/' && !base) base = "/tmp"; }
    if (tmpl[0] == '/') snprintf(name, sizeof name, "%s", tmpl); else snprintf(name, sizeof name, "%s/%s", base, tmpl);
    int n = (int)strlen(name), x = n; while (x > 0 && name[x - 1] == 'X') x--;
    if (x == n) { fprintf(2, "mktemp: the template needs trailing X characters\n"); return 2; }
    static const char set[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    /* the name: not guessable from the pid or the time alone (the E-clock's low bits change every 1.4 us), and the
     * file is created exclusively (O_EXCL: the check and the creation are one request), mode 0600 / 0700 from the start */
    u32 seed = sys_hrtime() * 2654435761u ^ ((u32)sys_time() << 8) ^ ((u32)sys_getpid() << 24) ^ sys_ticks() * 40503u;
    u32 old = (u32)sys_umask(077);
    for (int tries = 0; tries < 100; tries++) {
        for (int k = x; k < n; k++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;            /* xorshift32 */
            seed += sys_hrtime();
            name[k] = set[(seed >> 8) % 62];
        }
        if (dir) {
            if (sys_mkdir(name) < 0) {
                struct ustat st;
                if (sys_stat(name, &st) == 0) continue;                       /* taken: another name */
                sys_umask(old);
                fprintf(2, "mktemp: %s: %s\n", name, zwhy("cannot create")); return 1;
            }
        } else {
            int fd = sys_open(name, O_WRONLY | O_CREAT | O_EXCL);
            if (fd == -17) continue;                                            /* taken: another name */
            if (fd < 0) { sys_umask(old); fprintf(2, "mktemp: %s: %s\n", name, zwhy("cannot create")); return 1; }
            sys_close(fd);
        }
        sys_umask(old);
        printf("%s\n", name);
        return 0;
    }
    sys_umask(old);
    fprintf(2, "mktemp: no free name\n");
    return 1;
}
