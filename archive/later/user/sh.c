/* zsh - the Zenix shell. Line editing over the serial console, tokenized
 * commands, builtins, and fork/exec/wait for programs on disk. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "usys.h"

#define MAXTOK 8

static int readline(char *buf, int max)
{
    int len = 0;
    for (;;) {
        char c;
        if (read(0, &c, 1) != 1)
            continue;
        if (c == '\r' || c == '\n') {
            write(1, "\r\n", 2);
            buf[len] = 0;
            return len;
        }
        if (c == 8 || c == 127) {
            if (len) {
                len--;
                write(1, "\b \b", 3);
            }
            continue;
        }
        if ((unsigned char)c >= 32 && c != 127 && len < max - 1) {
            buf[len++] = c;             /* Latin-1 passes through */
            write(1, &c, 1);
        }
    }
}

static void run(char *av[])
{
    /* "hello" -> "HELLO.ELF" unless the name already has a dot */
    char path[16];
    int i, dot = 0;
    for (i = 0; av[0][i] && i < 11; i++) {
        char c = av[0][i];
        if (c == '.')
            dot = 1;
        path[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    path[i] = 0;
    if (!dot)
        strcat(path, ".ELF");

    fflush(stdout);
    long pid = sys_fork();
    if (pid == 0) {
        sys_exec(path, av);
        printf("zsh: %s: not found\n", path);
        fflush(stdout);
        sys_exit(127);
    }
    if (pid < 0) {
        printf("zsh: fork failed\n");
        return;
    }
    sys_setfg(pid);             /* Ctrl-C goes to the child */
    int status = 0;
    while (sys_wait(&status) < 0)
        ;                       /* interrupted by a signal: keep waiting */
    sys_setfg(getpid());
    if (status)
        printf("[exit %d]\n", status);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    printf("zsh: Zenix shell on pid %d - 'help' for help\n", getpid());
    fflush(stdout);
    sys_setfg(getpid());

    char line[128];
    char *av[MAXTOK + 1];
    for (;;) {
        write(1, "zenix# ", 7);
        if (readline(line, sizeof line) == 0)
            continue;

        int ac = 0;
        char *p = line;
        while (*p && ac < MAXTOK) {
            while (*p == ' ')
                *p++ = 0;
            if (!*p)
                break;
            av[ac++] = p;
            while (*p && *p != ' ')
                p++;
        }
        av[ac] = 0;
        if (!ac)
            continue;

        if (!strcmp(av[0], "exit"))
            return 0;
        if (!strcmp(av[0], "help")) {
            printf("builtins: help exit - programs: ls cat echo hello\n");
            fflush(stdout);
            continue;
        }
        run(av);
    }
}
