#include "libz.h"

struct ent { char name[64]; u32 mode, size; };
#define MAXE 128

static void modestr(u32 m, char *s)
{
    u32 t = m & S_IFMT;
    s[0] = t == S_IFDIR ? 'd' : t == S_IFLNK ? 'l' : '-';
    for (int i = 0; i < 9; i++)
        s[1 + i] = (m & (0400u >> i)) ? "rwxrwxrwx"[i] : '-';
    s[10] = 0;
}

static int ls(const char *path, int lflag, int multi)
{
    struct ustat st;
    if (sys_stat(path, &st) < 0) { fprintf(2, "ls: %s: not found\n", path); return 1; }
    static struct ent e[MAXE];
    int n = 0;
    if ((st.mode & S_IFMT) != S_IFDIR) {
        strcpy(e[0].name, path); e[0].mode = st.mode; e[0].size = st.size; n = 1;
    } else {
        int fd = sys_open(path, 0);
        struct udirent d;
        if (fd < 0) return 1;
        while (n < MAXE && sys_readdir(fd, &d) > 0) {
            if (d.name[0] == '.' && !lflag) continue;
            strcpy(e[n].name, d.name);
            char full[160];
            u32 pl = strlen(path);
            memcpy(full, path, pl);
            full[pl] = '/';
            strcpy(full + pl + 1, d.name);
            struct ustat s2;
            if (sys_stat(full, &s2) == 0) { e[n].mode = s2.mode; e[n].size = s2.size; }
            else { e[n].mode = 0; e[n].size = 0; }
            n++;
        }
        sys_close(fd);
        if (multi) printf("%s:\n", path);
    }
    for (int i = 1; i < n; i++)             /* insertion sort by name */
        for (int j = i; j > 0 && strcmp(e[j - 1].name, e[j].name) > 0; j--) {
            struct ent t = e[j]; e[j] = e[j - 1]; e[j - 1] = t;
        }
    for (int i = 0; i < n; i++) {
        if (lflag) {
            char m[11]; modestr(e[i].mode, m);
            printf("%s %8u %s\n", m, e[i].size, e[i].name);
        } else {
            printf("%s%s\n", e[i].name, (e[i].mode & S_IFMT) == S_IFDIR ? "/" : "");
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int lflag = 0, first = 1, rc = 0;
    if (argc > 1 && !strcmp(argv[1], "-l")) { lflag = 1; first = 2; }
    if (first >= argc) return ls(".", lflag, 0);
    for (int i = first; i < argc; i++) rc |= ls(argv[i], lflag, argc - first > 1);
    return rc;
}
