#include "libz.h"
/* rm [-f] [-r] [-v] file... : remove files; -r removes directories and what is in them */
static int o_f, o_r, o_v, rc;
static void rmtree(const char *path, int top)
{
    struct ustat st;
    if (sys_lstat(path, &st) < 0) { if (!o_f) { fprintf(2, "rm: %s: not found\n", path); rc = 1; } return; }
    if ((st.mode & S_IFMT) == S_IFDIR) {
        if (!o_r) { fprintf(2, "rm: %s: is a directory (use -r)\n", path); rc = 1; return; }
        int fd = sys_open(path, O_RDONLY);
        if (fd >= 0) {
            char *names[256]; int n = 0; struct udirent d;
            while (sys_readdir(fd, &d) > 0 && n < 256) if (strcmp(d.name, ".") && strcmp(d.name, "..")) names[n++] = strdup(d.name);
            sys_close(fd);
            for (int i = 0; i < n; i++) { char c[256]; if (strlen(path) + strlen(names[i]) + 2 < sizeof c) { strcpy(c, path); if (c[strlen(c) - 1] != '/') strcat(c, "/"); strcat(c, names[i]); rmtree(c, 0); } free(names[i]); }
        }
        if (sys_rmdir(path) < 0) { fprintf(2, "rm: %s: %s\n", path, zwhy("cannot remove")); rc = 1; } else if (o_v) printf("removed directory '%s'\n", path);
    } else {
        if (sys_unlink(path) < 0) { fprintf(2, "rm: %s: %s\n", path, zwhy("cannot remove")); rc = 1; } else if (o_v) printf("removed '%s'\n", path);
    }
    (void)top;
}
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'f') o_f = 1; else if (*p == 'r' || *p == 'R') o_r = 1; else if (*p == 'v') o_v = 1; else { fprintf(2, "rm: unknown option -%c\n", *p); return 2; } }
    }
    if (i >= argc) { if (!o_f) { fprintf(2, "usage: rm [-frv] file...\n"); return 2; } return 0; }
    for (; i < argc; i++) rmtree(argv[i], 1);
    return rc;
}
