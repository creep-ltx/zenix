#include "libz.h"
/* mv [-f] [-v] source... dest : rename; across directories a directory is copied and removed */
static int o_v;
static int copy_tree(const char *src, const char *dst)
{
    struct ustat st; if (sys_stat(src, &st) < 0) return -1;
    if ((st.mode & S_IFMT) != S_IFDIR) {
        int in = sys_open(src, O_RDONLY), out = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC); if (in < 0 || out < 0) return -1;
        char b[1024]; int n; while ((n = sys_read(in, b, sizeof b)) > 0) if (sys_write(out, b, (u32)n) != n) return -1;
        sys_close(in); sys_close(out); sys_chmod(dst, st.mode & 07777); sys_utimes(dst, st.mtime); return 0;
    }
    if (sys_mkdir(dst) < 0) return -1;
    int fd = sys_open(src, O_RDONLY); if (fd < 0) return -1;
    char *names[256]; int n = 0; struct udirent d;
    while (sys_readdir(fd, &d) > 0 && n < 256) if (strcmp(d.name, ".") && strcmp(d.name, "..")) names[n++] = strdup(d.name);
    sys_close(fd);
    for (int i = 0; i < n; i++) { char s2[256], d2[256]; strcpy(s2, src); strcat(s2, "/"); strncpy(s2 + strlen(s2), names[i], 100); strcpy(d2, dst); strcat(d2, "/"); strncpy(d2 + strlen(d2), names[i], 100); if (copy_tree(s2, d2) < 0) return -1; free(names[i]); }
    return 0;
}
static int remove_tree(const char *p)
{
    struct ustat st; if (sys_lstat(p, &st) < 0) return -1;
    if ((st.mode & S_IFMT) != S_IFDIR) return sys_unlink(p);
    int fd = sys_open(p, O_RDONLY); if (fd < 0) return -1;
    char *names[256]; int n = 0; struct udirent d;
    while (sys_readdir(fd, &d) > 0 && n < 256) if (strcmp(d.name, ".") && strcmp(d.name, "..")) names[n++] = strdup(d.name);
    sys_close(fd);
    for (int i = 0; i < n; i++) { char c[256]; strcpy(c, p); strcat(c, "/"); strncpy(c + strlen(c), names[i], 100); remove_tree(c); free(names[i]); }
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
        char target[256];
        if (todir) { const char *b = strrchr(argv[i], '/'); b = b && b[1] ? b + 1 : argv[i]; strcpy(target, dest); if (target[strlen(target) - 1] != '/') strcat(target, "/"); strncpy(target + strlen(target), b, 100); }
        else strcpy(target, dest);
        if (sys_rename(argv[i], target) < 0) {
            if (copy_tree(argv[i], target) < 0 || remove_tree(argv[i]) < 0) { fprintf(2, "mv: cannot move %s\n", argv[i]); rc = 1; continue; }
        }
        if (o_v) printf("'%s' -> '%s'\n", argv[i], target);
    }
    return rc;
}
