#include "libz.h"
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(2, "usage: mv from to\n"); return 2; }
    struct ustat st;
    char to[128];
    strcpy(to, argv[2]);
    if (sys_stat(to, &st) == 0 && (st.mode & S_IFMT) == S_IFDIR) {
        const char *b = argv[1];
        for (const char *q = argv[1]; *q; q++) if (*q == '/') b = q + 1;
        u32 l = strlen(to);
        if (l && to[l - 1] != '/') to[l++] = '/';
        strcpy(to + l, b);
    }
    int r = sys_rename(argv[1], to);
    if (r < 0) { fprintf(2, "mv: cannot move %s (directories only within one parent)\n", argv[1]); return 1; }
    return 0;
}
