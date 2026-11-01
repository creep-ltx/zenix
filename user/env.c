/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* env [NAME=value...] [command [args...]]: print the environment, or run a command with changes */
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && strchr(argv[i], '='); i++) { char *e = strdup(argv[i]); char *eq = strchr(e, '='); *eq = 0; setenv(e, eq + 1, 1); }
    if (i < argc) {
        char path[ZPATH_MAX];
        int slash = strchr(argv[i], '/') != 0;              /* with a slash as it is: the kernel judges its length */
        if (!slash && strlen(argv[i]) + 6 > sizeof path) { dprintf(2, "env: %s: file name too long\n", argv[i]); return 127; }
        if (!slash) { strcpy(path, "/bin/"); strcat(path, argv[i]); }
        int pid = sys_spawn(slash ? argv[i] : path, argv + i, 0);
        if (pid < 0) { dprintf(2, "env: %s: cannot run\n", argv[i]); return 127; }
        return sys_wait(pid);
    }
    getenv("");
    for (char **e = environ; e && *e; e++) dprintf(1, "%s\n", *e);
    return 0;
}
