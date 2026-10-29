/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* test server: port "ping" (or -port NAME); type 1 = add a[0]+a[1] into a[2]; type 2 = copy the client's string a[0]
 * (pointer into the client's memory) to upper case in place; type 3 = answer with its sys_svc_self() (the start count and
 * SVC_MIDREQ: audit A8's test); type 7 = wait a second, then crash (an illegal
 * instruction: the service manager's tests, DESIGN D11); type 9 = quit. Exits after serving COUNT requests if given.
 * -call NAME: call port NAME once before creating its own (a start-up that needs another service: D11's tests).
 * -fs: a filesystem server that hangs on sync: FS_ROOT answers 2, FS_SYNC never returns, FS_QUIT ends it, anything
 * else fails (a reboot must not wait for it for ever: audit A7, tools/svc-test.sh). -fsstall: the same, but every
 * request other than FS_ROOT, FS_CLOSE and FS_QUIT never returns (a program that opens a file there is held in the
 * kernel until the server ends: wasabid's "freed", tools/remote-test.sh). -fsbad: the same, but FS_ROOT
 * fails too (a mount that fails must end its server: audit A9). -fslinked: a filesystem server that answers DESIGN D26's
 * FS_LINKED wrongly: FS_OPEN of anything but its root and /f -1025 (FSL_A1 for a one-path request: outside the table),
 * FS_READ and FS_READDIR -1024 (requests that never ask for it), anything else -1024 with the path left as it is (a
 * valid answer: the kernel goes on at that text, from /); its root opens as a directory, /f as a file; FS_ROOT 2,
 * FS_SYNC and FS_CLOSE 0, FS_QUIT ends it. */
#define FS_OPEN 1
#define FS_READDIR 6
#define FS_CLOSE 24
#define FS_SYNC 13
#define FS_ROOT 22
#define FS_QUIT 23
int main(int argc, char **argv)
{
    const char *pname = "ping";
    int left = -1, fs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-port") && i + 1 < argc) pname = argv[++i];
        else if (!strcmp(argv[i], "-fs")) fs = 1;
        else if (!strcmp(argv[i], "-fsbad")) fs = 2;
        else if (!strcmp(argv[i], "-fsstall")) fs = 4;
        else if (!strcmp(argv[i], "-fslinked")) fs = 3;
        else if (!strcmp(argv[i], "-call") && i + 1 < argc) {
            struct umsg c;
            memset(&c, 0, sizeof c);
            c.type = 1;
            int r = sys_call(sys_port_find(argv[++i]), &c);
            if (r < 0) { dprintf(1, "msgpingd: %s: the call to %s failed (%d)\n", pname, argv[i], r); return 1; }
        } else left = atoi(argv[i]);
    }
    int port = sys_port_create(pname);
    if (port < 0) { dprintf(1, "msgpingd: port busy\n"); return 1; }
    struct umsg *m = sys_recv(port, 1);
    for (;;) {
        if ((int)m == -1) return 1;
        int r = 0;
        if (fs) {
            if (m->type == FS_SYNC && fs != 3) for (;;) sys_sleep(100);
            if (fs == 4 && m->type != FS_ROOT && m->type != FS_CLOSE && m->type != FS_QUIT) for (;;) sys_sleep(100);
            if (m->type == FS_QUIT) { sys_reply(m, 0); return 0; }
            int root = fs == 3 && m->type == FS_OPEN && !strcmp((const char *)m->a[0], "/");
            if (root || (fs == 3 && m->type == FS_OPEN && !strcmp((const char *)m->a[0], "/f"))) {
                u32 *st = (u32 *)m->a[3];               /* struct ustat: ino, mode, size, mtime, uid, gid, links */
                memset(st, 0, 28);
                st[0] = root ? 2 : 3; st[1] = root ? 040755 : 0100644; st[2] = root ? 0 : 10; st[6] = 1;
                r = (int)st[0];
            } else if (fs == 3)
                r = m->type == FS_ROOT ? 2 : m->type == FS_SYNC || m->type == FS_CLOSE ? 0 : m->type == FS_OPEN ? -1025 : -1024;
            else
                r = m->type == FS_ROOT && (fs == 1 || fs == 4) ? 2 : -38;
            m = sys_replyrecv(m, r, port);
            continue;
        }
        if (m->type == 1) m->a[2] = m->a[0] + m->a[1];
        else if (m->type == 2) { char *s = (char *)m->a[0]; for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }
        else if (m->type == 3) r = sys_svc_self();
        else if (m->type == 7) { sys_sleep(100); __asm__ volatile("illegal"); }
        else if (m->type == 9) { sys_reply(m, 0); return 0; }
        else r = -1;
        if (left > 0 && --left == 0) { sys_reply(m, r); return 0; }
        m = sys_replyrecv(m, r, port);
    }
}
