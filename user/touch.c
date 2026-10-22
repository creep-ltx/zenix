#include "libz.h"
int main(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) { fprintf(2, "touch: %s: cannot create\n", argv[i]); rc = 1; }
        else sys_close(fd);
    }
    return rc;
}
