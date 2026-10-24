/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* test server: port "ping" (or -port NAME); type 1 = add a[0]+a[1] into a[2]; type 2 = copy the client's string a[0]
 * (pointer into the client's memory) to upper case in place; type 7 = wait a second, then crash (an illegal
 * instruction: the service manager's tests, DESIGN D11); type 9 = quit. Exits after serving COUNT requests if given.
 * -call NAME: call port NAME once before creating its own (a start-up that needs another service: D11's tests). */
int main(int argc, char **argv)
{
    const char *pname = "ping";
    int left = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-port") && i + 1 < argc) pname = argv[++i];
        else if (!strcmp(argv[i], "-call") && i + 1 < argc) {
            struct umsg c;
            memset(&c, 0, sizeof c);
            c.type = 1;
            int r = sys_call(sys_port_find(argv[++i]), &c);
            if (r < 0) { printf("pingd: %s: the call to %s failed (%d)\n", pname, argv[i], r); return 1; }
        } else left = atoi(argv[i]);
    }
    int port = sys_port_create(pname);
    if (port < 0) { printf("pingd: port busy\n"); return 1; }
    struct umsg *m = sys_recv(port, 1);
    for (;;) {
        if ((int)m == -1) return 1;
        int r = 0;
        if (m->type == 1) m->a[2] = m->a[0] + m->a[1];
        else if (m->type == 2) { char *s = (char *)m->a[0]; for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }
        else if (m->type == 7) { sys_sleep(100); __asm__ volatile("illegal"); }
        else if (m->type == 9) { sys_reply(m, 0); return 0; }
        else r = -1;
        if (left > 0 && --left == 0) { sys_reply(m, r); return 0; }
        m = sys_replyrecv(m, r, port);
    }
}
