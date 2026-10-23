#include "libz.h"
/* cp [-r] [-f] [-p] [-v] source... dest   (dest a directory when there are several sources) */
static int o_r, o_f, o_p, o_v, rc;
static int __attribute__((noinline)) copy_file(const char *src, const char *dst, const struct ustat *st)   /* not in the recursion */
{
    int in = sys_open(src, O_RDONLY); if (in < 0) { fprintf(2, "cp: %s: %s\n", src, zwhy("cannot open")); return 1; }
    int out = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { fprintf(2, "cp: %s: %s\n", dst, zwhy("cannot write")); sys_close(in); return 1; }
    char b[1024]; int n, bad = 0;
    while ((n = sys_read(in, b, sizeof b)) > 0) if (sys_write(out, b, (u32)n) != n) { fprintf(2, "cp: write failed (disk full?)\n"); bad = 1; break; }
    sys_close(in); sys_close(out);
    if (o_p) { sys_chmod(dst, st->mode & 07777); sys_utimes(dst, st->mtime); }
    if (o_v && !bad) printf("'%s' -> '%s'\n", src, dst);
    return bad;
}
static void copy_path(const char *src, const char *dst)
{
    struct ustat st;
    if (sys_stat(src, &st) < 0) { fprintf(2, "cp: %s: %s\n", src, zwhy("no such file or directory")); rc = 1; return; }
    if ((st.mode & S_IFMT) == S_IFDIR) {
        if (!o_r) { fprintf(2, "cp: %s: is a directory (use -r)\n", src); rc = 1; return; }
        struct ustat dt;
        if (sys_stat(dst, &dt) < 0 && sys_mkdir(dst) < 0) { fprintf(2, "cp: %s: %s\n", dst, zwhy("cannot create")); rc = 1; return; }
        if (o_p) sys_chmod(dst, st.mode & 07777);
        char **names; int n = dir_list(src, &names);          /* heap, not stack: one level per directory */
        if (n < 0) { fprintf(2, "cp: %s: %s\n", src, zwhy("cannot read")); rc = 1; return; }
        for (int i = 0; i < n; i++) {
            char *s2 = path_join(src, names[i]), *d2 = s2 ? path_join(dst, names[i]) : 0;
            if (d2) copy_path(s2, d2);
            else { fprintf(2, "cp: %s/%s: %s\n", s2 ? dst : src, names[i], zwhy("cannot copy")); rc = 1; }
            free(s2); free(d2);
        }
        dir_free(names, n);
        return;
    }
    if (copy_file(src, dst, &st)) rc = 1;
}
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'r' || *p == 'R') o_r = 1; else if (*p == 'f') o_f = 1; else if (*p == 'p') o_p = 1; else if (*p == 'v') o_v = 1; else { fprintf(2, "cp: unknown option -%c\n", *p); return 2; } }
    int n = argc - i;
    if (n < 2) { fprintf(2, "usage: cp [-rfpv] source... dest\n"); return 2; }
    const char *dest = argv[argc - 1]; struct ustat dt;
    int todir = sys_stat(dest, &dt) == 0 && (dt.mode & S_IFMT) == S_IFDIR;
    if (n > 2 && !todir) { fprintf(2, "cp: %s: not a directory\n", dest); return 2; }
    for (; i < argc - 1; i++) {
        char *target;
        if (todir) { const char *b = strrchr(argv[i], '/'); b = b && b[1] ? b + 1 : argv[i]; target = path_join(dest, b); }
        else target = strdup(dest);
        if (!target) { fprintf(2, "cp: %s: %s\n", argv[i], zwhy("cannot copy")); rc = 1; continue; }
        copy_path(argv[i], target);
        free(target);
    }
    return rc;
}
