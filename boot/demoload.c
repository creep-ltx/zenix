/* SPDX-License-Identifier: GPL-2.0-only */
/* The demo floppy's stage 2 (DESIGN D25 revision 3, section 6): read by the bootblock (boot/demoboot.S), started
 * while AmigaOS still lives. It shows a screen with a bar, reads the packed stream with trackdisk.device one track at
 * a time (the next track is read while this one is unpacked), unpacks the kernel and the RAM disk's image straight into
 * the fast-RAM buffer they stay in (boot/demounpack.c), checks everything, and hands over as every Zenix boot does
 * (boot/handoff.S through zb_tables and zb_go, as zenixboot). Nothing is ever written to the floppy. The kernel is
 * byte for byte the one of every image and costs nothing here.
 *
 * Any failure stops with the reason on the screen (and on the serial port, 9600 baud, for the tests) and touches
 * nothing: the machine is switched off to start something else. */
#include "demounpack.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

/* the same numbers as boot/handoff.S */
#define KLOG_AREA_SIZE  0x4200
#define HANDOFF_TABLE   0x400
#define HANDOFF_STACK   0x10000

long zb_os(void *base, long lvo, long d0, long d1, long d2, long d3, long a0, long a1);
u32 zb_tables(void *table, void *buffer, u32 ksize);
void zb_serial(const char *fmt, long *args);
void zb_go(void *table, void *buffer, u32 ksize, u32 home) __attribute__((noreturn));
long zb_onstack(long (*fn)(void *), void *arg, void *lower, u32 size);
void demo_screen(int failed, const char *const *lines);   /* boot/demoscreen.c (68000 code) */
void demo_bar(u32 done, u32 all);
long dl_main(u8 *io, u32 table_off);
extern void *SysBase;
void *GfxBase;

/* exec.library */
#define Forbid()            zb_os(SysBase, -132, 0, 0, 0, 0, 0, 0)
#define AllocMem(n, f)      ((u8 *)zb_os(SysBase, -198, (long)(n), (long)(f), 0, 0, 0, 0))
#define AllocAbs(n, a)      ((u8 *)zb_os(SysBase, -204, (long)(n), 0, 0, 0, 0, (long)(a)))
#define FreeMem(a, n)       zb_os(SysBase, -210, (long)(n), 0, 0, 0, 0, (long)(a))
#define AvailMem(f)         ((u32)zb_os(SysBase, -216, 0, (long)(f), 0, 0, 0, 0))
#define DoIO(r)             ((signed char)zb_os(SysBase, -456, 0, 0, 0, 0, 0, (long)(r)))
#define SendIO(r)           zb_os(SysBase, -462, 0, 0, 0, 0, 0, (long)(r))
#define WaitIO(r)           ((signed char)zb_os(SysBase, -474, 0, 0, 0, 0, 0, (long)(r)))
#define RawIOInit()         zb_os(SysBase, -504, 0, 0, 0, 0, 0, 0)
#define OpenLibrary(n, v)   ((void *)zb_os(SysBase, -552, (long)(v), 0, 0, 0, 0, (long)(n)))
#define CacheControl(v, m)  zb_os(SysBase, -648, (long)(v), (long)(m), 0, 0, 0, 0)
#define MEMF_PUBLIC  (1L << 0)
#define MEMF_CHIP    (1L << 1)
#define MEMF_FAST    (1L << 2)
#define MEMF_CLEAR   (1L << 16)
#define MEMF_LARGEST (1L << 17)
#define MEMF_REVERSE (1L << 18)
#define CACRF_EnableD (1L << 8)
/* graphics.library */
#define LoadView(v)         zb_os(GfxBase, -222, 0, 0, 0, 0, 0, (long)(v))
#define WaitBlit()          zb_os(GfxBase, -228, 0, 0, 0, 0, 0, 0)
#define WaitTOF()           zb_os(GfxBase, -270, 0, 0, 0, 0, 0, 0)

#define EXEC_W(off) (*(volatile u16 *)((u8 *)SysBase + (off)))
#define EXEC_L(off) (*(volatile u32 *)((u8 *)SysBase + (off)))

/* trackdisk.device's request (IOExtTD; the ROM Kernel Reference Manual: Devices) */
#define IO_COMMAND 28
#define IO_ERROR 31
#define IO_LENGTH 36
#define IO_DATA 40
#define IO_OFFSET 44
#define IOTD_SIZE 56
#define IOSTD_SIZE 48
#define CMD_READ 2
#define TD_MOTOR 9

/* the compiler may call these, and tinfl does */
void *memcpy(void *d, const void *s, unsigned long n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }

#include "bootmem.h"

/* the bar of the loading screen (boot/demoscreen.c opened it): 400 pixels for the whole stream */
extern void *demo_rp;
#define RectFill(rp, a, b, c, d) zb_os(GfxBase, -306, (long)(a), (long)(b), (long)(c), (long)(d), 0, (long)(rp))
void demo_bar(u32 done, u32 all)
{
    if (!demo_rp || !GfxBase || !all) return;
    while (all > 0xFFFF) { all >>= 1; done >>= 1; }
    if (done > all) done = all;
    u32 w = done * 397 / all;
    if (w) RectFill(demo_rp, 121, 113, 120 + w, 120);
}

static void say(const char *fmt, long a, long b)
{
    long v[2] = { a, b };
    zb_serial(fmt, v);
}

/* the failure screen: the reason in plain words, the same on the serial port */
static long refuse(const char *what, const char *why)
{
    const char *lines[] = { what, why, "Nothing on this disk or in this Amiga was changed.",
                            "Switch the Amiga off to start something else.", 0 };
    say("demo: %s: %s\n", (long)what, (long)why);
    demo_screen(1, lines);
    return 1;
}

static void io_set(u8 *r, u16 cmd, void *data, u32 len, u32 off)
{
    *(u16 *)(r + IO_COMMAND) = cmd;
    *(u32 *)(r + IO_LENGTH) = len;
    *(void **)(r + IO_DATA) = data;
    *(u32 *)(r + IO_OFFSET) = off;
}

static void motor_off(u8 *io)
{
    io_set(io, TD_MOTOR, 0, 0, 0);
    (void)DoIO(io);
}

/* a piece of the stream on the disk: from off to the end of its track, at most to end */
static u32 track;                       /* the table's: 5632 on a DD disk, 11264 on an HD one */
static u32 piece(u32 off, u32 end) { u32 n = track - off % track; return n < end - off ? n : end - off; }

static long dl_run(void *arg);
static u8 *g_io;
static u32 g_table_off;

long dl_main(u8 *io, u32 table_off)
{
    /* the klog area first, before anything is allocated (boot/handoff.S): the kernel reads the previous boot's log there */
    u32 chiptop = EXEC_L(62) & ~0xFFFu;
    (void)AllocAbs(KLOG_AREA_SIZE, chiptop - KLOG_AREA_SIZE);
    zone_lo[0] = 0; zone_hi[0] = HANDOFF_STACK;
    zone_lo[1] = chiptop - KLOG_AREA_SIZE; zone_hi[1] = chiptop;
    RawIOInit();
    say("demo: the Zenix demo disk's loader\n", 0, 0);
    GfxBase = OpenLibrary("graphics.library", 37);
    g_io = io;
    g_table_off = table_off;
    u8 *stack = AllocMem(8192, MEMF_PUBLIC);
    if (!stack) { motor_off(io); return refuse("Not enough memory", "There is no memory for the loader itself."); }
    return zb_onstack(dl_run, 0, stack, 8192);     /* returns only when something failed */
}

static long dl_run(void *arg)
{
    (void)arg;
    u8 *io = g_io, *sector = 0, *reqs[2] = { 0, 0 }, *bufs[2] = { 0, 0 };
    struct dtab t;
    const char *why;

    demo_screen(0, 0);
    sector = AllocMem(DT_SECTOR, MEMF_PUBLIC);
    if (!sector) { motor_off(io); return refuse("Not enough memory", "There is no memory for the loader itself."); }
    io_set(io, CMD_READ, sector, DT_SECTOR, g_table_off);
    int err = DoIO(io);
    if (err) { motor_off(io); return refuse("The floppy could not be read", "Its table sector has a read error."); }
    if ((why = dt_check(sector, g_table_off, &t))) { motor_off(io); return refuse("This is not a usable Zenix demo disk", why); }

    /* the memory: the kernel and the RAM disk in one fast-RAM block, away from where the kernel will run */
    u32 total = t.kern_len + t.image_len;
    u32 home = pick_home();
    zone_lo[2] = home; zone_hi[2] = home + (t.kern_mem > t.kern_len ? t.kern_mem : t.kern_len);   /* the copy too */
    u8 *table = alloc_safe(HANDOFF_TABLE, ALLOC_CHIP);
    u8 *buf = AvailMem(MEMF_FAST | MEMF_LARGEST) >= total && table ? alloc_safe(total, ALLOC_FAST_TOP) : 0;
    struct dunpack *u = buf ? (struct dunpack *)AllocMem(du_state_size(), MEMF_PUBLIC) : 0;
    for (int i = 0; i < 2 && u; i++) {
        reqs[i] = i ? AllocMem(IOTD_SIZE, MEMF_PUBLIC | MEMF_CLEAR) : io;
        bufs[i] = AllocMem(t.track, MEMF_PUBLIC);
        if (!reqs[i] || !bufs[i]) u = 0;
    }
    if (!u) {
        motor_off(io);
        say("demo: %ld bytes of fast RAM wanted in one block\n", (long)total, 0);
        return refuse("This Zenix demo disk needs 4 MB of fast RAM (a 68020 or better).",
                      "This Amiga has not enough fast memory free.");
    }
    memcpy(reqs[1], io, IOSTD_SIZE);    /* (the strap's may be an IOStdReq) a second request to the same drive: one track read while one is unpacked */

    track = t.track;
    du_start(u, buf, total, t.stream_len);
    u32 off = t.stream_off, end = t.stream_off + ((t.stream_len + DT_SECTOR - 1) & ~(DT_SECTOR - 1));
    u32 len[2];
    int cur = 0, r = 0;
    len[0] = piece(off, end);
    io_set(reqs[0], CMD_READ, bufs[0], len[0], off);
    SendIO(reqs[0]);
    while (r == 0) {
        err = WaitIO(reqs[cur]);
        u32 here = off;
        off += len[cur];
        int next = off < end && !err;
        if (next) {
            len[cur ^ 1] = piece(off, end);
            io_set(reqs[cur ^ 1], CMD_READ, bufs[cur ^ 1], len[cur ^ 1], off);
            SendIO(reqs[cur ^ 1]);
        }
        if (err) {
            motor_off(io);
            say("demo: trackdisk error %ld at byte %ld\n", (long)err, (long)here);
            return refuse("The floppy could not be read", "A track has a read error (a damaged disk or a dirty drive).");
        }
        r = du_feed(u, bufs[cur], len[cur]);
        demo_bar(off - t.stream_off, end - t.stream_off);
        if (r == 0 && !next) r = -100;  /* every byte read, and the stream wants more: du_feed said so already */
        if (r != 0 && next) (void)WaitIO(reqs[cur ^ 1]);    /* nothing may still be reading when we stop */
        cur ^= 1;
    }
    motor_off(io);
    if (r < 0) return refuse("The packed system on this disk is damaged", r == -100 ? du_why(-1) : du_why(r));
    if ((why = du_check_parts(buf, &t))) return refuse("The packed system on this disk is damaged", why);
    say("demo: unpacked %ld bytes, the kernel goes to 0x%lx\n", (long)total, (long)home);

    /* stage 2 of the boot colours (boot/handoff.S, A11): stage0 and the memory list, AmigaOS still alive */
    *(volatile u16 *)0xDFF096 = 0x0180;
    *(volatile u16 *)0xDFF100 = 0x0200;
    *(volatile u16 *)0xDFF180 = 0x0FF;
    *(u32 *)(table + 0x74) = t.image_len;   /* bootinfo: the RAM disk's size; handoff_tables adds its address */
    u32 h2 = zb_tables(table, buf, t.kern_len);
    if (h2 != home) return refuse("Internal error", "The kernel's home moved (zb_tables and pick_home disagree).");
    /* Nothing can fail any more: the display off, the 68040/68060 data cache off as after a reset, no other task, no
     * blit running; then zb_go switches AmigaOS off and jumps (boot/handoff-c.S), as zenixboot does */
    LoadView(0);
    WaitTOF();
    WaitTOF();
    if (EXEC_W(296) & 0x88) CacheControl(0, CACRF_EnableD);
    Forbid();
    WaitBlit();
    zb_go(table, buf, t.kern_len, home);
}
