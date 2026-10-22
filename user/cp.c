#include "libz.h"
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(2, "usage: cp from to\n"); return 2; }
    int in = sys_open(argv[1], O_RDONLY);
    if (in < 0) { fprintf(2, "cp: %s: cannot open\n", argv[1]); return 1; }
    struct ustat st;
    char to[128];
    strcpy(to, argv[2]);
    if (sys_stat(to, &st) == 0 && (st.mode & S_IFMT) == S_IFDIR) {     /* cp f dir/ */
        const char *b = argv[1];
        for (const char *q = argv[1]; *q; q++) if (*q == '/') b = q + 1;
        u32 l = strlen(to);
        if (l && to[l - 1] != '/') to[l++] = '/';
        strcpy(to + l, b);
    }
    int out = sys_open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { fprintf(2, "cp: %s: cannot write\n", to); return 1; }
    char b[512]; int n, rc = 0;
    while ((n = sys_read(in, b, sizeof b)) > 0)
        if (sys_write(out, b, (u32)n) != n) { fprintf(2, "cp: write failed (disk full?)\n"); rc = 1; break; }
    sys_close(in); sys_close(out);
    return rc;
}
