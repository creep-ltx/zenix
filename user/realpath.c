// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* realpath [-e] path...: print the absolute name with ., .. and symbolic links resolved (-e: the path must exist).
 * Links are followed as the kernel follows them (DESIGN D26): at most 40 in one path, then "cannot resolve". One
 * pass at a time, no recursion: the buffers are static (a 40-link chain on a 16 KB stack, CLAUDE.md) */
#define MAXLINKS 40
static char path[320], acc[160], tgt[160], next[320];
static int resolve(const char *in, char *out)
{
    int links = 0;
    acc[0] = 0;
    if (in[0] != '/' && sys_getcwd(acc, sizeof acc) < 0) strcpy(acc, "/");
    if (!strcmp(acc, "/")) acc[0] = 0;
    snprintf(path, sizeof path, "%s", in);
    char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *e = p; while (*e && *e != '/') e++;
        char save = *e; *e = 0;
        if (!strcmp(p, ".")) { }
        else if (!strcmp(p, "..")) { char *sl = strrchr(acc, '/'); if (sl) *sl = 0; }
        else {
            u32 l = strlen(acc); if (l + strlen(p) + 2 >= sizeof acc) return -1;
            acc[l] = '/'; strcpy(acc + l + 1, p);
            struct ustat st;
            if (sys_lstat(acc[0] ? acc : "/", &st) == 0 && (st.mode & S_IFMT) == S_IFLNK) {
                if (++links > MAXLINKS) return -1;
                int n = sys_readlink(acc, tgt, sizeof tgt - 1); if (n < 0) return -1;
                if (n == (int)sizeof tgt - 1) return -1;    /* a longer target (F15): "cannot resolve", never a prefix */
                tgt[n] = 0;
                *strrchr(acc, '/') = 0;                     /* the directory part, then the link text relative to it */
                *e = save;
                if ((u32)snprintf(next, sizeof next, "%s%s", tgt, e) >= sizeof next) return -1;
                strcpy(path, next);
                if (tgt[0] == '/') acc[0] = 0;              /* an absolute target starts again from / */
                p = path;
                continue;
            }
        }
        *e = save; p = e;
    }
    snprintf(out, 160, "%s", acc[0] ? acc : "/");
    return 0;
}
int main(int argc, char **argv)
{
    int must = 0, i = 1, rc = 0;
    if (i < argc && !strcmp(argv[i], "-e")) { must = 1; i++; }
    if (i >= argc) { fprintf(2, "usage: realpath [-e] path...\n"); return 2; }
    for (; i < argc; i++) {
        static char out[160]; struct ustat st;
        if (resolve(argv[i], out) < 0) { fprintf(2, "realpath: %s: cannot resolve\n", argv[i]); rc = 1; continue; }
        if (must && sys_stat(out, &st) < 0) { fprintf(2, "realpath: %s: not found\n", argv[i]); rc = 1; continue; }
        printf("%s\n", out);
    }
    return rc;
}
