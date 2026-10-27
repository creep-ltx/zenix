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
 * Updates (DESIGN D15, docs/d15-self-updates.md section 6): with the kernel in the directory Zenix/ next to it,
 * zenixboot installs the kernel of a bundle found there (Zenix/zenix-update.zup) in a careful order, keeps the
 * previous one as zenix.old, hands the bundle to the new kernel in memory for zup (the root half), and follows the
 * trial: the update note and the previous boot's log say whether the new version came up; a crash or a hang (a warm
 * reset) leads to a question, "Zenix (previous)" (PREVIOUS) to the old kernel. zenixboot and bootmenu are never
 * updated by a bundle.
 *
 * Freestanding (no C library): every AmigaOS call goes through zb_os (boot/zenixboot-start.S). Built with the same
 * m68k-elf compiler as the kernel; tools/mkhunk.py makes the AmigaOS executable. The entry code has already refused
 * a 68000/68010 and Kickstart 1.3 when zb_main runs. */

typedef unsigned long u32;
typedef unsigned short u16;
typedef unsigned char u8;
#include "../user/sha256.h"

#define ZB_VERSION "0.2b4"
#define ZB_DATE "27.10.2026"
static const char version[] __attribute__((used, section(".text.start"))) = "$VER: zenixboot " ZB_VERSION " (" ZB_DATE ")";

/* the same numbers as boot/handoff.S */
#define KLOG_AREA_SIZE  0x4200
#define HANDOFF_TABLE   0x400
#define HANDOFF_STACK   0x10000
/* the same numbers as kernel/kernel.h and kernel/serial.c (frozen: the klog area, the update note, bootinfo) */
#define KLOG_HDR        256
#define KLOG_SIZE       8192
#define KLOG_MAGIC      0x5A4B4C47u     /* "ZKLG" */
#define KLOG_END_RUNNING 0u
#define KLOG_END_PANIC  1u
#define UPDNOTE_OFF     0xE0u
#define UPDNOTE_MAGIC   0x5A55504Eu     /* "ZUPN" */
#define UPDNOTE_FAILED  2u
#define UPDNOTE_CONFIRMED 3u
#define UPDNOTE_ROOTFAILED 4u   /* it came up, but its root half failed (audit3 B1): the bundle is kept */
#define UPD_F_ROLLBACK  1u
#define BI_UPD          0x78            /* bootinfo: bundle address, size, flags */
#define HWINFO_DRYRUN   0x5A485744u     /* kernel/hwinfo.h: "ZHWD" in the hwinfo magic, stage0 writes no hardware */
#define ZUP_HDR 96
#define ZUP_TOCE 80
#define ZUP_MAX (16u << 20)

long zb_os(void *base, long lvo, long d0, long d1, long d2, long d3, long a0, long a1);
long zb_dopkt(void *dosbase, void *port, long action);
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
#define MEMF_FAST    (1L << 2)
#define MEMF_CLEAR   (1L << 16)
#define MEMF_REVERSE (1L << 18)
#define CACRF_EnableD (1L << 8)
/* dos.library (offsets: the ROM Kernel Reference Manual's function offset table) */
#define Open(n, m)          zb_os(DOSBase, -30, 0, (long)(n), (long)(m), 0, 0, 0)
#define Close(f)            zb_os(DOSBase, -36, 0, (long)(f), 0, 0, 0, 0)
#define Read(f, b, n)       zb_os(DOSBase, -42, 0, (long)(f), (long)(b), (long)(n), 0, 0)
#define Write(f, b, n)      zb_os(DOSBase, -48, 0, (long)(f), (long)(b), (long)(n), 0, 0)
#define Input()             zb_os(DOSBase, -54, 0, 0, 0, 0, 0, 0)
#define Seek(f, p, m)       zb_os(DOSBase, -66, 0, (long)(f), (long)(p), (long)(m), 0, 0)
#define DeleteFile(n)       zb_os(DOSBase, -72, 0, (long)(n), 0, 0, 0, 0)
#define Rename(a, b)        zb_os(DOSBase, -78, 0, (long)(a), (long)(b), 0, 0, 0)
#define Lock(n, m)          zb_os(DOSBase, -84, 0, (long)(n), (long)(m), 0, 0, 0)
#define UnLock(l)           zb_os(DOSBase, -90, 0, (long)(l), 0, 0, 0, 0)
#define Info(l, d)          zb_os(DOSBase, -114, 0, (long)(l), (long)(d), 0, 0, 0)
#define IoErr()             zb_os(DOSBase, -132, 0, 0, 0, 0, 0, 0)
#define CurrentDir(l)       zb_os(DOSBase, -126, 0, (long)(l), 0, 0, 0, 0)
#define WaitForChar(f, us)  zb_os(DOSBase, -204, 0, (long)(f), (long)(us), 0, 0, 0)
#define SetMode(f, m)       zb_os(DOSBase, -426, 0, (long)(f), (long)(m), 0, 0, 0)
#define PrintFault(c, h)    zb_os(DOSBase, -474, 0, (long)(c), (long)(h), 0, 0, 0)
#define GetProgramDir()     zb_os(DOSBase, -600, 0, 0, 0, 0, 0, 0)
#define GetDeviceProc(n, d) ((u32 *)zb_os(DOSBase, -642, 0, (long)(n), (long)(d), 0, 0, 0))
#define FreeDeviceProc(d)   zb_os(DOSBase, -648, 0, (long)(d), 0, 0, 0, 0)
#define ReadArgs(t, a, r)   zb_os(DOSBase, -798, 0, (long)(t), (long)(a), (long)(r), 0, 0)
#define FreeArgs(r)         zb_os(DOSBase, -858, 0, (long)(r), 0, 0, 0, 0)
#define VPrintf(f, a)       zb_os(DOSBase, -954, 0, (long)(f), (long)(a), 0, 0, 0)
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
#define SHARED_LOCK  (-2)
#define OFFSET_BEGINNING (-1)
#define OFFSET_END 1
#define ERROR_NO_FREE_STORE 103
#define ACTION_FLUSH 27                 /* the Guru Book 21.2.5.3: write all buffers; replied when done */
#define ID_VALIDATED 82                 /* InfoData id_DiskState: writable (80 write protected, 81 validating) */
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

/* the compiler may call these (struct copies), and sha256.c does */
void *memcpy(void *d, const void *s, unsigned long n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }
static int memeq(const void *a, const void *b, u32 n) { const u8 *p = a, *q = b; while (n--) if (*p++ != *q++) return 0; return 1; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

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
 * the end. Chip RAM first (as the floppy loads), then any memory; the bundle (fast) fast RAM first, then any. */
static u8 *alloc_safe(u32 n, int fast)
{
    u8 *keep[16];
    u32 nkeep = 0;
    u8 *p = 0;
    for (int pass = 0; pass < 2 && !p; pass++) {
        long flags = pass ? 0 : fast ? MEMF_FAST : MEMF_CHIP;
        flags |= fast ? 0 : MEMF_CLEAR | MEMF_REVERSE;
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
static long zdir;                       /* a lock on Zenix/ next to zenixboot (the update layout), 0 if there is none */
static long olddir;                     /* the shell's directory while Zenix/ is the current one (0: not changed) */

static long open_file(const char *name, int in_progdir)
{
    long old = 0, fh;
    long dir = in_progdir ? (zdir ? zdir : GetProgramDir()) : olddir;   /* RAM=FILE: the shell's directory */
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

/* ---- the update files in Zenix/ (every name below is relative to it: CurrentDir(zdir) while they are used) ---- */
static const char B_ZUP[] = "zenix-update.zup", B_PART[] = "zenix-update.zup.part", B_BAD[] = "zenix-update.bad",
                  B_DONE[] = "zenix-update.done", B_REJ[] = "zenix-update.rejected";
static int wrote;                       /* something was written on BOOT: (flushed again before AmigaOS goes) */

/* ACTION_FLUSH to the volume's handler, and wait for its reply: FFS keeps metadata (headers, the bitmap) in its
 * buffers; after this they are on the disk */
static void flushfs(void)
{
    u32 *dp = GetDeviceProc("PROGDIR:", 0);
    if (dp) {
        zb_dopkt(DOSBase, (void *)dp[0], ACTION_FLUSH);
        FreeDeviceProc(dp);
    }
    say("zenixboot: update: flushed\n", 0, 0, 0);
}
static int exists(const char *name)
{
    long l = Lock(name, SHARED_LOCK);
    if (l) UnLock(l);
    return l != 0;
}
static void del(const char *name) { if (exists(name)) { DeleteFile(name); wrote = 1; } }
/* AmigaDOS Rename fails when the target exists: the target goes first */
static int ren(const char *a, const char *b)
{
    del(b);
    wrote = 1;
    if (!Rename(a, b)) { fail_dos(a); return -1; }
    say("zenixboot: update: renamed %s to %s\n", (long)a, (long)b, 0);
    return 0;
}
static int write_file(const char *name, const u8 *p, u32 n)
{
    long fh = Open(name, MODE_NEWFILE);
    wrote = 1;
    if (!fh) { fail_dos(name); return -1; }
    long w = Write(fh, p, n);
    Close(fh);
    if (w != (long)n) { fail_dos(name); return -1; }
    return 0;
}
static u8 *iobuf;                       /* 4 KB for reading files piece by piece */
static int file_sha(const char *name, u8 out[32], u32 *size)
{
    long fh = Open(name, MODE_OLDFILE), n;
    if (!fh || !iobuf) { if (fh) Close(fh); return -1; }
    struct sha256 c;
    u32 total = 0;
    sha256_init(&c);
    while ((n = Read(fh, iobuf, 4096)) > 0) { sha256_update(&c, iobuf, (unsigned)n); total += (u32)n; }
    Close(fh);
    if (n < 0) return -1;
    sha256_final(&c, out);
    if (size) *size = total;
    return 0;
}
static int file_has(const char *name, const u8 *sha)
{
    u8 h[32];
    return file_sha(name, h, 0) == 0 && memeq(h, sha, 32);
}

/* ---- the state file (Zenix/state, frozen format: text, "zenixboot-state 1" ... "end") ---- */
enum { PH_NONE, PH_INSTALLING, PH_TRIAL, PH_CONFIRMED, PH_REJECTED, PH_SKIPPED };
static const char *const phases[] = { "none", "installing", "trial", "confirmed", "rejected", "skipped" };
static struct { u32 version, handovers, lastboot; u8 ksha[32]; int haveksha, phase; } st;

static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_u(char *p, u32 v)
{
    char t[12]; int n = 0;
    do t[n++] = (char)('0' + v % 10); while ((v /= 10));
    while (n) *p++ = t[--n];
    return p;
}
static u32 get_u(const char *s, const char **end)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (u32)(*s++ - '0');
    if (end) *end = s;
    return v;
}
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static int state_parse(char *t, long n)
{
    if (n < 18 || !memeq(t, "zenixboot-state 1\n", 18)) return -1;
    t[n] = 0;
    int end = 0;
    st.version = st.handovers = st.lastboot = 0; st.haveksha = 0; st.phase = PH_NONE;
    for (char *l = t; *l; ) {
        char *e = l;
        while (*e && *e != '\n') e++;
        if (*e) *e++ = 0;
        char *v = l;
        while (*v && *v != ' ') v++;
        if (*v) *v++ = 0;
        if (streq(l, "version")) st.version = get_u(v, 0);
        else if (streq(l, "handovers")) st.handovers = get_u(v, 0);
        else if (streq(l, "lastboot")) st.lastboot = get_u(v, 0);
        else if (streq(l, "phase")) { for (int i = 0; i < 6; i++) if (streq(v, phases[i])) st.phase = i; }
        else if (streq(l, "kernelsha")) {
            st.haveksha = 1;
            for (int i = 0; i < 32; i++) {
                int a = hexv(v[2 * i]), b = a < 0 ? -1 : hexv(v[2 * i + 1]);
                if (b < 0) { st.haveksha = 0; break; }
                st.ksha[i] = (u8)(a << 4 | b);
            }
        } else if (streq(l, "end")) end = 1;
        l = e;
    }
    return end ? 0 : -1;
}
/* "state", or "state.new" when a write was cut between deleting the old one and renaming the new one */
static void state_read(void)
{
    static char buf[512];
    const char *names[2] = { "state", "state.new" };
    st.phase = PH_NONE;
    for (int i = 0; i < 2; i++) {
        long fh = Open(names[i], MODE_OLDFILE), n;
        if (!fh) continue;
        n = Read(fh, buf, sizeof buf - 1);
        Close(fh);
        if (n > 0 && state_parse(buf, n) == 0) return;
    }
    st.phase = PH_NONE;
}
static int state_write(void)
{
    static char buf[512];
    static const char hx[] = "0123456789abcdef";
    char *p = put_s(buf, "zenixboot-state 1\nversion ");
    p = put_u(p, st.version);
    if (st.haveksha) {
        p = put_s(p, "\nkernelsha ");
        for (int i = 0; i < 32; i++) { *p++ = hx[st.ksha[i] >> 4]; *p++ = hx[st.ksha[i] & 15]; }
    }
    p = put_s(p, "\nphase "); p = put_s(p, phases[st.phase]);
    p = put_s(p, "\nhandovers "); p = put_u(p, st.handovers);
    p = put_s(p, "\nlastboot "); p = put_u(p, st.lastboot);
    p = put_s(p, "\nend\n");
    del("state.new");
    if (write_file("state.new", (u8 *)buf, (u32)(p - buf))) return -1;
    flushfs();
    if (ren("state.new", "state")) return -1;   /* deletes state first */
    flushfs();
    say("zenixboot: update: state %s, version %ld\n", (long)phases[st.phase], (long)st.version, 0);
    return 0;
}

/* ---- the previous boot's log and the update note (the klog area, only when our AllocAbs of it succeeded) ---- */
static u8 *klog;
static u32 klog_newest(u32 *reason)     /* the newest valid slot's boot number (by its header), 0 if none */
{
    u32 best = 0;
    for (int s = 0; klog && s < 2; s++) {
        const u32 *h = (const u32 *)(klog + s * (KLOG_HDR + KLOG_SIZE));
        if (h[0] != KLOG_MAGIC || h[1] != KLOG_SIZE || h[6] != ~(h[0] + h[1] + h[2] + h[3] + h[4] + h[5])) continue;
        if (h[2] > best) { best = h[2]; *reason = h[5]; }
    }
    return best;
}
static u32 *note(void)                  /* the update note if it is valid, else 0 */
{
    u32 *n = klog ? (u32 *)(klog + UPDNOTE_OFF) : 0;
    return n && n[0] == UPDNOTE_MAGIC && n[3] == ~(n[0] + n[1] + n[2]) ? n : 0;
}
static void note_clear(void) { if (klog) memset(klog + UPDNOTE_OFF, 0, 16); }

/* ---- the bundle ---- */
static u8 *bun, *bunraw;                /* the bundle in memory (16-aligned, 16 bytes free before it), its AllocMem */
static u32 bunsize, bunalloc, bunv;
static const u8 *bkern;                 /* its boot/zenix part */
static u32 bkernsize;
static u8 bksha[32];

/* every count, size and offset checked (untrusted: anyone who can write BOOT: wrote it); with full, the SHA-256 of
 * every part too. 0, or a reason */
static const char *bundle_check(const u8 *p, u32 len, int full)
{
    if (len < ZUP_HDR || !memeq(p, "ZUP1", 4) || be32(p + 4) != 1 || be32(p + 8) != ZUP_HDR) return "not a ZUP1 bundle";
    u32 ntoc = be32(p + 52);
    if (be32(p + 56) != len) return "its size is not the one its header says (cut short?)";
    if (be32(p + 48) != ZUP_HDR || ntoc < 1 || ntoc > 64 || ntoc * ZUP_TOCE > len - ZUP_HDR) return "a bad table of contents";
    u8 h[32];
    struct sha256 c;
    sha256_init(&c); sha256_update(&c, p, 64); sha256_update(&c, p + ZUP_HDR, ntoc * ZUP_TOCE); sha256_final(&c, h);
    if (!memeq(h, p + 64, 32)) return "the table of contents does not match its SHA-256";
    bkern = 0;
    for (u32 i = 0; i < ntoc; i++) {
        const u8 *e = p + ZUP_HDR + i * ZUP_TOCE;
        u32 off = be32(e + 40), size = be32(e + 44), z = 0;
        while (z < 40 && e[z]) z++;
        if (z == 40) return "a part's name is not terminated";
        if (off < ZUP_HDR + ntoc * ZUP_TOCE || off > len || size > len - off) return "a part lies outside the bundle";
        if (full) {
            sha256(p + off, size, h);
            if (!memeq(h, e + 48, 32)) return "a part does not match its SHA-256 (damaged)";
        }
        if (streq((const char *)e, "boot/zenix")) { bkern = p + off; bkernsize = size; memcpy(bksha, e + 48, 32); }
    }
    if (!bkern) return "it has no kernel (boot/zenix)";
    bunv = be32(p + 12);
    return 0;
}
/* The roots a kernel can serve (DESIGN D19 step 1): its version record "ZVER" (kernel/syscall.c), grown by "ZSRV" and
 * a mask (1 ext2, 2 ZenixFS). The shipped kernel has no ZenixFS server built in; only the test-only kernel-zfs has.
 * ROOTS_NONE: no record found; ROOTS_OLD: a record from before ZSRV (that kernel carried both servers, but it never
 * said so on purpose, so it is no evidence of a ZenixFS root). */
#define ROOTS_ZENIXFS 2u
#define ROOTS_NONE 0x80000000u
#define ROOTS_OLD  0x40000000u
static u32 roots_in(const u8 *p, u32 n, u32 *at)
{
    for (u32 i = 0; i + 32 <= n; i++)
        if (p[i] == 'Z' && memeq(p + i, "ZVER", 4)) {
            if (at) *at = i;
            return memeq(p + i + 24, "ZSRV", 4) ? be32(p + i + 28) & 0xFFFFu : ROOTS_OLD;
        }
    if (at) *at = n > 31 ? n - 31 : 0;      /* where the next piece must start searching again */
    return ROOTS_NONE;
}
static u32 roots_of_file(const char *name)
{
    static u8 win[4096 + 32];
    long fh = Open(name, MODE_OLDFILE), n;
    u32 keep = 0, r = ROOTS_NONE, at;
    if (!fh) return ROOTS_NONE;
    while (r == ROOTS_NONE && (n = Read(fh, win + keep, 4096)) > 0) {
        u32 total = keep + (u32)n;
        r = roots_in(win, total, &at);
        keep = total - at;                  /* the last 31 bytes: a record that straddles two pieces */
        memcpy(win, win + at, keep);        /* forward: safe, the target lies below */
    }
    Close(fh);
    return r;
}
/* an update must never make a ZenixFS-root machine unbootable: a machine that runs a kernel which says it serves
 * ZenixFS roots (the test kernel kernel-zfs) gets no kernel that says it cannot. 0, or the reason */
static const char *roots_check(void)
{
    u32 have = roots_of_file("zenix"), want = roots_in(bkern, bkernsize, 0);
    if ((have & (ROOTS_NONE | ROOTS_OLD)) || !(have & ROOTS_ZENIXFS))
        return 0;                           /* no evidence of a ZenixFS root */
    if (!(want & (ROOTS_NONE | ROOTS_OLD)) && !(want & ROOTS_ZENIXFS))
        return "its kernel has no ZenixFS server, and this machine runs a kernel for a ZenixFS root (kernel-zfs)";
    return 0;
}
/* the whole file into memory, fast RAM first */
static const char NOMEM[] = "not enough memory for it";
static const char *bundle_load(long size)
{
    if (size < ZUP_HDR || (u32)size > ZUP_MAX) return "its size is out of range";
    long fh = Open(B_ZUP, MODE_OLDFILE);
    if (!fh) return "it cannot be opened";
    bunalloc = (u32)size + 32;
    bunraw = alloc_safe(bunalloc, 1);
    if (!bunraw) { Close(fh); bunalloc = 0; return NOMEM; }
    bun = (u8 *)(((u32)bunraw + 16 + 15) & ~15u);
    bunsize = (u32)size;
    long n = Read(fh, bun, size);
    Close(fh);
    return n == size ? 0 : "it cannot be read";
}
static void bundle_free(void)
{
    if (bunraw) FreeMem(bunraw, bunalloc);
    bunraw = bun = 0;
    bunsize = bunalloc = 0;
}
/* a damaged or unusable bundle: renamed .bad (never re-hashed at every start), the reason said */
static void bundle_bad(const char *why)
{
    say("zenixboot: update: Zenix/%s is not used: %s; renamed %s\n", (long)B_ZUP, (long)why, (long)B_BAD);
    bundle_free();
    ren(B_ZUP, B_BAD);
    flushfs();
}

/* before any write: the volume writable and validated, and room for zenix.new besides everything there */
static const char *volume_check(u32 need)
{
    static long id[10] __attribute__((aligned(4)));
    if (!Info(zdir, id)) return "Info() failed";
    if (id[2] != ID_VALIDATED) return id[2] == 81 ? "the volume is being validated" : "the volume is write-protected";
    u32 bpb = (u32)id[5] ? (u32)id[5] : 512, freeb = (u32)(id[3] - id[4]);
    u32 blocks = (need + bpb - 1) / bpb, ext = blocks / 72 + 2;     /* data, extension and header blocks */
    if (freeb < blocks + ext + 8) return "not enough free space on the volume";
    return 0;
}

/* The kernel steps for V (section 6's order): zenix.new written, read back and checked, flushed; zenix.old deleted
 * (never while zenix is missing); zenix renamed zenix.old, flushed; zenix.new renamed zenix, flushed. Repeatable:
 * zenix with V's SHA-256 means done, and zenix missing means the rename to zenix.old happened (zenix.old untouched). */
static int kernel_steps(void)
{
    if (file_has("zenix", bksha)) { say("zenixboot: update: Zenix/zenix is the new kernel already\n", 0, 0, 0); return 0; }
    int have = exists("zenix");
    if (have || !file_has("zenix.new", bksha)) {
        del("zenix.new");
        say("zenixboot: update: writing zenix.new (%ld bytes)\n", (long)bkernsize, 0, 0);
        if (write_file("zenix.new", bkern, bkernsize)) return -1;
        if (!file_has("zenix.new", bksha)) { say("zenixboot: update: zenix.new does not read back right\n", 0, 0, 0); del("zenix.new"); return -1; }
        flushfs();
    }
    if (have) {
        del("zenix.old");
        if (ren("zenix", "zenix.old")) return -1;
        flushfs();
    }
    if (ren("zenix.new", "zenix")) return -1;
    flushfs();
    return 0;
}

/* "Zenix (previous)": the bundle out of the way first (so a cut never applies it again), then the old kernel the
 * default again; the rollback flag tells zup to put the root back too */
static u32 upd_flags;
static void previous(void)
{
    upd_flags |= UPD_F_ROLLBACK;
    if (exists(B_ZUP)) { ren(B_ZUP, B_REJ); flushfs(); }
    if (!exists("zenix.old")) {
        say("zenixboot: no previous version is kept (Zenix/zenix.old): starting the installed one\n", 0, 0, 0);
        return;
    }
    if (exists("zenix")) { if (ren("zenix", "zenix.rejected")) return; flushfs(); }
    if (ren("zenix.old", "zenix")) return;
    flushfs();
    st.phase = PH_REJECTED;
    state_write();
    say("zenixboot: the previous version is the default again (the rejected kernel: Zenix/zenix.rejected)\n", 0, 0, 0);
}
static void retire(const char *why)
{
    say("zenixboot: update: version %ld %s: the update is done\n", (long)st.version, (long)why, 0);
    if (exists(B_ZUP)) { ren(B_ZUP, B_DONE); flushfs(); }
    st.phase = PH_CONFIRMED;
    state_write();
}

/* the timed question: 10 s, the default is the previous version (asked only with evidence of a failure) */
static int ask_previous(const char *what)
{
    long in = Input();
    say("zenixboot: Zenix %ld %s - start the previous version? (Y in 10 s) ", (long)st.version, (long)what, 0);
    int yes = 1;
    if (in) {
        SetMode(in, 1);
        for (int s = 10; s > 0; s--) {
            u8 c;
            if (WaitForChar(in, 1000000) && Read(in, &c, 1) == 1) {
                if (c == 'n' || c == 'N') { yes = 0; break; }
                if (c == 'y' || c == 'Y' || c == '\r' || c == '\n') break;
            }
        }
        SetMode(in, 0);
    }
    say(yes ? "previous\n" : "no: the new one\n", 0, 0, 0);
    return yes;
}

/* the whole boot half (not with CHECK, RAM= or an explicit kernel; the files in Zenix/). Leaves bun/bunsize set when
 * the new kernel is to get the bundle. */
static void update_half(int prev_asked)
{
    long size;
    state_read();
    if (exists(B_PART)) {               /* a copy that was cut off: never used */
        say("zenixboot: update: Zenix/%s was not copied to the end: renamed %s\n", (long)B_PART, (long)B_BAD, 0);
        ren(B_PART, B_BAD);
        flushfs();
    }
    if (prev_asked) { previous(); return; }
    if (st.phase == PH_TRIAL) {         /* did the new version come up? Only evidence counts (section 8) */
        u32 *n = note(), reason = 0, last = klog_newest(&reason);
        if (n && n[1] == UPDNOTE_CONFIRMED && n[2] == st.version) {
            note_clear();
            retire("came up (its login prompt on console 1)");
        } else if (n && n[1] == UPDNOTE_ROOTFAILED && n[2] == st.version) {
            note_clear();                   /* the kernel works, the root half was not installed: still the trial */
            st.handovers = 0;               /* (a sign of trouble: the 3-start rule counts from here) */
            say("zenixboot: update: Zenix %ld came up, but its root half was not installed (see dmesg -p in Zenix):"
                " the bundle is kept and goes to Zenix again\n", (long)st.version, 0, 0);
        } else if (last && last > st.lastboot && (reason == KLOG_END_PANIC || reason == KLOG_END_RUNNING)) {
            if (n && n[1] == UPDNOTE_FAILED && n[2] == st.version)
                say("zenixboot: its root half failed (see dmesg -p in Zenix)\n", 0, 0, 0);
            note_clear();
            if (ask_previous(reason == KLOG_END_PANIC ? "crashed" : "did not come up (reset before its login)")) {
                previous();
                return;
            }
        } else if (st.handovers >= 3) {
            retire("started 3 times without a sign of trouble");
        } else {
            say("zenixboot: Zenix %ld was installed and not yet confirmed; if it misbehaves, choose Zenix (previous)\n",
                (long)st.version, 0, 0);
        }
    }
    long fh = Open(B_ZUP, MODE_OLDFILE);
    if (!fh) return;
    size = file_size(fh);
    static u8 hdr[ZUP_HDR];
    long n = size >= ZUP_HDR ? Read(fh, hdr, ZUP_HDR) : -1;
    Close(fh);
    if (n != ZUP_HDR || !memeq(hdr, "ZUP1", 4)) { bundle_bad("not a ZUP1 bundle"); return; }
    u32 v = be32(hdr + 12);
    if (st.version == v && st.phase == PH_CONFIRMED) {      /* offered again after it was done: kept as it is */
        say("zenixboot: update: version %ld is installed and confirmed already: Zenix/%s renamed %s\n", (long)v,
            (long)B_ZUP, (long)B_DONE);
        ren(B_ZUP, B_DONE);
        flushfs();
        return;
    }
    if (v < st.version && st.phase != PH_REJECTED && st.phase != PH_NONE) {     /* never an older kernel than installed */
        bundle_bad("it is older than the installed version");
        return;
    }
    const char *why = bundle_load(size);
    if (why == NOMEM) { say("zenixboot: update: %s: not installed this time\n", (long)why, 0, 0); return; }
    if (!why) why = bundle_check(bun, bunsize, !(st.version == v && st.phase == PH_TRIAL));
    if (why) { bundle_bad(why); return; }
    if (st.version == v && st.phase == PH_TRIAL) {          /* the kernel is in place: the root half again (harmless) */
        say("zenixboot: update: version %ld in trial: the bundle goes to Zenix again\n", (long)v, 0, 0);
        return;
    }
    if ((why = roots_check())) { bundle_bad(why); return; }
    if ((why = volume_check(bkernsize))) {
        say("zenixboot: update: version %ld not installed this time: %s\n", (long)v, (long)why, 0);
        bundle_free();
        return;
    }
    say("zenixboot: update: installing version %ld\n", (long)v, 0, 0);
    st.version = v; memcpy(st.ksha, bksha, 32); st.haveksha = 1; st.phase = PH_INSTALLING; st.handovers = 0;
    if (state_write() || kernel_steps()) {
        say("zenixboot: update: the kernel steps did not finish: the installed system starts\n", 0, 0, 0);
        bundle_free();
        return;
    }
    st.phase = PH_TRIAL;
    state_write();
}

/* the start rule: zenix; if it is missing or bad, a zenix.new with the state's SHA-256, then zenix.old */
static int kernel_ok(long fh, long ksize, u8 *hdr)
{
    if (ksize < 32 || Read(fh, hdr, 32) != 32 || Seek(fh, 0, OFFSET_BEGINNING) < 0) return 0;
    u32 image = be32(hdr + 8), mem = be32(hdr + 12), nrel = be32(hdr + 16), reloff = be32(hdr + 20), st0 = be32(hdr + 28);
    return hdr[4] == 'Z' && hdr[5] == 'E' && hdr[6] == 'X' && hdr[7] == 'E' && image <= (u32)ksize && image >= 32 &&
           mem >= image && mem <= 0x1000000 && reloff >= image && reloff <= (u32)ksize &&
           nrel <= ((u32)ksize - reloff) / 4 && st0 < image;
}

int zb_main(void)
{
    static const char tmpl[] = "KERNEL,RAM/K,CHECK/S,PREVIOUS/S";
    long args[4] = { 0, 0, 0, 0 };
    long rda, kfh = 0, rfh = 0, ksize = 0, rsize = 0;
    u8 *table = 0, *buf = 0, hdr[32];
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
    int check = args[2] != 0, prev = args[3] != 0;

    /* the klog area first, before anything else can be allocated there (as the bootblock); if AmigaOS already uses
     * part of it, the zones below keep our buffers out of it */
    u32 chiptop = EXEC_L(62) & ~0xFFFu; /* MaxLocMem, rounded down to 4 KB as the kernel does (kernel/serial.c) */
    klog = AllocAbs(KLOG_AREA_SIZE, chiptop - KLOG_AREA_SIZE);
    zone_lo[0] = 0; zone_hi[0] = HANDOFF_STACK;
    zone_lo[1] = chiptop - KLOG_AREA_SIZE; zone_hi[1] = chiptop;
    iobuf = AllocMem(4096, 0);

    /* the update layout: Zenix/ next to zenixboot, with the kernel in it (else "zenix" next to zenixboot, as before) */
    if (!args[0]) {
        long old = CurrentDir(GetProgramDir());
        zdir = Lock("Zenix", SHARED_LOCK);
        CurrentDir(old);
        if (zdir) {
            olddir = CurrentDir(zdir);
            if (!exists("zenix") && !exists("zenix.new") && !exists("zenix.old")) {
                CurrentDir(olddir); olddir = 0; UnLock(zdir); zdir = 0;
            }
        }
    }
    if (zdir && !check && !rname) update_half(prev);
    else if (zdir && (check || rname) && exists(B_ZUP))
        say("zenixboot: an update is waiting (Zenix/%s): %s\n", (long)B_ZUP,
            (long)(check ? "CHECK writes nothing" : "ignored with RAM= (the root half needs the disk's root)"), 0);
    else if (prev)
        say("zenixboot: PREVIOUS: no Zenix/ directory with a kernel: nothing to go back to\n", 0, 0, 0);

    /* the kernel: a ZEXE image; its header says how much memory it needs at its home. With Zenix/: the start rule */
    static const char *const order[3] = { "zenix", "zenix.new", "zenix.old" };
    for (int i = 0; i < (zdir ? 3 : 1); i++) {
        if (zdir) kname = order[i];
        if (zdir && i == 1 && !(st.haveksha && file_has(kname, st.ksha))) continue;
        kfh = open_file(kname, !args[0]);
        if (!kfh) { if (!zdir) { rc = fail_dos(kname); goto out; } continue; }
        ksize = file_size(kfh);
        if (kernel_ok(kfh, ksize, hdr)) break;
        say("zenixboot: %s is not a Zenix kernel (build/kernel.bin, a ZEXE image)\n", (long)kname, 0, 0);
        Close(kfh); kfh = 0;
        if (!zdir) goto out;
    }
    if (!kfh) { say("zenixboot: no usable kernel in Zenix/ (zenix, zenix.new, zenix.old)\n", 0, 0, 0); goto out; }
    if (zdir && kname != order[0]) {
        say("zenixboot: starting Zenix/%s (Zenix/zenix is missing or not a kernel)\n", (long)kname, 0, 0);
        bundle_free();                  /* the bundle's root half needs its own kernel */
    }
    u32 image = be32(hdr + 8), mem = be32(hdr + 12), st0 = be32(hdr + 28);
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

    table = alloc_safe(HANDOFF_TABLE, 0);
    buf = table ? alloc_safe(total, 0) : 0;
    if (!buf) {
        say("zenixboot: not enough free memory: %ld bytes for the kernel%s, away from where the kernel will run "
            "(0x%lx)\n", (long)total, (long)(rname ? " and the RAM disk" : ""), (long)home);
        goto out;
    }
    if (bun && overlaps(bunraw, bunalloc)) {    /* loaded before the kernel's home was known: move it out of the way */
        u8 *nraw = alloc_safe(bunalloc, 1);
        if (!nraw) { say("zenixboot: update: no memory for the bundle away from the kernel: not handed over\n", 0, 0, 0); bundle_free(); }
        else {
            u8 *nb = (u8 *)(((u32)nraw + 16 + 15) & ~15u);
            memcpy(nb, bun, bunsize);
            FreeMem(bunraw, bunalloc);
            bunraw = nraw; bun = nb;
        }
    }

    say("zenixboot " ZB_VERSION ": loading %s%s (%ld bytes)", (long)(zdir ? "Zenix/" : ""), (long)kname, ksize);
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
    if (zdir && !check && !rname) {     /* bootinfo +0x78: the bundle for zup, and the flags (D15) */
        *(u32 *)(table + BI_UPD) = (u32)bun;
        *(u32 *)(table + BI_UPD + 4) = bun ? bunsize : 0;
        *(u32 *)(table + BI_UPD + 8) = upd_flags;
    }

    colour(DIAG_CYAN);                   /* stage 2: stage0 and the memory list, AmigaOS still alive */
    if (check)                          /* CHECK goes back to AmigaOS: stage0 must not stop its Ethernet (the GENET) */
        *(u32 *)(table + 0x100) = HWINFO_DRYRUN;
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
    if (zdir && st.phase == PH_TRIAL && kname == order[0]) {   /* one more handover of the trial, and its boot number */
        u32 reason;
        st.handovers++;
        st.lastboot = klog_newest(&reason);
        display_back();
        state_write();
    }
    if (bun) say("zenixboot: update: the bundle (%ld bytes at 0x%lx) goes to Zenix for its root half\n", (long)bunsize, (long)bun, 0);
    if (wrote) flushfs();               /* everything on the disk before AmigaOS goes */

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
    bundle_free();
    if (wrote) flushfs();
    if (olddir || zdir) { if (olddir) CurrentDir(olddir); UnLock(zdir); }
    if (iobuf) FreeMem(iobuf, 4096);
    if (klog) FreeMem(klog, KLOG_AREA_SIZE);
    FreeArgs(rda);
    CloseLibrary(GfxBase);
    CloseLibrary(DOSBase);
    return rc;
}
