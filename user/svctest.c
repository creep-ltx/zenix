// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* svctest: a test program for the service manager (DESIGN D11, tools/svc-test.sh). Each use prints one line starting
 * with "svctest " and the result.
 *   svctest port NAME              try to create port NAME (a registered name: -16 for anyone, root too)
 *   svctest reg NAME NICE PROG...  register service NAME (root)
 *   svctest rm NAME                remove it (root)
 *   svctest call NAME TYPE [A0]    one call to port NAME: the result and how long it took (E-clock, ms) */
#define E_MS 709u

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "port")) {
        printf("svctest port %s: %d\n", argv[2], sys_port_create(argv[2]));
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "reg")) {
        char text[SVC_TEXT_MAX];
        text[0] = 0;
        for (int i = 4; i < argc; i++) {
            if (strlen(text) + strlen(argv[i]) + 2 > sizeof text) { printf("svctest reg: too long\n"); return 1; }
            if (i > 4) strcat(text, " ");
            strcat(text, argv[i]);
        }
        printf("svctest reg %s: %d\n", argv[2], sys_svc_register(argv[2], text, atoi(argv[3])));
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "rm")) {
        printf("svctest rm %s: %d\n", argv[2], sys_svc_remove(argv[2]));
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "call")) {
        int port = sys_port_find(argv[2]);
        struct umsg m;
        memset(&m, 0, sizeof m);
        m.type = (u32)atoi(argv[3]);
        m.a[0] = argc > 4 ? (u32)atoi(argv[4]) : 0;
        u32 t0 = sys_hrtime();
        int r = port < 0 ? port : sys_call(port, &m);
        printf("svctest call %s %s: %d in %u ms\n", argv[2], argv[3], r, (sys_hrtime() - t0) / E_MS);
        return 0;
    }
    printf("usage: svctest port|reg|rm|call ...\n");
    return 2;
}
