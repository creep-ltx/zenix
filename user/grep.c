#include "libz.h"
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(2, "usage: grep text [file]\n"); return 2; }
    int fd = argc > 2 ? sys_open(argv[2], 0) : 0;
    if (fd < 0) { fprintf(2, "grep: cannot open\n"); return 2; }
    static char line[512]; u32 len = 0; int n, found = 0; char c;
    while ((n = sys_read(fd, &c, 1)) > 0) {
        if (c != '\n' && len < sizeof line - 1) { line[len++] = c; continue; }
        line[len] = 0;
        if (strstr(line, argv[1])) { printf("%s\n", line); found = 1; }
        len = 0;
    }
    return found ? 0 : 1;
}
