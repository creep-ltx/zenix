#include <stdio.h>
#include <signal.h>
#include <unistd.h>

/* Spins forever printing dots - until Ctrl-C delivers SIGINT and the
 * handler fires. */
static void on_int(int sig)
{
    (void)sig;
    write(1, "\nspin: caught SIGINT, exiting cleanly\n", 38);
    _exit(0);
}

int main(void)
{
    signal(SIGINT, on_int);
    printf("spin: looping - press Ctrl-C\n");
    fflush(stdout);
    for (unsigned n = 0;; n++) {
        if ((n & 0xFFFFF) == 0)
            write(1, ".", 1);
    }
}
