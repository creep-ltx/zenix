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
    dprintf(1, "badptr: %s %s\n", what, ok ? "ok" : "FAILED");
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
    if (sys_geteuid() == 0) { dprintf(1, "badptr: ipc: run this as a normal user\n"); return 2; }
    check("ipc: port \"fs\" cannot be created", sys_port_create("fs") == -EBUSY);     /* registered services (D11) */
    check("ipc: port \"fs.ide0p9\" cannot be created", sys_port_create("fs.ide0p9") == -13);
    check("ipc: port \"disk\" cannot be created", sys_port_create("disk") == -EBUSY);
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
    dprintf(1, "badptr: ipc: %d system processes, %d refused kill, stop and foreground\n", servers, refused);
    check("ipc: system processes cannot be killed, stopped or made foreground", servers >= 4 && refused == servers);
    /* O_EXCL (here-documents, mktemp): one request checks and creates; an existing name - also a dangling symbolic link
     * another user put there - is never opened or followed */
    sys_unlink("/tmp/.badptr-x"); sys_unlink("/tmp/.badptr-l");
    int f1 = sys_open("/tmp/.badptr-x", O_WRONLY | O_CREAT | O_EXCL), f2 = sys_open("/tmp/.badptr-x", O_WRONLY | O_CREAT | O_EXCL);
    sys_symlink("/tmp/.badptr-t", "/tmp/.badptr-l");
    int f3 = sys_open("/tmp/.badptr-l", O_WRONLY | O_CREAT | O_EXCL);
    struct ustat st;
    int followed = sys_stat("/tmp/.badptr-t", &st) == 0;
    dprintf(1, "badptr: ipc: O_EXCL gives %d, %d, %d\n", f1 >= 0 ? 0 : f1, f2, f3);
    check("ipc: O_EXCL creates once, then -17, also for a dangling symbolic link", f1 >= 0 && f2 == -17 && f3 == -17 && !followed);
    if (f1 >= 0) sys_close(f1);
    sys_unlink("/tmp/.badptr-x"); sys_unlink("/tmp/.badptr-l"); sys_unlink("/tmp/.badptr-t");
    check("ipc: the clock cannot be set", sys_settime(1) == -13);
    check("ipc: the serial copy cannot be switched", sys_ttymirror(1) == -13);
    dprintf(1, "badptr: ipc %s\n", bad ? "FAILED" : "all ok");
    return bad;
}

/* D15 step 0 (frozen interfaces): a struct the kernel or a server writes never goes past the caller's buffer. Size 0
 * is what every program built before D15 passes: it gets the struct's first size (uproc: 32 bytes; D10 made it 40 and
 * D13 44, so an older ps had less room than the kernel wrote) */
static unsigned char gb[160];
static int fill(void) { memset(gb, 0xA5, sizeof gb); return 0; }
static int written(u32 n)               /* exactly the first n bytes were written: everything after is untouched */
{
    for (u32 i = n; i < sizeof gb; i++) if (gb[i] != 0xA5) return 0;
    return 1;
}
static void size_checks(void)
{
    fill();
    int r = sys3(21, 0, (u32)gb, 0);
    dprintf(1, "badptr: procinfo, size 0: %d, bytes 32-43 %s\n", r, written(32) ? "untouched" : "WRITTEN");
    check("procinfo from an old program (size 0, a 40-byte buffer and a guard): 32 bytes, nothing after", r == 0 && written(32));
    int ok = fill() || (sys3(21, 0, (u32)gb, 40) == 0 && written(40));
    ok = ok && !fill() && sys3(21, 0, (u32)gb, sizeof(struct uproc) + 64) == 0 && written(sizeof(struct uproc));
    ok = ok && !fill() && sys3(21, 0, (u32)gb, 8) == -EINVAL && written(0);
    check("procinfo sized: 40 gets 40 bytes, more gets the whole struct, less than 32 -EINVAL", ok);
    int fd = sys_open("/", O_RDONLY);
    ok = fd >= 0;
    for (u32 big = 0; big <= 100; big += 100) {         /* size 0 (an old program), then one bigger than every struct */
        ok = ok && !fill() && sys3(13, (u32)"/", (u32)gb, big) == 0 && written(sizeof(struct ustat));
        ok = ok && !fill() && sys3(57, (u32)"/", (u32)gb, big) == 0 && written(sizeof(struct ustat));
        ok = ok && !fill() && sys3(14, (u32)fd, (u32)gb, big) == 0 && written(sizeof(struct ustat));
        ok = ok && !fill() && sys3(12, (u32)fd, (u32)gb, big) > 0 && written(sizeof(struct udirent));
        ok = ok && !fill() && sys3(43, (u32)gb, 0, big) == 0 && written(sizeof(struct ustatfs));
        ok = ok && !fill() && sys3(71, 0, (u32)gb, big) == 0 && written(sizeof(struct umountinfo));
        ok = ok && !fill() && sys3(22, (u32)gb, big, 0) == 0 && written(16);
    }
    ok = ok && !fill() && sys3(13, (u32)"/", (u32)gb, 4) == -EINVAL && sys3(22, (u32)gb, 4, 0) == -EINVAL && written(0);
    if (fd >= 0) sys_close(fd);
    check("stat, lstat, fstat, readdir, statfs, mountinfo, meminfo: size 0 or bigger writes the struct only; smaller -EINVAL", ok);
    int disk = sys_port_find("disk");
    ok = disk >= 0 && !fill() && call(disk, DISK_LIST, 0, (u32)gb, 0x7FFF, 0) == 0 && written(DISKINFO_FIRST);
    ok = ok && !fill() && call(disk, DISK_LISTSZ, 0, (u32)gb, sizeof gb, 0) == 0 && written(sizeof(struct diskinfo));
    ok = ok && !fill() && call(disk, DISK_LISTSZ, 0, (u32)gb, 40, 0) == -EINVAL && written(0);
    check("disk info: DISK_LIST writes the first 80 bytes, DISK_LISTSZ at most the size given, below 80 -EINVAL", ok);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "ipc")) return ipc_checks();
    int a = sys_write(1, (const void *)0x1000, 16);
    int b = sys_write(1, (const void *)0xFFFFFFF0, 64);
    dprintf(1, "badptr: write(0x1000)=%d write(0xfffffff0)=%d\n", a, b);
    check("bad pointers refused", a == -EFAULT && b == -EFAULT);
    size_checks();

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

    /* many small blocks: they fill the 4 KB first chunk and the growing ones after it (16, 64 KB), stay apart and
     * keep their contents; freed, they are used again */
    static char *sm[600]; int nsm = 0, apart = 1;
    while (nsm < 600 && (sm[nsm] = malloc(100))) { memset(sm[nsm], nsm & 0xFF, 100); nsm++; }
    for (int i = 0; i < nsm; i++) for (int j = 0; j < 100; j++) if (sm[i][j] != (char)(i & 0xFF)) apart = 0;
    check("600 small blocks across several chunks keep their contents", nsm == 600 && apart);
    for (int i = 0; i < nsm; i++) free(sm[i]);
    char *again1 = malloc(100);
    int reused = 0; for (int i = 0; i < nsm; i++) if (sm[i] == again1) reused = 1;
    check("... and are used again after free", reused);
    free(again1);

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
    dprintf(1, "badptr: %d big blocks before NULL\n", nb);
    check("malloc says NULL when the regions run out", nb > 0 && nb < 16);
    for (int i = 0; i < nb; i++) free(big[i]);
    void *again = malloc(65536);
    check("free gives big blocks back", again != 0);
    free(again);

    /* a pointer malloc did not give out is reported, not followed */
    static u32 fake[4] = { 16, 0x12345678, 0, 0 };
    free(&fake[2]);
    check("a bad free is survived", 1);

    dprintf(1, "badptr: %s\n", bad ? "FAILED" : "all ok");
    return bad;
}
