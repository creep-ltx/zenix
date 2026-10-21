#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: cat FILE...\n");
        return 1;
    }
    char buf[512];
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("cat: %s: not found\n", argv[i]);
            return 1;
        }
        int n;
        while ((n = read(fd, buf, sizeof buf)) > 0)
            write(1, buf, n);
        close(fd);
    }
    return 0;
}
