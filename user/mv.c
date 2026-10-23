#include "libz.h"
/* mv [-f] [-v] source... dest : rename; across directories a directory is copied and removed */
static int o_v;
/* a file's copy buffer lives here, not in copy_tree(), which recurses once per directory level */
static int __attribute__((noinline)) copy_file(const char *src, const char *dst, const struct ustat *st)
{
    int in = sys_open(src, O_RDONLY), out = in < 0 ? -1 : sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC), bad = in < 0 || out < 0;
    char b[1024]; int n; while (!bad && (n = sys_read(in, b, sizeof b)) > 0) if (sys_write(out, b, (u32)n) != n) bad = 1;
    if (in >= 0) sys_close(in);
    if (out >= 0) sys_close(out);
    if (bad) { fprintf(2, "mv: %s: %s\n", src, zwhy("cannot copy")); return -1; }
    sys_chmod(dst, st->mode & 07777); sys_utimes(dst, st->mtime); return 0;
}
static int copy_tree(const char *src, const char *dst)
{
    struct ustat st; if (sys_stat(src, &st) < 0) return -1;
    if ((st.mode & S_IFMT) != S_IFDIR) return copy_file(src, dst, &st);
    if (sys_mkdir(dst) < 0) return -1;
    char **names; int n = dir_list(src, &names);              /* heap, not stack: one level per directory */
    if (n < 0) { fprintf(2, "mv: %s: %s\n", src, zwhy("cannot read")); return -1; }
    int r = 0;
    for (int i = 0; i < n && !r; i++) {
        char *s2 = path_join(src, names[i]), *d2 = s2 ? path_join(dst, names[i]) : 0;
        if (!d2) { fprintf(2, "mv: %s/%s: %s\n", s2 ? dst : src, names[i], zwhy("cannot copy")); r = -1; }
        else r = copy_tree(s2, d2);
        free(s2); free(d2);
    }
    dir_free(names, n);
    return r;
}
static int remove_tree(const char *p)
{
    struct ustat st; if (sys_lstat(p, &st) < 0) return -1;
    if ((st.mode & S_IFMT) != S_IFDIR) return sys_unlink(p);
    char **names; int n = dir_list(p, &names);                /* heap, not stack: one level per directory */
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        char *c = path_join(p, names[i]);
        if (c) remove_tree(c); else fprintf(2, "mv: %s/%s: %s\n", p, names[i], zwhy("cannot remove"));
        free(c);
    }
    dir_free(names, n);
    return sys_rmdir(p);
}
int main(int argc, char **argv)
{
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'v') o_v = 1; else if (*p != 'f') { fprintf(2, "mv: unknown option -%c\n", *p); return 2; } }
    int n = argc - i;
    if (n < 2) { fprintf(2, "usage: mv [-fv] source... dest\n"); return 2; }
    const char *dest = argv[argc - 1]; struct ustat dt;
    int todir = sys_stat(dest, &dt) == 0 && (dt.mode & S_IFMT) == S_IFDIR;
    if (n > 2 && !todir) { fprintf(2, "mv: %s: not a directory\n", dest); return 2; }
    for (; i < argc - 1; i++) {
        char *target;
        if (todir) { const char *b = strrchr(argv[i], '/'); b = b && b[1] ? b + 1 : argv[i]; target = path_join(dest, b); }
        else target = strdup(dest);
        if (!target) { fprintf(2, "mv: %s: %s\n", argv[i], zwhy("cannot move")); rc = 1; continue; }
        if (sys_rename(argv[i], target) < 0) {
            if (copy_tree(argv[i], target) < 0 || remove_tree(argv[i]) < 0) { fprintf(2, "mv: cannot move %s\n", argv[i]); rc = 1; free(target); continue; }
        }
        if (o_v) printf("'%s' -> '%s'\n", argv[i], target);
        free(target);
    }
    return rc;
}
