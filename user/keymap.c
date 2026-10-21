#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "usys.h"

/* keymap [us|sv] — switch the console keyboard layout, live and
 * persistently (KEYMAP.CFG is read by the kernel at boot). */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: keymap us|sv\n");
        return 1;
    }
    if (__sys(SYS_setkmap, (long)argv[1], 0, 0) < 0) {
        printf("keymap: unknown layout '%s' (have: us sv)\n", argv[1]);
        return 1;
    }
    int fd = open("KEYMAP.CFG", O_WRONLY);
    if (fd >= 0) {
        write(fd, argv[1], strlen(argv[1]));
        close(fd);
    }
    printf("keymap: %s (persisted)\n", argv[1]);
    return 0;
}
