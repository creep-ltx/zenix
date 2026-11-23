// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
#include "../kernel/diskproto.h"
/* svctest: a test program for the service manager (DESIGN D11, tools/svc-test.sh). Each use prints one line starting
 * with "svctest " and the result.
 *   svctest port NAME              try to create port NAME (a registered name: -16 for anyone, root too)
 *   svctest ports SECS             create ordinary ports (u0, u1 ...) until refused, say how many, hold them SECS
 *                                  seconds (the held slots of the services are not among them: D11 5.2)
 *   svctest reg NAME NICE PROG...  register service NAME (root)
 *   svctest rm NAME                remove it (root)
 *   svctest call NAME TYPE [A0 [SECS]]  one call to port NAME: the result and how long it took (E-clock, ms); then
 *                                  wait SECS seconds (a kill after a failed call must end it at once: audit A1)
 *   svctest kcall PID NAME TYPE    kill PID (the root server), wait a second, then the same call: a file service is then
 *                                  read while the root server is stopped (two loads nested, D11 5.1)
 *   svctest raw DEVICE             open DEVICE raw at the disk server (-16 while it is mounted)
 *   svctest hold DEVICE SECS       open DEVICE, read its sector 0, wait SECS seconds, read it again with the same handle
 *                                  (a diskd restarted meanwhile must still know the handle)
 *   svctest keep FILE SECS         open FILE, read it, delete it, wait, read it again through the open file: the same
 *                                  bytes after a filesystem server restart (D11 5.4), then close it
 *   svctest kill PID SECS          wait SECS seconds, then kill PID twice (at once even mid-request); run at nice -20 it
 *                                  wakes while a busy server is in the middle of a request
 *   svctest reboot PID             kill PID (a server), wait half a second, then reboot (root): with the server dead and
 *                                  maybe a loader asleep in its back-off, the sync must not wait (D11 6.3)
 *   svctest mount PORT DIR         put the filesystem server on port PORT (already running) on DIR (root): a fake
 *                                  filesystem (pingd -fs) for the reboot's time limit (audit A7)
 *   svctest lost FILE SECS [PID]   create FILE and write AAAA, kill PID (the filesystem server: it ends before its next
 *                                  commit), wait, read it through the open file (ZenixFS: the file is gone with the
 *                                  uncommitted seconds: an error, never another file)
 *   svctest serve PORT TYPE        a filesystem server on PORT that holds every request until it is cancelled (DESIGN
 *                                  D23 step 1), saying "PORT ready" once its port is open: FS_ROOT 2 (fsinfo type TYPE: "nfs" makes it a network mount), FS_CLOSE
 *                                  0 (said), FS_SYNC 0 (its a0 said), FS_QUIT ends it, FS_OPEN of /o 5. Polls its port with POLLPRI; on it, the
 *                                  held requests marked UMSG_CANCEL are answered: a stat 0 (a server that ignores the
 *                                  mark), anything else -EINTR. A request that comes marked is answered -EINTR at once
 *   svctest catch FILE             catch SIGINT, then open FILE: a caught signal must not cancel the open (D16 4)
 *   svctest trunc FILE             open FILE with O_WRONLY|O_CREAT|O_TRUNC (the kernel's FS_STAT, then FS_OPEN)
 *   svctest unlink FILE            unlink FILE (the kernel's FS_LSTAT, then FS_UNLINK) */
#include "../kernel/fsproto.h"
#define E_MS 709u

static int ncaught;
static void caught(int sig) { (void)sig; ncaught++; }

/* svctest serve (D23 step 1): every request but FS_ROOT, FS_CLOSE, FS_SYNC and FS_QUIT is held until it is cancelled */
static int serve(const char *name, const char *type)
{
    struct umsg *held[8];
    int nh = 0, port = sys_port_create(name);
    if (port < 0) { dprintf(1, "svctest serve: port %d\n", port); return 1; }
    dprintf(1, "svctest serve: %s ready\n", name);    /* the test waits for this, not a fixed time */
    for (;;) {
        struct pollfd pf = { port, POLLPORT | POLLIN | POLLPRI, 0 };
        int n = sys_evwait(&pf, 1, -1);
        if (n < 0) return 1;
        if (pf.revents & POLLPRI) {
            int k = 0;
            for (int i = 0; i < nh; ) {
                struct umsg *m = held[i];
                if (!(m->kflags & UMSG_CANCEL)) { i++; continue; }
                int st = m->type == FS_STAT || m->type == FS_LSTAT || m->type == FS_FSTAT, r = -EINTR;
                if (st) {
                    u32 *u = (u32 *)(m->type == FS_FSTAT ? m->a[1] : m->a[2]);   /* ino mode size mtime uid gid links */
                    memset(u, 0, 28);
                    u[0] = 3; u[1] = 0100644; u[6] = 1;
                    r = 0;
                }
                dprintf(1, "svctest serve: type %u answered %d after the cancel\n", m->type, r);
                sys_reply(m, r);
                held[i] = held[--nh];
                k++;
            }
            dprintf(1, "svctest serve: POLLPRI, %d held cancelled\n", k);
        }
        if (!(pf.revents & POLLIN)) continue;
        struct umsg *m = sys_recv(port, 0);
        if (!m || (int)m == -1) continue;
        int r;
        if (m->type == FS_ROOT) {
            struct fsinfo *fi = (struct fsinfo *)m->a[0];
            memset(fi, 0, sizeof *fi);
            strcpy(fi->dev, "svct");
            strncpy(fi->type, type, sizeof fi->type - 1);
            r = 2;
        } else if (m->type == FS_CLOSE) {
            dprintf(1, "svctest serve: close %u%s\n", m->a[0], m->kflags & UMSG_CANCEL ? " came marked" : "");
            r = 0;
        } else if (m->type == FS_OPEN && !(m->kflags & UMSG_CANCEL) && !strcmp((const char *)m->a[0], "/o")) {
            u32 *u = (u32 *)m->a[3];            /* /o opens (a file of 10 bytes): its reads are held */
            memset(u, 0, 28);
            u[0] = 5; u[1] = 0100644; u[2] = 10; u[6] = 1;
            r = 5;
        }
        else if (m->type == FS_SYNC) { dprintf(1, "svctest serve: %s sync a0=%u\n", name, m->a[0]); r = 0; }
        else if (m->type == FS_QUIT) { sys_reply(m, 0); return 0; }
        else if (m->kflags & UMSG_CANCEL) {
            dprintf(1, "svctest serve: type %u came cancelled\n", m->type);
            r = -EINTR;
        } else if (nh < 8) {
            dprintf(1, "svctest serve: type %u held\n", m->type);
            held[nh++] = m;
            continue;
        } else r = -EBUSY;
        sys_reply(m, r);
    }
}

static const char USAGE[] = "usage: svctest port|ports|reg|rm|call|kcall|raw|hold|keep|lost|mount|serve|catch|trunc|unlink ...\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    if (argc >= 3 && !strcmp(argv[1], "port")) {
        dprintf(1, "svctest port %s: %d\n", argv[2], sys_port_create(argv[2]));
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "ports")) {
        char n[8];
        int k = 0, r;
        for (;;) {
            n[0] = 'u'; n[1] = (char)('0' + k / 10); n[2] = (char)('0' + k % 10); n[3] = 0;
            if ((r = sys_port_create(n)) < 0) break;
            k++;
        }
        dprintf(1, "svctest ports: %d made, then %d\n", k, r);
        sys_sleep((u32)atoi(argv[2]) * 100);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "reg")) {
        char text[SVC_TEXT_MAX];
        text[0] = 0;
        for (int i = 4; i < argc; i++) {
            if (strlen(text) + strlen(argv[i]) + 2 > sizeof text) { dprintf(1, "svctest reg: too long\n"); return 1; }
            if (i > 4) strcat(text, " ");
            strcat(text, argv[i]);
        }
        dprintf(1, "svctest reg %s: %d\n", argv[2], sys_svc_register(argv[2], text, atoi(argv[3])));
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "rm")) {
        dprintf(1, "svctest rm %s: %d\n", argv[2], sys_svc_remove(argv[2]));
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "kcall")) {
        sys_kill(atoi(argv[2]));
        sys_sleep(100);
        argv++;
        argc--;
        argv[1] = "call";                       /* the rest as svctest call NAME TYPE */
    }
    if (argc >= 4 && !strcmp(argv[1], "call")) {
        int port = sys_port_find(argv[2]);
        struct umsg m;
        memset(&m, 0, sizeof m);
        m.type = (u32)atoi(argv[3]);
        m.a[0] = argc > 4 ? (u32)atoi(argv[4]) : 0;
        u32 t0 = sys_hrtime();
        int r = port < 0 ? port : sys_call(port, &m);
        dprintf(1, "svctest call %s %s: %d in %u ms\n", argv[2], argv[3], r, (sys_hrtime() - t0) / E_MS);
        if (argc > 5)
            sys_sleep((u32)atoi(argv[5]) * 100);
        return 0;
    }
    if (argc >= 3 && (!strcmp(argv[1], "raw") || !strcmp(argv[1], "hold"))) {
        static char sec[512];
        int dp = sys_port_find("disk"), raw = argv[1][0] == 'r';
        struct umsg m;
        m.type = DISK_OPEN; m.a[0] = (u32)argv[2]; m.a[1] = raw ? DISK_RAW : 0;
        int h = sys_call(dp, &m);
        if (raw) { dprintf(1, "svctest raw %s: %d\n", argv[2], h); return 0; }
        m.type = DISK_READ; m.a[0] = (u32)h; m.a[1] = 0; m.a[2] = 1; m.a[3] = (u32)sec;
        int r1 = sys_call(dp, &m);
        sys_sleep(argc > 3 ? (u32)atoi(argv[3]) * 100 : 500);
        m.type = DISK_READ; m.a[0] = (u32)h; m.a[1] = 0; m.a[2] = 1; m.a[3] = (u32)sec;
        int r2 = sys_call(dp, &m);
        dprintf(1, "svctest hold %s: handle %d, read %d, after the wait %d\n", argv[2], h, r1, r2);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "keep")) {
        static char a[4096], b[4096];
        int fd = sys_open(argv[2], O_RDONLY);
        int n1 = fd < 0 ? fd : sys_read(fd, a, sizeof a), u = sys_unlink(argv[2]);
        sys_sleep((u32)atoi(argv[3]) * 100);
        sys_lseek(fd, 0, 0);
        int n2 = sys_read(fd, b, sizeof b);
        dprintf(1, "svctest keep %s: read %d, deleted %d, again %d: %s\n", argv[2], n1, u, n2,
               n1 > 0 && n1 == n2 && !memcmp(a, b, (u32)n1) ? "the same bytes" : "DIFFERENT");
        sys_close(fd);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "kill")) {
        sys_sleep((u32)atoi(argv[3]) * 100);
        int a = sys_kill(atoi(argv[2])), b = sys_kill(atoi(argv[2]));
        dprintf(1, "svctest kill %s: %d %d\n", argv[2], a, b);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "reboot")) {
        sys_kill(atoi(argv[2]));
        sys_sleep(50);
        dprintf(1, "svctest reboot: %d\n", sys_reboot(0));
        return 1;
    }
    if (argc >= 4 && !strcmp(argv[1], "lost")) {
        char buf[8];
        int fd = sys_open(argv[2], O_RDWR | O_CREAT | O_TRUNC), w = fd < 0 ? fd : sys_write(fd, "AAAA", 4);
        if (argc > 4) sys_kill(atoi(argv[4]));
        sys_sleep((u32)atoi(argv[3]) * 100);
        sys_lseek(fd, 0, 0);
        memset(buf, 0, sizeof buf);
        int r = sys_read(fd, buf, 4);
        dprintf(1, "svctest lost %s: wrote %d, read %d \"%s\"\n", argv[2], w, r, r > 0 ? buf : "");
        sys_close(fd);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "mount")) {
        struct umountreq rq = { argv[3], argv[2], "test", "test" };
        dprintf(1, "svctest mount %s: %d\n", argv[3], sys_mount(&rq));
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "serve"))
        return serve(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "catch")) {
        signal(SIGINT, caught);
        int fd = sys_open(argv[2], O_RDONLY);
        dprintf(1, "svctest catch: open %d, SIGINT caught %d\n", fd, ncaught);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "trunc")) {
        dprintf(1, "svctest trunc: %d\n", sys_open(argv[2], O_WRONLY | O_CREAT | O_TRUNC));
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "unlink")) {
        dprintf(1, "svctest unlink: %d\n", sys_unlink(argv[2]));
        return 0;
    }
    zusage(USAGE);
}
