// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* realpath [-e] path...: print the absolute name with ., .. and symbolic links resolved (-e: the path must exist) */
static int resolve(const char *in, char *out, int depth)
{
    char cur[160]; cur[0] = 0;
    if (in[0] != '/') { if (sys_getcwd(cur, sizeof cur) < 0) strcpy(cur, "/"); }
    char path[300]; snprintf(path, sizeof path, "%s", in);
    char acc[160]; snprintf(acc, sizeof acc, "%s", cur[0] ? cur : "");
    if (!strcmp(acc, "/")) acc[0] = 0;
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
                if (depth > 8) return -1;
                char tgt[160]; int n = sys_readlink(acc, tgt, sizeof tgt - 1); if (n < 0) return -1;
                if (n == (int)sizeof tgt - 1) return -1;    /* a longer target (F15): "cannot resolve", never a prefix */
                tgt[n] = 0;
                char *sl = strrchr(acc, '/'); *sl = 0;                  /* the directory part, then the link text relative to it */
                char joined[300]; if (tgt[0] == '/') snprintf(joined, sizeof joined, "%s", tgt); else snprintf(joined, sizeof joined, "%s/%s", acc[0] ? acc : "/", tgt);
                *e = save; char rest[300]; snprintf(rest, sizeof rest, "%s%s", joined, e);
                return resolve(rest, out, depth + 1);
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
        char out[160]; struct ustat st;
        if (resolve(argv[i], out, 0) < 0) { fprintf(2, "realpath: %s: cannot resolve\n", argv[i]); rc = 1; continue; }
        if (must && sys_stat(out, &st) < 0) { fprintf(2, "realpath: %s: not found\n", argv[i]); rc = 1; continue; }
        printf("%s\n", out);
    }
    return rc;
}
