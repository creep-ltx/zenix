#include "libz.h"
/* Embedded first program: waits for the filesystem server, then starts
 * /bin/init and exits. */
int main(void)
{
    int tries = 0;
    while (sys_port_find("fs") < 0) {
        if (++tries > 600) { printf("boot: the filesystem server did not start\n"); return 1; }
        sys_sleep(5);
    }
    char *argv[] = { "/bin/init", 0 };
    if (sys_spawn("/bin/init", argv, 0) < 0) { printf("boot: cannot start /bin/init\n"); return 1; }
    return 0;
}
