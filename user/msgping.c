/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* test client: a few calls, a zero-copy string call, then N timed round trips. The memory protection tests (DESIGN
 * D60 stage 2, tools/mmu-test.sh): winread FILE [CASE], reads of a file of msgpingd -fswin's pattern into a guarded
 * region (the bytes read must be the file's, every byte around them as before); readpast FILE, a read into the end
 * of its own memory (-EFAULT); rewrite, msgpingd's type 10 (its message rewritten while it is served); call PORT, one
 * call there (its result). */
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
int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "winread"))
        return winread(argv[2], argc > 3 ? atoi(argv[3]) : 0);
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
    sys_call(port, &m);
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
