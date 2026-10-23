/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* Built into the kernel image: the starter. Waits for the filesystem
 * server, starts /bin/init on console 1 and waits for it; should init
 * ever end, it says so and starts it again. (It exists because the idle
 * process cannot block on a message.) */
int main(void)
{
    int tries = 0;
    while (sys_port_find("fs") < 0) {
        if (++tries > 600) { printf("boot: the filesystem server did not start\n"); return 1; }
        sys_sleep(5);
    }
    char *argv[] = { "/bin/init", 0 };
    for (;;) {
        int pid = sys_spawn("/bin/init", argv, 0);
        if (pid < 0) { printf("boot: cannot start /bin/init\n"); sys_sleep(500); continue; }
        int st = sys_wait(pid);
        printf("boot: /bin/init ended (status %d), starting it again\n", st);
        sys_sleep(100);
    }
}
