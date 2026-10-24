// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
#include "../kernel/diskproto.h"
/* svctest: a test program for the service manager (DESIGN D11, tools/svc-test.sh). Each use prints one line starting
 * with "svctest " and the result.
 *   svctest port NAME              try to create port NAME (a registered name: -16 for anyone, root too)
 *   svctest reg NAME NICE PROG...  register service NAME (root)
 *   svctest rm NAME                remove it (root)
 *   svctest call NAME TYPE [A0]    one call to port NAME: the result and how long it took (E-clock, ms)
 *   svctest raw DEVICE             open DEVICE raw at the disk server (-16 while it is mounted)
 *   svctest hold DEVICE SECS       open DEVICE, read its sector 0, wait SECS seconds, read it again with the same handle
 *                                  (a diskd restarted meanwhile must still know the handle) */
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
    if (argc >= 3 && (!strcmp(argv[1], "raw") || !strcmp(argv[1], "hold"))) {
        static char sec[512];
        int dp = sys_port_find("disk"), raw = argv[1][0] == 'r';
        struct umsg m;
        m.type = DISK_OPEN; m.a[0] = (u32)argv[2]; m.a[1] = raw ? DISK_RAW : 0;
        int h = sys_call(dp, &m);
        if (raw) { printf("svctest raw %s: %d\n", argv[2], h); return 0; }
        m.type = DISK_READ; m.a[0] = (u32)h; m.a[1] = 0; m.a[2] = 1; m.a[3] = (u32)sec;
        int r1 = sys_call(dp, &m);
        sys_sleep(argc > 3 ? (u32)atoi(argv[3]) * 100 : 500);
        m.type = DISK_READ; m.a[0] = (u32)h; m.a[1] = 0; m.a[2] = 1; m.a[3] = (u32)sec;
        int r2 = sys_call(dp, &m);
        printf("svctest hold %s: handle %d, read %d, after the wait %d\n", argv[2], h, r1, r2);
        return 0;
    }
    printf("usage: svctest port|reg|rm|call|raw|hold ...\n");
    return 2;
}
