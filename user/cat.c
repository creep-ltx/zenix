#include "libz.h"
static int dump(int fd)
{
    char b[512]; int n;
    while ((n = sys_read(fd, b, sizeof b)) > 0)
        if (sys_write(1, b, (u32)n) < 0) return 1;
    return n < 0;
}
int main(int argc, char **argv)
{
    if (argc < 2) return dump(0);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], 0);
        if (fd < 0) { fprintf(2, "cat: %s: cannot open\n", argv[i]); rc = 1; continue; }
        rc |= dump(fd);
        sys_close(fd);
    }
    return rc;
}
