#include "libz.h"

/* pid 1: print the banner, run the shell, run it again when it exits */
int main(void)
{
    char *sh[] = { "/bin/sh", 0 };
    int fd = sys_open("/etc/motd", 0);
    if (fd >= 0) {
        char b[256]; int n;
        while ((n = sys_read(fd, b, sizeof b)) > 0) sys_write(1, b, (u32)n);
        sys_close(fd);
    }
    for (;;) {
        int pid = sys_spawn("/bin/sh", sh, 0);
        if (pid < 0) { printf("init: cannot start /bin/sh\n"); sys_sleep(500); continue; }
        sys_setfg(0);
        sys_wait(pid);
        printf("init: shell exited, starting another\n");
    }
}
