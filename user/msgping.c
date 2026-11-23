/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* test client: a few calls, a zero-copy string call, then N timed round trips. The memory protection tests (DESIGN
 * D60 stage 2, tools/mmu-test.sh): winread FILE [CASE], reads of a file of msgpingd -fswin's pattern into a guarded
 * region (the bytes read must be the file's, every byte around them as before); readpast FILE, a read into the end
 * of its own memory (-EFAULT); rewrite, msgpingd's type 10 (its message rewritten while it is served); call PORT, one
 * call there (its result). D60 stage 3: past, peer and wild PORT (below); the zero-copy call declares its buffer. */
static unsigned char pat(u32 i) { return (unsigned char)(i * 7 + i / 256); }
static int winread(const char *file, int only)
{
    static const u32 cs[][3] = {        /* the buffer's offset in the region, its length, the file's offset */
        { 0, 1, 0 }, { 1, 10, 5 }, { 4095, 2, 100 }, { 100, 4096, 0 }, { 4000, 5000, 7 }, { 4096, 4096, 4096 },
        { 10, 12000, 3 }, { 8191, 1, 19999 }, { 3, 16000, 1000 },
        { 4146, 100, 50 }, { 4146, 4046, 50 }, { 8192, 100, 50 },     /* 10-12: mid-page, to a page's end, from its start */
    };
    u32 n = 16384 + 4096;
    unsigned char *r = sys_memalloc(n);
    if (!r) { dprintf(1, "msgping: no memory\n"); return 1; }
    int fd = sys_open(file, 0), ok = 0, bad = 0;
    if (fd < 0) { dprintf(1, "msgping: winread %s: open %d\n", file, fd); return 1; }
    for (int c = 0; c < (int)(sizeof cs / sizeof cs[0]); c++) {
        if (only ? c + 1 != only : c >= 9)
            continue;
        u32 bo = cs[c][0], len = cs[c][1], fo = cs[c][2];
        memset(r, 0xA5, n);
        sys_lseek(fd, (int)fo, 0);
        int got = sys_read(fd, r + bo, len);
        u32 w = 0;
        for (u32 i = 0; i < n; i++)             /* (a failed read: every byte as before) */
            if (r[i] != (i >= bo && i < bo + len && got > 0 ? pat(fo + i - bo) : 0xA5)) { w = i + 1; break; }
        if (only) {
            dprintf(1, "msgping: winread %s case %d: read %d, %s\n", file, c + 1, got,
                    w ? "WRONG" : got > 0 ? "the bytes right" : "untouched");
            sys_close(fd);
            return w != 0;
        }
        if (got != (int)len || w) {
            dprintf(1, "msgping: winread %s BAD case %d: read %d of %u, first wrong byte %d\n", file, c + 1, got, len,
                    (int)w - 1);
            bad++;
        } else
            ok++;
    }
    sys_close(fd);
    dprintf(1, "msgping: winread %s %s %d\n", file, bad ? "FAILED" : "ok", ok);
    return bad != 0;
}
/* D60 7.4 (tools/mmu-test.sh): shm, a region offered to a child (msgping shmchild ADDR), which takes it, checks the
 * parent's bytes and writes its own; the parent sees them, frees the region early (the child keeps it and owns it
 * then), the child checks it can still read it and that it owns it, and drops it. "msgping: shm ok" when every step
 * held (with full protection only: without it the calls change nothing and the region is simply freed) */
static int shm_child(u32 a)
{
    volatile unsigned char *r = (volatile unsigned char *)a;
    struct ushm u;
    int t = -1;
    for (int i = 0; i < 200 && (t = sys_shm(SHM_TAKE, (void *)a, 0)) < 0; i++) sys_sleep(1);
    if (t < 0) { dprintf(1, "msgping: shmchild: take %d\n", t); return 1; }
    for (u32 i = 0; i < 8192; i++) if (r[i] != (unsigned char)(i * 3)) { dprintf(1, "msgping: shmchild: byte %u\n", i); return 2; }
    r[5000] = 0x5A;                                     /* the parent's turn: it frees the region */
    for (int i = 0; i < 300 && r[5001] != 0x77; i++) sys_sleep(1);
    if (r[5001] != 0x77) { dprintf(1, "msgping: shmchild: the parent's mark\n"); return 3; }
    for (int i = 0; i < 300; i++) {                     /* freed by the parent: still here, now ours */
        if (sys_shm(SHM_INFO, (void *)a, (u32)&u) == 0 && (int)u.owner == sys_getpid()) break;
        sys_sleep(1);
    }
    int me = sys_shm(SHM_INFO, (void *)a, (u32)&u) == 0 && (int)u.owner == sys_getpid();
    int still = r[0] == 0 && r[3] == 9;
    int d = sys_shm(SHM_DROP, (void *)a, 0);
    dprintf(1, "msgping: shmchild: owner now %s, bytes %s, drop %d\n", me ? "me" : "not me", still ? "kept" : "LOST", d);
    if (!(me && still && d == 0))
        return 4;
    return r[0] + 5;                                    /* dropped and freed: this read must end us (139) */
}
static int shm_rochild(u32 a)                           /* msgping shmro ADDR: a read-only share, then a write */
{
    volatile unsigned char *r = (volatile unsigned char *)a;
    int t = -1;
    for (int i = 0; i < 200 && (t = sys_shm(SHM_TAKE, (void *)a, 0)) < 0; i++) sys_sleep(1);
    if (t < 0 || r[1] != 3) return 1;
    r[1] = 0;                                           /* read only: this write must end us (139) */
    return 2;
}
static int shm_test(void)
{
    volatile unsigned char *r = sys_memalloc(8192);
    char hex[16], *av[4] = { "msgping", "shmchild", hex, 0 };
    if (!r) { dprintf(1, "msgping: shm: no memory\n"); return 1; }
    for (u32 i = 0; i < 8192; i++) r[i] = (unsigned char)(i * 3);
    snprintf(hex, sizeof hex, "%x", (u32)r);
    int pid = sys_spawn("/bin/msgping", av, 0);
    if (pid < 0) { dprintf(1, "msgping: shm: spawn %d\n", pid); return 1; }
    int o = sys_shm(SHM_OFFER, (void *)r, (u32)pid);
    int o2 = sys_shm(SHM_OFFER, (void *)r, (u32)pid);  /* a second offer of the same: -EEXIST (with protection) */
    for (int i = 0; i < 300 && r[5000] != 0x5A; i++) sys_sleep(1);
    int seen = r[5000] == 0x5A;
    r[0] = 0; r[3] = 9;
    r[5001] = 0x77;
    int f = sys_memfree((void *)r);                     /* early: the child holds it */
    int st = sys_wait(pid);
    /* what may not be: an offer of what is not a region of ours, to ourselves; a take of what was not offered to us;
     * a read-only share written */
    char stk[16];
    int bad1 = sys_shm(SHM_OFFER, stk, (u32)pid), bad2 = -1, bad3 = -1, st2 = -1;
    volatile unsigned char *q = sys_memalloc(4096);
    if (q) {
        q[1] = 3;
        bad2 = sys_shm(SHM_OFFER, (void *)q, (u32)sys_getpid());
        bad3 = sys_shm(SHM_TAKE, (void *)q, 0);
        snprintf(hex, sizeof hex, "%x", (u32)q);
        av[1] = "shmro";
        int p2 = sys_spawn("/bin/msgping", av, 0);
        if (p2 >= 0 && sys_shm(SHM_OFFER, (void *)q, (u32)p2 | SHMF_RO) == 0)
            st2 = sys_wait(p2);
    }
    int ok = o == 0 && o2 == -17 && seen && f == 0 && st == 139 && bad1 == -22 && bad2 == -3 && bad3 == -2 &&
             st2 == 139 && q && q[1] == 3;
    dprintf(1, "msgping: shm %s (offer %d %d, child's byte %s, free %d, child %d after its drop; refused %d %d %d; "
            "read-only child %d)\n", ok ? "ok" : "FAILED", o, o2, seen ? "seen" : "not seen", f, st, bad1, bad2, bad3, st2);
    return !ok;
}
static const char USAGE[] = "usage: msgping [rounds]\n       msgping winread FILE [CASE] | readpast FILE | rewrite | call PORT\n       msgping past PORT | peer PORT | wild PORT | shm\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    if (argc > 1 && !strcmp(argv[1], "shm"))
        return shm_test();
    if (argc > 2 && !strcmp(argv[1], "shmchild"))
        return shm_child((u32)strtoul(argv[2], 0, 16));
    if (argc > 2 && !strcmp(argv[1], "shmro"))
        return shm_rochild((u32)strtoul(argv[2], 0, 16));
    if (argc > 2 && !strcmp(argv[1], "winread"))
        return winread(argv[2], argc > 3 ? atoi(argv[3]) : 0);
    if (argc > 2 && (!strcmp(argv[1], "past") || !strcmp(argv[1], "peer") || !strcmp(argv[1], "wild"))) {
        /* D60 stage 3: past PORT, a declared page (a region of its own) and msgpingd's type 11 one byte after it; peer
         * PORT, type 13 on its stack; wild PORT, type 14 on an undeclared buffer. The call's result, a[2], and whether
         * the buffer is as it was */
        int port = sys_port_find(argv[2]);
        struct umsg m;
        memset(&m, 0, sizeof m);
        char stk[16] = "untouched";
        unsigned char *pg = argv[1][1] == 'a' ? sys_memalloc(4096) : 0;
        struct ucalldecl dc = { 0, { { 0, 0, 0 } } };
        if (pg) {
            memset(pg, 0, 4096);
            dc.n = 1; dc.b[0].addr = (u32)pg; dc.b[0].len = 4096; dc.b[0].flags = CALLB_RD | CALLB_WR;
            m.type = 11; m.a[0] = (u32)pg; m.a[1] = 4096;
        } else {
            m.type = argv[1][1] == 'e' ? 13 : 14;
            m.a[0] = (u32)stk;
        }
        int r = port < 0 ? port : sys_callv(port, &m, &dc);
        dprintf(1, "msgping: %s %s: %d, a[2] %d, %s\n", argv[1], argv[2], r, (int)m.a[2],
                pg ? (pg[0] ? "changed" : "untouched") : stk[0] == 'u' ? "untouched" : "changed");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "readpast")) {
        unsigned char *r = sys_memalloc(4096);
        int fd = sys_open(argv[2], 0);
        int got = r && fd >= 0 ? sys_read(fd, r + 4096 - 10, 100) : -1;
        dprintf(1, "msgping: readpast %d\n", got);
        return got != -14;
    }
    if (argc > 2 && !strcmp(argv[1], "call")) {
        struct umsg m;
        memset(&m, 0, sizeof m);
        m.type = 1;
        int p = sys_port_find(argv[2]);
        dprintf(1, "msgping: call to %s: %d\n", argv[2], p < 0 ? p : sys_call(p, &m));
        return 0;
    }
    int port = sys_port_find("ping");
    if (port < 0) { dprintf(1, "msgping: no server (run msgpingd &)\n"); return 1; }
    struct umsg m;
    if (argc > 1 && !strcmp(argv[1], "rewrite")) {
        memset(&m, 0, sizeof m);
        m.type = 10; m.a[0] = (u32)&m;
        int r = sys_call(port, &m);
        dprintf(1, "msgping: message rewritten in flight: result %d, a[2] %u\n", r, m.a[2]);
        return 0;
    }
    m.type = 1; m.a[0] = 40; m.a[1] = 2;
    int r = sys_call(port, &m);
    dprintf(1, "msgping: 40+2 = %u (result %d)\n", m.a[2], r);
    char text[] = "zero copy: the server edits my buffer";
    m.type = 2; m.a[0] = (u32)text;
    struct ucalldecl dc = { 1, { { (u32)text, sizeof text, CALLB_RD | CALLB_WR } } };  /* D60 5.5: the buffer declared */
    sys_callv(port, &m, &dc);
    dprintf(1, "msgping: %s\n", text);
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    if (n < 1) n = 1;
    u32 t0 = sys_hrtime();                              /* the E-clock, as bench uses: the 100 Hz tick is too coarse */
    for (int i = 0; i < n; i++) { m.type = 1; m.a[0] = (u32)i; m.a[1] = 1; sys_call(port, &m); }
    u32 e = sys_hrtime() - t0;
    u32 tenths = (u32)(((unsigned long long)e * 10000000ull) / 709379u / (u32)n);   /* 0.1 us per trip (PAL E-clock) */
    u32 total = (u32)(((unsigned long long)e * 1000000ull) / 709379u);
    dprintf(1, "msgping: %d round trips in %u us, %u.%u us each\n", n, total, tenths / 10, tenths % 10);
    m.type = 9; sys_call(port, &m);
    return 0;
}
