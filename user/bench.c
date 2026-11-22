/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"

/* Zenix benchmark: times things with the CIA's E-clock (709379 Hz on PAL),
 * not the 100 Hz tick. Numbers from an emulator are NOT hardware numbers:
 * run it on the real machine and write the result down there. */

#define E_HZ 709379u
static u32 us(u32 e) { return (u32)(((unsigned long long)e * 1000000ull) / E_HZ); }

static void report(const char *what, u32 e, u32 n)
{
    u32 total = us(e);
    dprintf(1, "%-34s %8u us total, %6u us each (n=%u)\n", what, total, n ? total / n : 0, n);
}

/* how fast does this machine run a tight loop? (subq + bne in the I-cache is
 * about 6 clocks per pass on a 68020: that gives a rough effective MHz) */
static void bench_cpu(void)
{
    u32 n = 200000;
    u32 t0 = sys_hrtime();
    register u32 c = n;
    __asm__ volatile("0: subq.l #1,%0\n\tbne 0b" : "+d"(c) :: "cc");
    u32 e = sys_hrtime() - t0;
    u32 t = us(e);
    dprintf(1, "cpu loop: %u passes in %u us = %u passes/ms (about %u MHz if 6 clocks/pass)\n",
           n, t, t ? n * 1000 / t : 0, t ? (n * 6) / t : 0);
}

static void bench_syscall(void)
{
    u32 t0 = sys_hrtime();
    for (int i = 0; i < 5000; i++) sys_getpid();
    report("null system call (getpid)", sys_hrtime() - t0, 5000);
}

/* every call's result is checked: a failed open is fast, and a timing made of failures is not a timing */
static int nfail;
static char first_fail[80];
static void failed(const char *what, const char *name, int r)
{
    if (!nfail++) {
        const char *t = zstrerror(r);
        snprintf(first_fail, sizeof first_fail, "%s %s: %s", what, name, t ? t : "failed");
    }
}

static void report_checked(const char *what, u32 e, u32 n)
{
    if (nfail) dprintf(1, "%-34s FAILED: %d of %u calls (first: %s); no timing\n", what, nfail, n, first_fail);
    else report(what, e, n);
    nfail = 0;
}

static void bench_fsstat(void)
{
    struct ustat st;
    int r = sys_stat("/bin/sh", &st);            /* warm */
    if (r < 0) failed("stat", "/bin/sh", r);
    u32 t0 = sys_hrtime();
    for (int i = 0; i < 500 && !nfail; i++) if ((r = sys_stat("/bin/sh", &st)) < 0) failed("stat", "/bin/sh", r);
    report_checked("stat /bin/sh (call to fs server)", sys_hrtime() - t0, 500);
}

static void bench_ipc(void)
{
    static char *const av[] = { "msgpingd", "-port", "bping", 0 };     /* its own port: "ping" may be a service */
    int pid = sys_spawn("/bin/msgpingd", av, 0);
    if (pid < 0) { dprintf(1, "(no msgpingd, skipping ipc)\n"); return; }
    int port, tries = 0;
    while ((port = sys_port_find("bping")) < 0 && ++tries < 200) sys_sleep(1);
    if (port < 0) { sys_kill(pid); sys_wait(pid); return; }
    struct umsg m;
    m.type = 1; m.a[0] = 1; m.a[1] = 2;
    sys_call(port, &m);
    u32 t0 = sys_hrtime();
    for (int i = 0; i < 2000; i++) { m.type = 1; sys_call(port, &m); }
    report("message round trip (2 switches)", sys_hrtime() - t0, 2000);
    m.type = 9; sys_call(port, &m);
    sys_wait(pid);
}

static void bench_read(void)
{
    static char buf[4096];
    int fd = sys_open("/bin/sh", O_RDONLY);
    if (fd < 0) { failed("open", "/bin/sh", fd); report_checked("read /bin/sh, 4K chunks, cached", 0, 1); return; }
    u32 t0 = sys_hrtime();
    u32 bytes = 0;
    for (int r = 0; r < 100; r++) {
        sys_lseek(fd, 0, 0);
        int n;
        while ((n = sys_read(fd, buf, sizeof buf)) > 0) bytes += (u32)n;
        if (n < 0) failed("read", "/bin/sh", n);
    }
    u32 e = sys_hrtime() - t0;
    sys_close(fd);
    int bad = nfail;
    report_checked("read /bin/sh, 4K chunks, cached", e, 100);
    u32 t = us(e);
    if (t && !bad) dprintf(1, "   = %u KB/s\n", (u32)(((unsigned long long)bytes * 1000000ull) / t / 1024));
}

static void bench_cold(void)
{
    static char buf[4096];
    static const char *files[] = { "/bin/ls", "/bin/grep", "/bin/wc", "/bin/ps", "/bin/cp", "/bin/mv" };
    u32 t0 = sys_hrtime(), bytes = 0;
    for (int i = 0; i < 6; i++) {
        int fd = sys_open(files[i], O_RDONLY), n;
        if (fd < 0) { failed("open", files[i], fd); continue; }
        while ((n = sys_read(fd, buf, sizeof buf)) > 0) bytes += (u32)n;
        if (n < 0) failed("read", files[i], n);
        sys_close(fd);
    }
    u32 e = sys_hrtime() - t0;
    int bad = nfail;
    report_checked("read 6 files, first time (disk)", e, 6);
    u32 t = us(e);
    if (t && !bad) dprintf(1, "   = %u KB/s from the disk (%u bytes)\n", (u32)(((unsigned long long)bytes * 1000000ull) / t / 1024), bytes);
}

static void bench_create(void)
{
    char name[24];
    int r = sys_mkdir("/tmp/bench");
    if (r < 0 && r != -EEXIST) {
        const char *t = zstrerror(r);
        dprintf(1, "create/delete: cannot make /tmp/bench (%s): skipped\n", t ? t : "failed");
        return;
    }
    u32 t0 = sys_hrtime();
    for (int i = 0; i < 50; i++) {
        strcpy(name, "/tmp/bench/f");
        name[12] = (char)('0' + i / 10); name[13] = (char)('0' + i % 10); name[14] = 0;
        int fd = sys_open(name, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) { failed("open", name, fd); continue; }
        if ((r = sys_write(fd, "hello", 5)) != 5) failed("write", name, r < 0 ? r : -EIO);
        if ((r = sys_close(fd)) < 0) failed("close", name, r);
    }
    report_checked("create + write 50 small files", sys_hrtime() - t0, 50);
    t0 = sys_hrtime();
    for (int i = 0; i < 50; i++) {
        strcpy(name, "/tmp/bench/f");
        name[12] = (char)('0' + i / 10); name[13] = (char)('0' + i % 10); name[14] = 0;
        if ((r = sys_unlink(name)) < 0) failed("unlink", name, r);
    }
    report_checked("delete 50 files", sys_hrtime() - t0, 50);
    if ((r = sys_rmdir("/tmp/bench")) < 0) {
        const char *t = zstrerror(r);
        dprintf(1, "create/delete: cannot remove /tmp/bench (%s)\n", t ? t : "failed");
    }
}

static void bench_dir(void)
{
    struct udirent d;
    int fd = sys_open("/bin", O_RDONLY);
    if (fd < 0) { failed("open", "/bin", fd); report_checked("list /bin (entries read)", 0, 1); return; }
    u32 t0 = sys_hrtime();
    int total = 0;
    for (int r = 0; r < 50; r++) {
        sys_lseek(fd, 0, 0);
        while (sys_readdir(fd, &d) > 0) total++;
    }
    report_checked("list /bin (entries read)", sys_hrtime() - t0, (u32)total);
    sys_close(fd);
}

/* 300 lines to the terminal (/dev/tty, so "bench > file" still measures the screen): once with the serial copy of
 * the console off, once with it on. The copy waits for the port only after the port has received something since
 * boot (a terminal or the test harness typed); with nobody listening it drops what does not fit (kernel/serial.c). */
static u32 print_lines(int fd, const char *line)
{
    u32 t0 = sys_hrtime();
    for (int i = 0; i < 300; i++) sys_write(fd, line, 80);
    return sys_hrtime() - t0;
}

static void bench_screen(void)
{
    static char line[80];
    memset(line, 'x', 79); line[79] = '\n';
    int fd = sys_open("/dev/tty", O_WRONLY);
    if (fd < 0) fd = 1;
    int off = sys_ttymirror(0) == 0;              /* root only */
    u32 e_off = off ? print_lines(fd, line) : 0;
    sys_ttymirror(1);
    u32 e_on = print_lines(fd, line);
    if (fd != 1) sys_close(fd);
    dprintf(1, "\n");
    if (off) report("print 300 lines of 79 chars (no copy)", e_off, 300);
    else dprintf(1, "print 300 lines, serial copy off: not measured (only root may turn the copy off)\n");
    report("... with the serial copy on", e_on, 300);
}

/* malloc and free (docs/d27-c-library.md "The allocator"): small blocks of a few sizes in turn, as list nodes and
 * words are (the quick lists), and blocks of 300 to 3,800 bytes given and freed in reverse (the chunks' bins) */
static void bench_malloc(void)
{
    static void *q[64];
    static const u32 sz[8] = { 12, 16, 24, 32, 40, 64, 100, 200 };
    u32 r = 5, n = 5000, bad = 0;
    for (int i = 0; i < 64; i++) if (!(q[i] = malloc(sz[i & 7]))) bad++;
    u32 t0 = sys_hrtime();
    for (u32 i = 0; i < n; i++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        int s = (int)(r & 63);
        free(q[s]);
        if (!(q[s] = malloc(sz[(r >> 8) & 7]))) bad++;
    }
    u32 e = sys_hrtime() - t0;
    for (int i = 0; i < 64; i++) free(q[i]);
    if (bad) { dprintf(1, "%-34s FAILED: %u blocks not given\n", "malloc + free, small", bad); return; }
    report("malloc + free, small blocks", e, n);
    void *b[8];
    t0 = sys_hrtime();
    for (u32 i = 0; i < n / 8; i++) {
        for (int j = 0; j < 8; j++) if (!(b[j] = malloc(300 + (u32)j * 500))) bad++;
        for (int j = 7; j >= 0; j--) free(b[j]);
    }
    e = sys_hrtime() - t0;
    if (bad) { dprintf(1, "%-34s FAILED: %u blocks not given\n", "malloc + free, 300-3,800 bytes", bad); return; }
    report("malloc + free, 300-3,800 bytes", e, n / 8 * 8);
}

/* bench dir PATH (docs/d49-readdir.md 2 and 9): where a directory listing's time goes, and the two ways of reading it
 * checked against each other. Timed: the null call, a trip to the directory's server with little work (fstat of the
 * open directory), a plain message trip (msgpingd), one record a call (SYS_readdir), the batches (libz's zdir over
 * SYS_readdirs), an lstat of every name (what ls did per name), the first and the last name's lstat. Checked: the same
 * names in the same order with the same inodes both ways, a type the same or the old one 0, every stat a batch gave
 * equal to lstat's (28 bytes), none on "." and "..", SYS_readdirs' -EINVAL for a buffer of 103 bytes and for one not
 * 4-aligned, one record in 104 bytes. "bench dir: identical, N records, S stats" and status 0, else "bench dir: BAD"
 * lines and status 1. With -q only the checks (no timing lines). */
static int bench_dirpath(const char *path, int quiet)
{
    struct ustat st;
    int fd = sys_open(path, O_RDONLY), r, n = 0, cap = 64, bad = 0, nst = 0;
    if (fd < 0) { dprintf(2, "bench: %s: %s\n", path, zstrerror(fd)); return 1; }
    struct udirent *v = malloc((u32)cap * sizeof *v);
    if (!v) return 1;
    u32 t0 = sys_hrtime();
    while ((r = sys_readdir(fd, &v[n])) > 0)
        if (++n == cap) { struct udirent *nv = realloc(v, (u32)(cap *= 2) * sizeof *v); if (!nv) return 1; v = nv; }
    u32 e_old = sys_hrtime() - t0;
    if (r < 0) { dprintf(2, "bench: %s: %s\n", path, zstrerror(r)); return 1; }
    t0 = sys_hrtime();
    struct zdir *z = zdir_open(path);
    const struct udirx *x;
    int k = 0;
    struct ustat *xs = malloc((u32)(n + 1) * sizeof *xs);
    char *hx = calloc((u32)n + 1, 1);
    if (!z || !xs || !hx) { dprintf(2, "bench: %s: %s\n", path, zwhy("cannot open")); return 1; }
    while ((x = zdir_next(z))) {
        if (k < n && (strcmp(x->name, v[k].name) || x->ino != v[k].ino || (v[k].type && x->type != v[k].type))) {
            dprintf(1, "bench dir: BAD record %d: %s #%u type %u, one by one %s #%u type %u\n", k, x->name, x->ino, x->type, v[k].name, v[k].ino, v[k].type);
            bad = 1;
        }
        if (k < n && (x->flags & UDX_STAT)) { memcpy(&xs[k], &x->st, sizeof xs[k]); hx[k] = 1; }
        k++;
    }
    u32 e_new = sys_hrtime() - t0;
    if (zdir_err(z)) { dprintf(1, "bench dir: BAD the batches ended with %s\n", zstrerror(zdir_err(z))); bad = 1; }
    zdir_close(z);
    if (k != n) { dprintf(1, "bench dir: BAD %d records in batches, %d one by one\n", k, n); bad = 1; }
    t0 = sys_hrtime();
    for (int i = 0; i < n; i++) {
        char *full = path_join(path, v[i].name);
        int le = full ? sys_lstat(full, &st) : -ENAMETOOLONG;
        int dot = v[i].name[0] == '.' && (!v[i].name[1] || (v[i].name[1] == '.' && !v[i].name[2]));
        if (hx[i] && (dot || le < 0 || memcmp(&st, &xs[i], sizeof st))) {
            dprintf(1, "bench dir: BAD %s: the batch's stat (mode %o size %u ino %u) is not lstat's (%d: mode %o size %u ino %u)\n",
                    v[i].name, xs[i].mode, xs[i].size, xs[i].ino, le, st.mode, st.size, st.ino);
            bad = 1;
        }
        nst += hx[i];
        free(full);
    }
    u32 e_lstat = sys_hrtime() - t0;
    {   /* the call's own checks: a buffer too small, one not 4-aligned, the smallest that holds one record */
        static u32 b[64];
        sys_lseek(fd, 0, 0);
        int r1 = sys_readdirs(fd, b, 103), r2 = sys_readdirs(fd, (char *)b + 2, 200), r3 = sys_readdirs(fd, b, 104);
        int nosys = r3 == -ENOSYS;
        if (!nosys && (r1 != -EINVAL || r2 != -EINVAL || r3 <= 0 || r3 > 104 || !udirx_step((struct udirx *)b, (u32)r3))) {
            dprintf(1, "bench dir: BAD SYS_readdirs: 103 bytes %d, an odd buffer %d, 104 bytes %d\n", r1, r2, r3);
            bad = 1;
        }
        if (nosys && !quiet) dprintf(1, "bench dir: the directory's server reads one record a call (-ENOSYS: libz's old path)\n");
    }
    if (!quiet) {
        dprintf(1, "dir %s: %d records\n", path, n);
        report("readdir, one record a call", e_old, (u32)n);
        report("readdir in batches (zdir)", e_new, (u32)n);
        report("lstat of every name", e_lstat, (u32)n);
        {   /* the call alone, without libz: its records counted, nothing looked at */
            static u32 b[512];
            u32 recs = 0;
            sys_lseek(fd, 0, 0);
            t0 = sys_hrtime();
            while ((r = sys_readdirs(fd, b, sizeof b)) > 0)
                for (u32 o = 0, st; o < (u32)r && (st = udirx_step((struct udirx *)((char *)b + o), (u32)r - o)); o += st) recs++;
            if (r == 0) report("SYS_readdirs alone, 2 KB a call", sys_hrtime() - t0, recs);
            else dprintf(1, "SYS_readdirs alone: %d after %u records\n", r, recs);
        }
        t0 = sys_hrtime();
        for (int i = 0; i < 5000; i++) sys_getpid();
        report("null system call (getpid)", sys_hrtime() - t0, 5000);
        t0 = sys_hrtime();
        for (int i = 0; i < 300 && !nfail; i++) if ((r = sys_fstat(fd, &st)) < 0) failed("fstat", path, r);
        report_checked("fstat of the directory (a trip)", sys_hrtime() - t0, 300);
        bench_ipc();
        char *fn = 0, *ln = 0;
        for (int i = 0; i < n; i++)
            if (strcmp(v[i].name, ".") && strcmp(v[i].name, "..")) { free(ln); ln = path_join(path, v[i].name); if (!fn && ln) fn = strdup(ln); }
        if (fn && ln) {
            t0 = sys_hrtime();
            for (int i = 0; i < 50 && !nfail; i++) if ((r = sys_lstat(fn, &st)) < 0) failed("lstat", fn, r);
            report_checked("lstat of the first name", sys_hrtime() - t0, 50);
            t0 = sys_hrtime();
            for (int i = 0; i < 50 && !nfail; i++) if ((r = sys_lstat(ln, &st)) < 0) failed("lstat", ln, r);
            report_checked("lstat of the last name", sys_hrtime() - t0, 50);
        }
        free(fn); free(ln);
    }
    sys_close(fd);
    free(v); free(xs); free(hx);
    if (!bad) dprintf(1, "bench dir: identical, %d records, %d stats\n", n, nst);
    return bad;
}

/* bench mmu [BIG] (DESIGN D60 section 9.4, stage 0): the figures the memory protection budget is judged on, each one
 * line "mmubench: NAME VALUE UNIT" (us a call, or KB/s), the same with protection off and on. BIG: a large file of the
 * root (default /bin/tcc). Every call's result is checked: a failure prints "mmubench: NAME FAILED" and exit 1. */
static int mb_bad;
static void mb_line(const char *what, u32 v, const char *unit) { dprintf(1, "mmubench: %s %u %s\n", what, v, unit); }
static void mb_fail(const char *what, int r) { dprintf(1, "mmubench: %s FAILED (%d)\n", what, r); mb_bad = 1; }
/* tenths of a microsecond each, from E-clock ticks e for n calls */
static u32 mb_each(u32 e, u32 n) { return (u32)(((unsigned long long)e * 10000000ull) / E_HZ / n); }
static void mb_each_line(const char *what, u32 e, u32 n)
{
    u32 t = mb_each(e, n);
    dprintf(1, "mmubench: %s %u.%u us\n", what, t / 10, t % 10);
}
static u32 mb_kbs(u32 bytes, u32 e) { u32 t = us(e); return t ? (u32)(((unsigned long long)bytes * 1000000ull) / t / 1024) : 0; }
static int bench_mmu(const char *big)
{
    static char buf[16384];
    struct ustat st;
    int r, fd;
    u32 t0, e, bytes = 0;
    /* a big file, read once through the filesystem server and the disk server (the first read since the boot) */
    if ((fd = sys_open(big, O_RDONLY)) < 0) { mb_fail("bigread", fd); return 1; }
    t0 = sys_hrtime();
    while ((r = sys_read(fd, buf, sizeof buf)) > 0) bytes += (u32)r;
    e = sys_hrtime() - t0;
    sys_close(fd);
    if (r < 0 || bytes < 65536) mb_fail("bigread", r); else mb_line("bigread", mb_kbs(bytes, e), "KB/s");
    t0 = sys_hrtime();
    for (int i = 0; i < 5000; i++) sys_getpid();
    mb_each_line("nullcall", sys_hrtime() - t0, 5000);
    if ((r = sys_stat("/bin/sh", &st)) < 0) { mb_fail("stat", r); return 1; }
    t0 = sys_hrtime();
    for (int i = 0; i < 500; i++) if ((r = sys_stat("/bin/sh", &st)) < 0) break;
    if (r < 0) mb_fail("stat", r); else mb_each_line("stat", sys_hrtime() - t0, 500);
    /* a cached 4 KB read (one call to the filesystem server each) */
    if ((fd = sys_open("/bin/sh", O_RDONLY)) < 0) { mb_fail("read4k", fd); return 1; }
    sys_read(fd, buf, 4096);
    u32 n4 = 0;
    t0 = sys_hrtime();
    for (int i = 0; i < 400; i++) {
        sys_lseek(fd, 0, 0);
        if ((r = sys_read(fd, buf, 4096)) != 4096) break;
        n4++;
    }
    e = sys_hrtime() - t0;
    sys_close(fd);
    if (n4 != 400) mb_fail("read4k", r); else mb_each_line("read4k", e, 800);  /* the lseek and the read: two calls */
    /* a call to a protected server and back (msgpingd, started by us: not a service) */
    {
        static char *const av[] = { "msgpingd", "-port", "mping", 0 };
        int pid = sys_spawn("/bin/msgpingd", av, 0), port = -1, tries = 0;
        if (pid < 0) mb_fail("msgcall", pid);
        else {
            while ((port = sys_port_find("mping")) < 0 && ++tries < 200) sys_sleep(1);
            struct umsg m;
            if (port < 0) mb_fail("msgcall", port);
            else {
                m.type = 1; m.a[0] = 1; m.a[1] = 2;
                sys_call(port, &m);
                int ok = 1;
                t0 = sys_hrtime();
                for (int i = 0; i < 2000 && ok; i++) { m.type = 1; m.a[0] = (u32)i; m.a[1] = 1; sys_call(port, &m); ok = m.a[2] == (u32)i + 1; }
                e = sys_hrtime() - t0;
                if (!ok) mb_fail("msgcall", -1); else mb_each_line("msgcall", e, 2000);
                m.type = 9; sys_call(port, &m);
            }
            if (port < 0) sys_kill(pid);
            sys_wait(pid);
        }
    }
    /* a round trip between two programs: a byte to cat through one pipe, back through another */
    {
        int a[2], b[2];
        if (sys_pipe(a) < 0 || sys_pipe(b) < 0) mb_fail("pipetrip", -1);
        else {
            static char *const av[] = { "cat", 0 };
            int io[3] = { a[0], b[1], 2 };
            int pid = sys_spawn("/bin/cat", av, io);
            sys_close(a[0]); sys_close(b[1]);
            if (pid < 0) mb_fail("pipetrip", pid);
            else {
                char c = 'x';
                int ok = sys_write(a[1], &c, 1) == 1 && sys_read(b[0], &c, 1) == 1;
                t0 = sys_hrtime();
                for (int i = 0; i < 1000 && ok; i++) ok = sys_write(a[1], &c, 1) == 1 && sys_read(b[0], &c, 1) == 1;
                e = sys_hrtime() - t0;
                if (!ok) mb_fail("pipetrip", -1); else mb_each_line("pipetrip", e, 1000);
                sys_close(a[1]);
                sys_wait(pid);
            }
            sys_close(b[0]);
        }
    }
    /* a pipe's throughput: cat BIG | (us), the bytes counted */
    {
        int a[2];
        if (sys_pipe(a) < 0) mb_fail("pipe", -1);
        else {
            static char *av[] = { "cat", 0, 0 };
            av[1] = (char *)big;
            int io[3] = { 0, a[1], 2 };
            t0 = sys_hrtime();
            int pid = sys_spawn("/bin/cat", av, io);
            sys_close(a[1]);
            u32 got = 0;
            if (pid < 0) mb_fail("pipe", pid);
            else {
                while ((r = sys_read(a[0], buf, sizeof buf)) > 0) got += (u32)r;
                e = sys_hrtime() - t0;
                sys_wait(pid);
                if (r < 0 || got != bytes) mb_fail("pipe", r); else mb_line("pipe", mb_kbs(got, e), "KB/s");
            }
            sys_close(a[0]);
        }
    }
    /* ls -laR / > /dev/null, the whole command */
    {
        static char *const av[] = { "ls", "-laR", "/", 0 };
        int nul = sys_open("/dev/null", O_WRONLY);
        int io[3] = { 0, nul, nul };
        t0 = sys_hrtime();
        int pid = nul < 0 ? nul : sys_spawn("/bin/ls", av, io);
        if (pid < 0) mb_fail("lsr", pid);
        else {
            r = sys_wait(pid);
            e = sys_hrtime() - t0;
            if (r != 0) mb_fail("lsr", r); else mb_line("lsr", us(e) / 1000, "ms");
        }
        if (nul >= 0) sys_close(nul);
    }
    dprintf(1, "mmubench: %s\n", mb_bad ? "FAILED" : "done");
    return mb_bad;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "mmu"))
        return bench_mmu(argc >= 3 ? argv[2] : "/bin/tcc");
    if (argc >= 3 && !strcmp(argv[1], "dir")) {
        int q = argc == 4 && !strcmp(argv[2], "-q");
        return bench_dirpath(argv[2 + q], q);
    }
    dprintf(1, "Zenix benchmark (E-clock %u Hz)\n", E_HZ);
    bench_cpu();
    bench_syscall();
    bench_malloc();
    bench_fsstat();
    bench_ipc();
    bench_dir();
    bench_read();
    bench_cold();
    bench_create();
    bench_screen();
    dprintf(1, "done\n");
    return 0;
}
