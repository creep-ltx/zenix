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
    printf("%-34s %8u us total, %6u us each (n=%u)\n", what, total, n ? total / n : 0, n);
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
    printf("cpu loop: %u passes in %u us = %u passes/ms (about %u MHz if 6 clocks/pass)\n",
           n, t, t ? n * 1000 / t : 0, t ? (n * 6) / t : 0);
}

static void bench_syscall(void)
{
    u32 t0 = sys_hrtime();
    for (int i = 0; i < 5000; i++) sys_getpid();
    report("null system call (getpid)", sys_hrtime() - t0, 5000);
}

static void bench_fsstat(void)
{
    struct ustat st;
    sys_stat("/bin/sh", &st);                    /* warm */
    u32 t0 = sys_hrtime();
    for (int i = 0; i < 500; i++) sys_stat("/bin/sh", &st);
    report("stat /bin/sh (call to fs server)", sys_hrtime() - t0, 500);
}

static void bench_ipc(void)
{
    int pid = sys_spawn("/bin/pingd", 0, 0);
    if (pid < 0) { printf("(no pingd, skipping ipc)\n"); return; }
    int port, tries = 0;
    while ((port = sys_port_find("ping")) < 0 && ++tries < 200) sys_sleep(1);
    if (port < 0) return;
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
    u32 t0 = sys_hrtime();
    u32 bytes = 0;
    for (int r = 0; r < 100; r++) {
        sys_lseek(fd, 0, 0);
        int n;
        while ((n = sys_read(fd, buf, sizeof buf)) > 0) bytes += (u32)n;
    }
    u32 e = sys_hrtime() - t0;
    sys_close(fd);
    report("read /bin/sh, 4K chunks, cached", e, 100);
    u32 t = us(e);
    if (t) printf("   = %u KB/s\n", (u32)(((unsigned long long)bytes * 1000000ull) / t / 1024));
}

static void bench_cold(void)
{
    static char buf[4096];
    static const char *files[] = { "/bin/ls", "/bin/grep", "/bin/wc", "/bin/ps", "/bin/cp", "/bin/mv" };
    u32 t0 = sys_hrtime(), bytes = 0;
    for (int i = 0; i < 6; i++) {
        int fd = sys_open(files[i], O_RDONLY), n;
        while ((n = sys_read(fd, buf, sizeof buf)) > 0) bytes += (u32)n;
        sys_close(fd);
    }
    u32 e = sys_hrtime() - t0;
    report("read 6 files, first time (disk)", e, 6);
    u32 t = us(e);
    if (t) printf("   = %u KB/s from the disk (%u bytes)\n", (u32)(((unsigned long long)bytes * 1000000ull) / t / 1024), bytes);
}

static void bench_create(void)
{
    char name[24];
    sys_mkdir("/tmp/bench");
    u32 t0 = sys_hrtime();
    for (int i = 0; i < 50; i++) {
        strcpy(name, "/tmp/bench/f");
        name[12] = (char)('0' + i / 10); name[13] = (char)('0' + i % 10); name[14] = 0;
        int fd = sys_open(name, O_WRONLY | O_CREAT);
        sys_write(fd, "hello", 5);
        sys_close(fd);
    }
    report("create + write 50 small files", sys_hrtime() - t0, 50);
    t0 = sys_hrtime();
    for (int i = 0; i < 50; i++) {
        strcpy(name, "/tmp/bench/f");
        name[12] = (char)('0' + i / 10); name[13] = (char)('0' + i % 10); name[14] = 0;
        sys_unlink(name);
    }
    report("delete 50 files", sys_hrtime() - t0, 50);
    sys_rmdir("/tmp/bench");
}

static void bench_dir(void)
{
    struct udirent d;
    int fd = sys_open("/bin", O_RDONLY);
    u32 t0 = sys_hrtime();
    int total = 0;
    for (int r = 0; r < 50; r++) {
        sys_lseek(fd, 0, 0);
        while (sys_readdir(fd, &d) > 0) total++;
    }
    report("list /bin (entries read)", sys_hrtime() - t0, (u32)total);
    sys_close(fd);
}

static void bench_screen(void)
{
    static char line[80];
    memset(line, 'x', 79); line[79] = '\n';
    sys_ttymirror(0);                            /* the serial port would be the bottleneck */
    u32 t0 = sys_hrtime();
    for (int i = 0; i < 300; i++) sys_write(1, line, 80);
    u32 e = sys_hrtime() - t0;
    sys_ttymirror(1);
    printf("\n");
    report("print 300 lines of 79 chars (scrolling)", e, 300);
}

int main(void)
{
    printf("Zenix benchmark (E-clock %u Hz)\n", E_HZ);
    bench_cpu();
    bench_syscall();
    bench_fsstat();
    bench_ipc();
    bench_dir();
    bench_read();
    bench_cold();
    bench_create();
    bench_screen();
    printf("done\n");
    return 0;
}
