/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"

/* input: show the mouse (port 1) and joystick (port 2) state; -w SECONDS watches and prints each change */
int main(int argc, char **argv)
{
    int secs = (argc > 2 && !strcmp(argv[1], "-w")) ? atoi(argv[2]) : 0;
    int last[4] = { 0, 0, -1, -1 };
    u32 end = sys_ticks() + (u32)secs * 100;
    for (;;) {
        int s[4];
        sys_input(s);
        if (!secs || s[0] || s[1] || s[2] != last[2] || s[3] != last[3]) {
            dprintf(1, "mouse dx %d dy %d buttons %c%c%c joystick %c%c%c%c%c\n", s[0], s[1],
                   (s[2] & 1) ? 'L' : '-', (s[2] & 2) ? 'R' : '-', (s[2] & 4) ? 'M' : '-',
                   (s[3] & 1) ? 'U' : '-', (s[3] & 2) ? 'D' : '-', (s[3] & 4) ? 'L' : '-',
                   (s[3] & 8) ? 'R' : '-', (s[3] & 16) ? 'F' : '-');
            last[2] = s[2]; last[3] = s[3];
        }
        if (!secs || (int)(sys_ticks() - end) >= 0)
            break;
        sys_sleep(5);
    }
    return 0;
}
