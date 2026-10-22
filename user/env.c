#include "libz.h"
/* env [NAME=value...] [command [args...]]: print the environment, or run a command with changes */
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && strchr(argv[i], '='); i++) { char *e = strdup(argv[i]); char *eq = strchr(e, '='); *eq = 0; setenv(e, eq + 1, 1); }
    if (i < argc) {
        char path[128];
        if (strchr(argv[i], '/')) strcpy(path, argv[i]); else { strcpy(path, "/bin/"); strcat(path, argv[i]); }
        int pid = sys_spawn(path, argv + i, 0);
        if (pid < 0) { fprintf(2, "env: %s: cannot run\n", argv[i]); return 127; }
        return sys_wait(pid);
    }
    getenv("");
    for (char **e = environ; e && *e; e++) printf("%s\n", *e);
    return 0;
}
