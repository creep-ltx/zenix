/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* cu: call up another computer through the serial port (DESIGN D12), as BSD's and Linux's cu. Keys go to the port, the
 * port's bytes to the screen; "~." typed at the start of a line ends it, "~~" sends one "~". Two directions without
 * select: the port-to-screen copy is a second cu (cu -r), started with the port as its standard input. */

static int copy_in(void)                    /* cu -r: the port (stdin) to the screen (stdout), until killed */
{
    static char buf[256];
    for (;;) {
        int n = sys_read(0, buf, sizeof buf);
        if (n <= 0)
            return n < 0 ? 1 : 0;
        if (sys_write(1, buf, (u32)n) < 0)
            return 1;
    }
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/ttyS0";
    if (argc == 2 && !strcmp(argv[1], "-r"))
        return copy_in();
    if (argc == 3 && !strcmp(argv[1], "-l"))
        dev = argv[2];
    else if (argc != 1) {
        fprintf(2, "usage: cu [-l device]\n");
        return 2;
    }
    int port = sys_open(dev, O_RDWR);
    if (port < 0) {
        fprintf(2, "cu: %s: %s\n", dev, zstrerror(port) ? zstrerror(port) : "cannot open");
        return 1;
    }
    char *cargv[] = { "cu", "-r", 0 };
    int io[3] = { port, 1, 2 };
    int child = sys_spawn("/bin/cu", cargv, io);
    if (child < 0) {
        fprintf(2, "cu: cannot start the receiving half: %s\n", zwhy("failed"));
        sys_close(port);
        return 1;
    }
    printf("Connected to %s. Type ~. at the start of a line to end.\r\n", dev);
    sys_ttymode(TTY_RAW | TTY_NOSIG);           /* every key as it is, Ctrl-C included, goes to the other side */
    int bol = 1, tilde = 0;
    for (;;) {
        char c;
        if (sys_read(0, &c, 1) != 1)
            break;
        if (tilde) {                            /* the byte after a ~ at the start of a line */
            tilde = 0;
            if (c == '.')
                break;
            if (c != '~')
                sys_write(port, "~", 1);
            sys_write(port, &c, 1);
            bol = 0;
            continue;
        }
        if (bol && c == '~') {
            tilde = 1;
            continue;
        }
        sys_write(port, &c, 1);
        bol = c == '\r' || c == '\n' || c == 3 || c == 21;     /* Return, Ctrl-C and Ctrl-U start a new line */
    }
    sys_ttymode(0);
    sys_kill(child);
    sys_wait(child);
    sys_close(port);
    printf("\r\n[cu: disconnected from %s]\n", dev);
    return 0;
}
