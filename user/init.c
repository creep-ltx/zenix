#include "usys.h"

/* Zenix init (pid 1): spawn the shell, respawn it if it ever exits. */
int main(void)
{
    uputs("\ninit: Zenix userland is alive, pid ");
    uputnum(sys_getpid());
    uputs("\n");

    for (;;) {
        long pid = sys_fork();
        if (pid == 0) {
            sys_exec("SH.ELF", 0);
            uputs("init: cannot exec SH.ELF!\n");
            sys_exit(1);
        }
        while (sys_wait(0) != pid)
            ;
        uputs("init: shell exited, respawning\n");
    }
}
