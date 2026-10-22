#include "libz.h"
/* ln [-s] [-f] target link_name   |   ln [-s] [-f] target... directory */
int main(int argc, char **argv)
{
    int sym = 0, force = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) for (const char *p = argv[i] + 1; *p; p++) { if (*p == 's') sym = 1; else if (*p == 'f') force = 1; else { fprintf(2, "ln: unknown option -%c\n", *p); return 2; } }
    int n = argc - i;
    if (n < 2) { fprintf(2, "usage: ln [-sf] target link_name\n"); return 2; }
    struct ustat st; int rc = 0;
    int todir = n > 2 || (sys_stat(argv[argc - 1], &st) == 0 && (st.mode & S_IFMT) == S_IFDIR);
    for (; i < argc - 1; i++) {
        char link[200];
        strcpy(link, argv[argc - 1]);
        if (todir) { const char *b = strrchr(argv[i], '/'); b = b ? b + 1 : argv[i]; if (link[strlen(link) - 1] != '/') strcat(link, "/"); strcat(link, b); }
        if (force) sys_unlink(link);
        int r = sym ? sys_symlink(argv[i], link) : sys_link(argv[i], link);
        if (r < 0) { fprintf(2, "ln: cannot create %s\n", link); rc = 1; }
    }
    return rc;
}
