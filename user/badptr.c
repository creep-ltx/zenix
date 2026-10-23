/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/fsproto.h"
#include "../kernel/diskproto.h"
/* Test program: hands the kernel pointers that are not ours (must get -1, not a crash), then checks that memory
 * calls fail with 0 (NULL) and that malloc, free and realloc keep their promises (audit B1-B3). One line per
 * check; exits 0 when every check passed.
 * badptr ipc (run as a normal user): what a user must not be able to do through raw messages and process calls
 * (audit D): fake the system's ports, send the filesystem or disk server requests of its own, kill or stop the
 * built-in servers or init, set the clock, switch the serial copy. */
static int bad;
static void check(const char *what, int ok)
{
    printf("badptr: %s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) bad = 1;
}

static int call(int port, u32 type, u32 a0, u32 a1, u32 a2, u32 a3)
{
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = type; m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3;
    m.kflags = UMSG_KERNEL;                         /* a forged claim: the kernel clears it */
    return sys_call(port, &m);
}

static int ipc_checks(void)
{
    static char buf[512];
    if (sys_geteuid() == 0) { printf("badptr: ipc: run this as a normal user\n"); return 2; }
    check("ipc: port \"fs\" cannot be created", sys_port_create("fs") == -13);
    check("ipc: port \"fs.ide0p9\" cannot be created", sys_port_create("fs.ide0p9") == -13);
    check("ipc: port \"disk\" cannot be created", sys_port_create("disk") == -13);
    int fs = sys_port_find("fs"), disk = sys_port_find("disk");
    check("ipc: FS_READ by inode refused", fs >= 0 && call(fs, FS_READ, 2, 0, (u32)buf, 16) == -13);
    check("ipc: FS_WRITE by inode refused", fs >= 0 && call(fs, FS_WRITE, 12, 0, (u32)buf, 4) == -13);
    check("ipc: FS_OPEN refused", fs >= 0 && call(fs, FS_OPEN, (u32)"/etc/passwd", 2, O_RDONLY, (u32)buf) == -13);
    check("ipc: FS_QUIT refused", fs >= 0 && call(fs, FS_QUIT, 0, 0, 0, 0) == -13);
    check("ipc: DISK_OPEN refused", disk >= 0 && call(disk, DISK_OPEN, (u32)"ide0", 0, 0, 0) == -13);
    check("ipc: DISK_READ refused", disk >= 0 && call(disk, DISK_READ, 0, 0, 1, (u32)buf) == -13);
    check("ipc: DISK_RESCAN refused", disk >= 0 && call(disk, DISK_RESCAN, 0, 0, 0, 0) == -13);
    check("ipc: DISK_LIST still answers", disk >= 0 && call(disk, DISK_LIST, 0, (u32)buf, 0, 0) == 0);
    int servers = 0, refused = 0;
    for (u32 i = 0; i < 256; i++) {      /* until procinfo says the table ends */
        struct uproc p;
        if (sys_procinfo(i, &p) < 0 || p.state == 0 || p.state == 5 || p.pid == 0) continue;
        if (strcmp(p.name, "diskd") && strcmp(p.name, "fsd") && strcmp(p.name, "zfsd") && strcmp(p.name, "boot") && strcmp(p.name, "init"))
            continue;
        servers++;
        if (sys_kill((int)p.pid) == -13 && sys_jobctl((int)p.pid, 0) == -13 && sys_setfg((int)p.pid) == -13) refused++;
    }
    printf("badptr: ipc: %d system processes, %d refused kill, stop and foreground\n", servers, refused);
    check("ipc: system processes cannot be killed, stopped or made foreground", servers >= 4 && refused == servers);
    /* O_EXCL (here-documents, mktemp): one request checks and creates; an existing name - also a dangling symbolic link
     * another user put there - is never opened or followed */
    sys_unlink("/tmp/.badptr-x"); sys_unlink("/tmp/.badptr-l");
    int f1 = sys_open("/tmp/.badptr-x", O_WRONLY | O_CREAT | O_EXCL), f2 = sys_open("/tmp/.badptr-x", O_WRONLY | O_CREAT | O_EXCL);
    sys_symlink("/tmp/.badptr-t", "/tmp/.badptr-l");
    int f3 = sys_open("/tmp/.badptr-l", O_WRONLY | O_CREAT | O_EXCL);
    struct ustat st;
    int followed = sys_stat("/tmp/.badptr-t", &st) == 0;
    printf("badptr: ipc: O_EXCL gives %d, %d, %d\n", f1 >= 0 ? 0 : f1, f2, f3);
    check("ipc: O_EXCL creates once, then -17, also for a dangling symbolic link", f1 >= 0 && f2 == -17 && f3 == -17 && !followed);
    if (f1 >= 0) sys_close(f1);
    sys_unlink("/tmp/.badptr-x"); sys_unlink("/tmp/.badptr-l"); sys_unlink("/tmp/.badptr-t");
    check("ipc: the clock cannot be set", sys_settime(1) == -13);
    check("ipc: the serial copy cannot be switched", sys_ttymirror(1) == -13);
    printf("badptr: ipc %s\n", bad ? "FAILED" : "all ok");
    return bad;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "ipc")) return ipc_checks();
    int a = sys_write(1, (const void *)0x1000, 16);
    int b = sys_write(1, (const void *)0xFFFFFFF0, 64);
    printf("badptr: write(0x1000)=%d write(0xfffffff0)=%d\n", a, b);
    check("bad pointers refused", a == -EFAULT && b == -EFAULT);

    /* the kernel: 0 on failure, never -1; sizes near 4 GB do not wrap */
    check("memalloc(0xfffffff0) is 0", sys_memalloc(0xFFFFFFF0u) == 0);
    check("memalloc(0xffffffff) is 0", sys_memalloc(0xFFFFFFFFu) == 0);
    check("memalloc(0) is 0", sys_memalloc(0) == 0);
    check("malloc(-16) is NULL", malloc((u32)-16) == 0);
    check("malloc(0x7fffffff) is NULL", malloc(0x7FFFFFFFu) == 0);

    /* small blocks are reused after free */
    char *p = malloc(100);
    free(p);
    char *q = malloc(100);
    check("a freed small block is reused", p && p == q);

    /* the largest small block (32 KB: it needs a chunk bigger than the first one) */
    char *l = malloc(32768);
    if (l) { memset(l, 1, 32768); free(l); }
    check("malloc(32768) works", l != 0);

    /* realloc keeps the contents, copies only the old block */
    char *r = malloc(20);
    for (int i = 0; i < 20; i++) r[i] = (char)i;
    char *s = realloc(r, 50000);
    int same = s != 0;
    for (int i = 0; s && i < 20; i++) if (s[i] != (char)i) same = 0;
    check("realloc keeps the contents", same);
    free(s);

    /* big blocks each take a region (at most 8 per process): malloc must say NULL when they run out, and free
     * must give them back so that the next one works */
    void *big[16]; int nb = 0;
    while (nb < 16 && (big[nb] = malloc(65536))) nb++;
    printf("badptr: %d big blocks before NULL\n", nb);
    check("malloc says NULL when the regions run out", nb > 0 && nb < 16);
    for (int i = 0; i < nb; i++) free(big[i]);
    void *again = malloc(65536);
    check("free gives big blocks back", again != 0);
    free(again);

    /* a pointer malloc did not give out is reported, not followed */
    static u32 fake[4] = { 16, 0x12345678, 0, 0 };
    free(&fake[2]);
    check("a bad free is survived", 1);

    printf("badptr: %s\n", bad ? "FAILED" : "all ok");
    return bad;
}
