#include "libz.h"
/* du [-s] [-a] [-h] [-k] [path...]: space used, in 1 KB blocks (file sizes rounded up) */
static int o_s, o_a, o_h;
static void show(u32 kb, const char *p)
{
    if (o_h) { if (kb >= 1024) oprintf("%u.%uM\t%s\n", kb / 1024, (kb % 1024) * 10 / 1024, p); else oprintf("%uK\t%s\n", kb, p); }
    else oprintf("%u\t%s\n", kb, p);
}
static u32 walk(const char *path, int top)
{
    struct ustat st;
    if (sys_stat(path, &st) < 0) return 0;
    u32 kb = (st.size + 1023) / 1024;
    if ((st.mode & S_IFMT) == S_IFDIR) {
        kb = 1;
        int fd = sys_open(path, O_RDONLY);
        if (fd >= 0) {
            char *names[256]; int n = 0; struct udirent d;
            while (sys_readdir(fd, &d) > 0 && n < 256) if (strcmp(d.name, ".") && strcmp(d.name, "..")) names[n++] = strdup(d.name);
            sys_close(fd);
            for (int i = 0; i < n; i++) { char c[256]; strcpy(c, path); u32 l = strlen(c); if (l && c[l - 1] != '/') strcat(c, "/"); strncpy(c + strlen(c), names[i], 100); kb += walk(c, 0); free(names[i]); }
        }
        if (!o_s || top) show(kb, path);
    } else if (o_a && !o_s) show(kb, path);
    return kb;
}
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) for (const char *p = argv[i] + 1; *p; p++) { if (*p == 's') o_s = 1; else if (*p == 'a') o_a = 1; else if (*p == 'h') o_h = 1; else if (*p != 'k') { fprintf(2, "du: unknown option -%c\n", *p); return 2; } }
    if (i >= argc) walk(".", 1); else for (; i < argc; i++) walk(argv[i], 1);
    return 0;
}
