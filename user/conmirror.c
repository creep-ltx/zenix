/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"

/* conmirror off|on: stop or start the serial copy of the console on the screen (SYS_ttymirror, root only). With the
 * copy off the screen never waits for the 115200-baud port; the kernel's own log still goes to the port. */
static void say(const char *s) { sys_write(2, s, strlen(s)); }

int main(int argc, char **argv)
{
    int on = argc == 2 && !strcmp(argv[1], "on"), off = argc == 2 && !strcmp(argv[1], "off");
    if (!on && !off) {
        say("usage: conmirror off|on\n");
        return 2;
    }
    int r = sys_ttymirror(on);
    if (r < 0) {
        say(r == -EACCES ? "conmirror: only root can switch the serial copy\n" : "conmirror: refused by the kernel\n");
        return 1;
    }
    return 0;
}
