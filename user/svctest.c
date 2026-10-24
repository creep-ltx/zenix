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
 *                                  (a diskd restarted meanwhile must still know the handle)
 *   svctest keep FILE SECS         open FILE, read it, delete it, wait, read it again through the open file: the same
 *                                  bytes after a filesystem server restart (D11 5.4), then close it
 *   svctest kill PID SECS          wait SECS seconds, then kill PID twice (at once even mid-request); run at nice -20 it
 *                                  wakes while a busy server is in the middle of a request
 *   svctest lost FILE SECS [PID]   create FILE and write AAAA, kill PID (the filesystem server: it ends before its next
 *                                  commit), wait, read it through the open file (ZenixFS: the file is gone with the
 *                                  uncommitted seconds: an error, never another file) */
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
    if (argc >= 4 && !strcmp(argv[1], "keep")) {
        static char a[4096], b[4096];
        int fd = sys_open(argv[2], O_RDONLY);
        int n1 = fd < 0 ? fd : sys_read(fd, a, sizeof a), u = sys_unlink(argv[2]);
        sys_sleep((u32)atoi(argv[3]) * 100);
        sys_lseek(fd, 0, 0);
        int n2 = sys_read(fd, b, sizeof b);
        printf("svctest keep %s: read %d, deleted %d, again %d: %s\n", argv[2], n1, u, n2,
               n1 > 0 && n1 == n2 && !memcmp(a, b, (u32)n1) ? "the same bytes" : "DIFFERENT");
        sys_close(fd);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "kill")) {
        sys_sleep((u32)atoi(argv[3]) * 100);
        int a = sys_kill(atoi(argv[2])), b = sys_kill(atoi(argv[2]));
        printf("svctest kill %s: %d %d\n", argv[2], a, b);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "lost")) {
        char buf[8];
        int fd = sys_open(argv[2], O_RDWR | O_CREAT | O_TRUNC), w = fd < 0 ? fd : sys_write(fd, "AAAA", 4);
        if (argc > 4) sys_kill(atoi(argv[4]));
        sys_sleep((u32)atoi(argv[3]) * 100);
        sys_lseek(fd, 0, 0);
        memset(buf, 0, sizeof buf);
        int r = sys_read(fd, buf, 4);
        printf("svctest lost %s: wrote %d, read %d \"%s\"\n", argv[2], w, r, r > 0 ? buf : "");
        sys_close(fd);
        return 0;
    }
    printf("usage: svctest port|reg|rm|call|raw|hold|keep|lost ...\n");
    return 2;
}
