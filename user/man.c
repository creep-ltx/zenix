#include "libz.h"
/* man [-k word] name...: show a manual page (in the pager); -k lists the pages whose text mentions the word */
#define DIR "/usr/man/"
static int show(const char *name)
{
    char path[100]; snprintf(path, sizeof path, DIR "%s.txt", name);
    struct ustat st;
    if (sys_stat(path, &st) < 0) { fprintf(2, "man: no manual entry for %s\n", name); return 1; }
    char *av[3] = { "less", path, 0 };
    int pid = sys_spawn("/bin/less", av, 0);
    if (pid < 0) { fprintf(2, "man: cannot run the pager\n"); return 1; }
    sys_setfg(pid); int rc = sys_wait(pid); sys_setfg(0);
    return rc;
}
static int keyword(const char *w)
{
    int fd = sys_open(DIR, O_RDONLY); if (fd < 0) return 1;
    char *names[400]; int n = 0; struct udirent d;
    while (sys_readdir(fd, &d) > 0 && n < 400) if (strstr(d.name, ".txt")) names[n++] = strdup(d.name);
    sys_close(fd);
    for (int i = 1; i < n; i++) { char *t = names[i]; int j = i; while (j > 0 && strcmp(names[j - 1], t) > 0) { names[j] = names[j - 1]; j--; } names[j] = t; }
    int found = 0;
    for (int i = 0; i < n; i++) {
        char path[100]; snprintf(path, sizeof path, DIR "%s", names[i]);
        int f = sys_open(path, O_RDONLY); if (f < 0) continue;
        char text[2048]; int len = sys_read(f, text, sizeof text - 1); sys_close(f); if (len < 0) len = 0; text[len] = 0;
        char lw[40], lt[2048]; u32 k;
        for (k = 0; w[k] && k < 39; k++)
            lw[k] = (char)tolower((unsigned char)w[k]);
        lw[k] = 0;
        for (int q = 0; q <= len; q++) lt[q] = (char)tolower((unsigned char)text[q]);
        if (strstr(lt, lw)) {
            char *sum = strstr(text, "SYNOPSIS AND DESCRIPTION"); sum = sum ? strchr(sum, '\n') : 0;
            char line[100]; k = 0; if (sum) { sum++; while (*sum == ' ') sum++; while (*sum && *sum != '\n' && k < 90) line[k++] = *sum++; } line[k] = 0;
            char nm[40]; strncpy(nm, names[i], 39); nm[39] = 0; char *dot = strstr(nm, ".txt"); if (dot) *dot = 0;
            printf("%-14s %s\n", nm, line); found = 1;
        }
    }
    return found ? 0 : 1;
}
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(2, "usage: man name...   |   man -k word\n"); return 1; }
    if (!strcmp(argv[1], "-k")) { if (argc < 3) return 1; return keyword(argv[2]); }
    int rc = 0;
    for (int i = 1; i < argc; i++) rc |= show(argv[i]);
    return rc;
}
