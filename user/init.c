#include "libz.h"

/* pid 1: print the banner, run the shell, run it again when it exits */
int main(void)
{
    char *sh[] = { "/bin/login", 0 };
    int fd = sys_open("/etc/motd", 0);
    if (fd >= 0) {
        char b[256]; int n;
        while ((n = sys_read(fd, b, sizeof b)) > 0) sys_write(1, b, (u32)n);
        sys_close(fd);
    }
    int fs = sys_open("/etc/fstab", 0);                 /* mount what /etc/fstab lists (see mount(8)) */
    if (fs >= 0) {
        sys_close(fs);
        char *ma[] = { "/bin/mount", "-a", 0 };
        int mp = sys_spawn("/bin/mount", ma, 0);
        if (mp >= 0) sys_wait(mp);
    }
    for (;;) {
        int pid = sys_spawn("/bin/login", sh, 0);
        if (pid < 0) { printf("init: cannot start /bin/login\n"); sys_sleep(500); continue; }
        sys_setfg(0);
        sys_wait(pid);
        sys_session(0xFFFFFFFFu, 0);            /* nobody is logged in any more: Amiga+Return opens nothing now */
        printf("init: session ended, starting another\n");
    }
}
