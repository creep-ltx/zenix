/* SPDX-License-Identifier: GPL-2.0-only */
/* zenixboot: an AmigaOS program that starts Zenix from files, for Amigas that boot from a hard disk, a CF card or
 * (under Emu68) an SD card instead of the Zenix floppy. Typically the only line of S:Startup-Sequence on a small
 * AmigaDOS boot partition that holds just zenixboot and the kernel (docs/man/zenixboot.txt, docs/how-it-works.md).
 *
 * It does what the floppy bootblock does, with the same code for the hand-over (boot/handoff.S): the same bootinfo
 * and hwinfo table, the same stage0 call, the same kernel home, the same klog area at the top of chip RAM, the same
 * stage colours. The difference is the order of failure: everything is read and checked while AmigaOS still runs, and
 * any problem is printed and returns to AmigaOS. Nothing is switched off before the last check has passed.
 *
 * Freestanding (no C library): every AmigaOS call goes through zb_os (boot/zenixboot-start.S). Built with the same
 * m68k-elf compiler as the kernel; tools/mkhunk.py makes the AmigaOS executable. The entry code has already refused
 * a 68000/68010 and Kickstart 1.3 when zb_main runs. */

typedef unsigned long u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define ZB_VERSION "0.1b1"
#define ZB_DATE "23.10.2026"
static const char version[] __attribute__((used, section(".text.start"))) = "$VER: zenixboot " ZB_VERSION " (" ZB_DATE ")";

/* the same numbers as boot/handoff.S */
#define KLOG_AREA_SIZE  0x4200
#define HANDOFF_TABLE   0x400
#define HANDOFF_STACK   0x10000

long zb_os(void *base, long lvo, long d0, long d1, long d2, long d3, long a0, long a1);
u32 zb_tables(void *table, void *buffer, u32 ksize);
void zb_serial(const char *fmt, long *args);
void zb_go(void *table, void *buffer, u32 ksize, u32 home) __attribute__((noreturn));
extern void *SysBase;
static void *DOSBase, *GfxBase;

/* exec.library */
#define Forbid()            zb_os(SysBase, -132, 0, 0, 0, 0, 0, 0)
#define AllocMem(n, f)      ((u8 *)zb_os(SysBase, -198, (long)(n), (long)(f), 0, 0, 0, 0))
#define AllocAbs(n, a)      ((u8 *)zb_os(SysBase, -204, (long)(n), 0, 0, 0, 0, (long)(a)))
#define FreeMem(a, n)       zb_os(SysBase, -210, (long)(n), 0, 0, 0, 0, (long)(a))
#define OpenLibrary(n, v)   ((void *)zb_os(SysBase, -552, (long)(v), 0, 0, 0, 0, (long)(n)))
#define CloseLibrary(b)     zb_os(SysBase, -414, 0, 0, 0, 0, 0, (long)(b))
#define CacheControl(v, m)  zb_os(SysBase, -648, (long)(v), (long)(m), 0, 0, 0, 0)
#define MEMF_CHIP    (1L << 1)
#define MEMF_CLEAR   (1L << 16)
#define MEMF_REVERSE (1L << 18)
#define CACRF_EnableD (1L << 8)
/* dos.library */
#define Open(n, m)          zb_os(DOSBase, -30, 0, (long)(n), (long)(m), 0, 0, 0)
#define Close(f)            zb_os(DOSBase, -36, 0, (long)(f), 0, 0, 0, 0)
#define Read(f, b, n)       zb_os(DOSBase, -42, 0, (long)(f), (long)(b), (long)(n), 0, 0)
#define Seek(f, p, m)       zb_os(DOSBase, -66, 0, (long)(f), (long)(p), (long)(m), 0, 0)
#define IoErr()             zb_os(DOSBase, -132, 0, 0, 0, 0, 0, 0)
#define CurrentDir(l)       zb_os(DOSBase, -126, 0, (long)(l), 0, 0, 0, 0)
#define PrintFault(c, h)    zb_os(DOSBase, -474, 0, (long)(c), (long)(h), 0, 0, 0)
#define GetProgramDir()     zb_os(DOSBase, -600, 0, 0, 0, 0, 0, 0)
#define ReadArgs(t, a, r)   zb_os(DOSBase, -798, 0, (long)(t), (long)(a), (long)(r), 0, 0)
#define FreeArgs(r)         zb_os(DOSBase, -858, 0, (long)(r), 0, 0, 0, 0)
#define VPrintf(f, a)       zb_os(DOSBase, -954, 0, (long)(f), (long)(a), 0, 0, 0)
#define MODE_OLDFILE 1005
#define OFFSET_BEGINNING (-1)
#define OFFSET_END 1
#define ERROR_NO_FREE_STORE 103
/* graphics.library */
#define LoadView(v)         zb_os(GfxBase, -222, 0, 0, 0, 0, 0, (long)(v))
#define WaitTOF()           zb_os(GfxBase, -270, 0, 0, 0, 0, 0, 0)
#define WaitBlit()          zb_os(GfxBase, -228, 0, 0, 0, 0, 0, 0)

/* the stage colours of boot/handoff.S and kernel/diag.h */
#define DIAG_BLUE 0x00F
#define DIAG_CYAN 0x0FF

#define EXEC_W(off) (*(volatile u16 *)((u8 *)SysBase + (off)))
#define EXEC_L(off) (*(volatile u32 *)((u8 *)SysBase + (off)))
#define CUSTOM(off) (*(volatile u16 *)(0xDFF000 + (off)))

/* a message to the console and to the serial port */
static void say(const char *fmt, long a, long b, long c)
{
    long v[3] = { a, b, c };
    VPrintf(fmt, v);
    zb_serial(fmt, v);
}

/* ---- the display: the stage colours while loading, and back to AmigaOS's screen if something fails ---- */
static int display_off;

static void colour(u16 c)
{
    if (!display_off) {
        CUSTOM(0x96) = 0x0180;          /* DMACON: copper and bitplane DMA off: COLOR00 fills the screen */
        CUSTOM(0x100) = 0x0200;         /* BPLCON0: no bitplanes */
        CUSTOM(0x106) = 0;              /* BPLCON3 */
        display_off = 1;
    }
    CUSTOM(0x180) = c;
}

static void display_back(void)
{
    if (!display_off) return;
    CUSTOM(0x96) = 0x8180;              /* the copper restarts from its list at the next frame and draws the screen */
    WaitTOF();
    WaitTOF();
    display_off = 0;
}

/* ---- memory ---- */
/* Where nothing of ours may lie: low memory (vectors, the copier at 0x500, the table's home at 0x1000, the kernel's
 * first stack below 0x10000), the klog area at the top of chip RAM (the kernel writes its log there at once), and
 * the kernel's home with its BSS (the copy and the BSS clear would overwrite it). */
static u32 zone_lo[3], zone_hi[3];

static int overlaps(const u8 *p, u32 n)
{
    u32 a = (u32)p, b = a + n;
    for (int i = 0; i < 3; i++)
        if (a < zone_hi[i] && zone_lo[i] < b) return 1;
    return 0;
}

/* AllocMem outside the zones: a block that lands in one is kept (so the next try gets another) and given back at
 * the end. Chip RAM first (as the floppy loads), then any memory. */
static u8 *alloc_safe(u32 n)
{
    u8 *keep[16];
    u32 nkeep = 0;
    u8 *p = 0;
    for (int pass = 0; pass < 2 && !p; pass++) {
        long flags = MEMF_CLEAR | MEMF_REVERSE | (pass == 0 ? MEMF_CHIP : 0);
        while (nkeep < 16) {
            u8 *q = AllocMem(n, flags);
            if (!q) break;
            if (!overlaps(q, n)) { p = q; break; }
            keep[nkeep++] = q;
        }
    }
    while (nkeep) FreeMem(keep[--nkeep], n);
    return p;
}

/* The kernel's home, chosen the same way as handoff_tables does (largest fast region of the first 8 in exec's list,
 * 256 KB aligned, else chip 0x40000): needed before the buffers are allocated, so that they keep out of it.
 * zb_tables chooses again; the two must agree (checked). */
static u32 pick_home(void)
{
    u32 best = 0, home = 0x40000;
    u8 *mh = (u8 *)EXEC_L(322);         /* MemList lh_Head */
    for (int n = 0; n < 8 && *(u32 *)mh; n++, mh = (u8 *)*(u32 *)mh) {
        u16 attrs = *(u16 *)(mh + 14);
        u32 lo = (*(u32 *)(mh + 20) + 0x3FFFF) & 0xFFFC0000, hi = *(u32 *)(mh + 24);
        if (!(attrs & 4) || hi <= lo) continue;
        if (hi - lo > best) { best = hi - lo; home = lo; }
    }
    return home;
}

/* ---- files ---- */
static long open_file(const char *name, int in_progdir)
{
    long old = 0, fh;
    long dir = in_progdir ? GetProgramDir() : 0;
    if (dir) old = CurrentDir(dir);
    fh = Open(name, MODE_OLDFILE);
    if (dir) CurrentDir(old);
    return fh;
}

static long file_size(long fh)
{
    if (Seek(fh, 0, OFFSET_END) < 0) return -1;
    return Seek(fh, 0, OFFSET_BEGINNING);  /* returns the position before: the end */
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* an AmigaOS error: its text on the console, its number on the serial port (the texts are in dos.library) */
static int fail_dos(const char *what)
{
    long e = IoErr();
    long v[2] = { (long)what, e };
    display_back();
    PrintFault(e, what);
    zb_serial("zenixboot: %s: AmigaOS error %ld\n", v);
    return 20;
}

int zb_main(void)
{
    static const char tmpl[] = "KERNEL,RAM/K,CHECK/S";
    long args[3] = { 0, 0, 0 };
    long rda, kfh = 0, rfh = 0, ksize, rsize = 0;
    u8 *klog = 0, *table = 0, *buf = 0, hdr[32];
    u32 k512, r512, total = 0, home;
    int rc = 20;

    DOSBase = OpenLibrary("dos.library", 37);
    GfxBase = OpenLibrary("graphics.library", 37);
    if (!DOSBase || !GfxBase) return 20;
    zb_os(SysBase, -504, 0, 0, 0, 0, 0, 0);     /* RawIOInit: the serial port at 9600 baud for zb_serial */
    rda = ReadArgs(tmpl, args, 0);
    if (!rda) { fail_dos("zenixboot"); return 20; }
    const char *kname = args[0] ? (const char *)args[0] : "zenix";
    const char *rname = (const char *)args[1];
    int check = args[2] != 0;

    /* the klog area first, before anything else can be allocated there (as the bootblock); if AmigaOS already uses
     * part of it, the zones below keep our buffers out of it */
    u32 chiptop = EXEC_L(62);           /* MaxLocMem */
    klog = AllocAbs(KLOG_AREA_SIZE, chiptop - KLOG_AREA_SIZE);
    zone_lo[0] = 0; zone_hi[0] = HANDOFF_STACK;
    zone_lo[1] = chiptop - KLOG_AREA_SIZE; zone_hi[1] = chiptop;

    /* the kernel: a ZEXE image; its header says how much memory it needs at its home */
    kfh = open_file(kname, !args[0]);
    if (!kfh) { rc = fail_dos(kname); goto out; }
    ksize = file_size(kfh);
    if (ksize < 32 || Read(kfh, hdr, 32) != 32 || Seek(kfh, 0, OFFSET_BEGINNING) < 0) {
        say("zenixboot: %s: cannot read it, or it is too short to be a kernel\n", (long)kname, 0, 0);
        goto out;
    }
    u32 image = be32(hdr + 8), mem = be32(hdr + 12), nrel = be32(hdr + 16), reloff = be32(hdr + 20), st0 = be32(hdr + 28);
    if (hdr[4] != 'Z' || hdr[5] != 'E' || hdr[6] != 'X' || hdr[7] != 'E' || image > (u32)ksize || image < 32 ||
        mem < image || mem > 0x1000000 || reloff < image || nrel > ((u32)ksize - reloff) / 4 || st0 >= image) {
        say("zenixboot: %s is not a Zenix kernel (build/kernel.bin, a ZEXE image)\n", (long)kname, 0, 0);
        goto out;
    }
    k512 = ((u32)ksize + 511) & ~511UL;
    home = pick_home();
    zone_lo[2] = home; zone_hi[2] = home + mem;

    if (rname) {
        rfh = open_file(rname, 0);
        if (!rfh) { rc = fail_dos(rname); goto out; }
        rsize = file_size(rfh);
        if (rsize < 2048) { say("zenixboot: %s is too small to be a RAM-disk image\n", (long)rname, 0, 0); goto out; }
    }
    r512 = ((u32)rsize + 511) & ~511UL;
    total = k512 + r512;

    table = alloc_safe(HANDOFF_TABLE);
    buf = table ? alloc_safe(total) : 0;
    if (!buf) {
        say("zenixboot: not enough free memory: %ld bytes for the kernel%s, away from where the kernel will run "
            "(0x%lx)\n", (long)total, (long)(rname ? " and the RAM disk" : ""), (long)home);
        goto out;
    }

    say("zenixboot " ZB_VERSION ": loading %s (%ld bytes)", (long)kname, ksize, 0);
    if (rname) say(" and the RAM disk %s (%ld bytes)", (long)rname, rsize, 0);
    say("\n", 0, 0, 0);

    colour(DIAG_BLUE);                   /* stage 1: loading */
    if (Read(kfh, buf, ksize) != ksize) { rc = fail_dos(kname); goto out; }
    if (rname && Read(rfh, buf + k512, rsize) != rsize) { rc = fail_dos(rname); goto out; }
    Close(kfh); kfh = 0;
    if (rfh) { Close(rfh); rfh = 0; }
    /* the header again, from memory (the file could have changed between the two reads) */
    if (be32(buf + 8) != image || be32(buf + 12) != mem || be32(buf + 28) != st0 || buf[4] != 'Z') {
        display_back();
        say("zenixboot: %s changed while it was read\n", (long)kname, 0, 0);
        goto out;
    }
    if (rname && !(buf[k512 + 1080] == 0x53 && buf[k512 + 1081] == 0xEF)) {
        display_back();
        say("zenixboot: %s is not an ext2 filesystem image (the RAM disk must be one)\n", (long)rname, 0, 0);
        goto out;
    }
    *(u32 *)(table + 0x74) = r512;      /* bootinfo: the RAM disk's size (0 = none); handoff_tables adds its address */

    colour(DIAG_CYAN);                   /* stage 2: stage0 and the memory list, AmigaOS still alive */
    u32 h2 = zb_tables(table, buf, k512);
    if (h2 != home) {
        display_back();
        say("zenixboot: internal error: the kernel's home is 0x%lx, expected 0x%lx\n", (long)h2, (long)home, 0);
        goto out;
    }
    if (check) {
        display_back();
        say("zenixboot: CHECK: everything is loaded and checked; Zenix would run at 0x%lx (%ld bytes there).\n"
            "zenixboot: CHECK: not started; back to AmigaOS.\n", (long)home, (long)mem, 0);
        rc = 0;
        goto out;
    }

    /* Now nothing can fail any more. Show no screen (an RTG driver gives the monitor back to the chipset on a NULL
     * view; not verified), 68040/68060 data cache off (with copyback flushed by exec) as after a reset, no other
     * task, no blit in progress; then zb_go switches AmigaOS off and jumps (boot/zenixboot-start.S). */
    LoadView(0);
    WaitTOF();
    WaitTOF();
    if (EXEC_W(296) & 0x88) CacheControl(0, CACRF_EnableD);
    Forbid();
    WaitBlit();
    zb_go(table, buf, k512, home);

out:
    display_back();
    if (kfh) Close(kfh);
    if (rfh) Close(rfh);
    if (buf) FreeMem(buf, total);
    if (table) FreeMem(table, HANDOFF_TABLE);
    if (klog) FreeMem(klog, KLOG_AREA_SIZE);
    FreeArgs(rda);
    CloseLibrary(GfxBase);
    CloseLibrary(DOSBase);
    return rc;
}
