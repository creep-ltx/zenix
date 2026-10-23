/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* test server: port "ping"; type 1 = add a[0]+a[1] into a[2]; type 2 = copy
 * the client's string a[0] (pointer into the client's memory) to upper case
 * in place; type 9 = quit. Exits after serving `argv[1]` requests if given. */
int main(int argc, char **argv)
{
    int port = sys_port_create("ping");
    if (port < 0) { printf("pingd: port busy\n"); return 1; }
    int left = argc > 1 ? atoi(argv[1]) : -1;
    struct umsg *m = sys_recv(port, 1);
    for (;;) {
        if ((int)m == -1) return 1;
        int r = 0;
        if (m->type == 1) m->a[2] = m->a[0] + m->a[1];
        else if (m->type == 2) { char *s = (char *)m->a[0]; for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }
        else if (m->type == 9) { sys_reply(m, 0); return 0; }
        else r = -1;
        if (left > 0 && --left == 0) { sys_reply(m, r); return 0; }
        m = sys_replyrecv(m, r, port);
    }
}
