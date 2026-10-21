#include "libz.h"
int main(int argc, char **argv)
{
    int fd = argc > 1 ? sys_open(argv[1], 0) : 0;
    if (fd < 0) { fprintf(2, "wc: cannot open\n"); return 1; }
    char b[512]; int n, inw = 0;
    u32 lines = 0, words = 0, bytes = 0;
    while ((n = sys_read(fd, b, sizeof b)) > 0)
        for (int i = 0; i < n; i++) {
            bytes++;
            if (b[i] == '\n') lines++;
            int sp = b[i] == ' ' || b[i] == '\n' || b[i] == '\t';
            if (!sp && !inw) words++;
            inw = !sp;
        }
    printf("%u %u %u\n", lines, words, bytes);
    return 0;
}
