/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* env [NAME=value...] [command [args...]]: print the environment, or run a command with changes */
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && strchr(argv[i], '='); i++) { char *e = strdup(argv[i]); char *eq = strchr(e, '='); *eq = 0; setenv(e, eq + 1, 1); }
    if (i < argc) {
        char *path = strchr(argv[i], '/') ? argv[i] : path_join("/bin", argv[i]);   /* with a slash as it is: the kernel judges its length */
        int pid = path ? sys_spawn(path, argv + i, 0) : -ENAMETOOLONG;
        if (pid < 0) { dprintf(2, "env: %s: %s\n", argv[i], path ? zwhy("cannot run") : "file name too long"); return 127; }
        return sys_wait(pid);
    }
    getenv("");
    for (char **e = environ; e && *e; e++) dprintf(1, "%s\n", *e);
    return 0;
}
