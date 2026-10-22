#include "libz.h"
/* Built into the kernel image: the session manager. Waits for the
 * filesystem server, starts /bin/init, then stays and opens a shell on a
 * workspace whenever the user asks for one (Amiga+Return). */
int main(void)
{
    int tries = 0;
    while (sys_port_find("fs") < 0) {
        if (++tries > 600) { printf("boot: the filesystem server did not start\n"); return 1; }
        sys_sleep(5);
    }
    char *argv[] = { "/bin/init", 0 };
    if (sys_spawn("/bin/init", argv, 0) < 0) { printf("boot: cannot start /bin/init\n"); return 1; }
    for (;;) {
        int w = sys_wsreq();
        if (w < 0) return 1;
        char *sh[] = { "/bin/sh", 0 };
        sys_spawn_tty("/bin/sh", sh, w);
    }
}
