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
#include "../crypto/zc/zc.h"            /* zc_sig_verify, the .sig format (DESIGN D21 6.1-6.2) */
#include "zpkgkeys.h"                   /* the build's trusted public keys (tools/zsign.py cheader, D21 6.3): the TEST
                                           key in a test build, tools/keys/zenix-release-*.pub with RELEASE=1 */

#define ZB_VERSION "0.3b1"
#define ZB_DATE "28.10.2026"
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
#define BI_KOPTS        0x84            /* bootinfo: KOPT_* (D37) */
#define KOPT_DCACHE_OFF 1u
#define KOPT_DCACHE_WT  2u
#define KOPT_MMU_ON     4u              /* DESIGN D41: memory protection on */
#define KOPT_MMU_OFF    8u
#define KOPT_MMU_STRICT 32u             /* D60 12: old servers get no lend */
#define KOPT_MMU_D41    64u             /* D60 12: D41's system processes (the way back) */
#define HWINFO_DRYRUN   0x5A485744u     /* kernel/hwinfo.h: "ZHWD" in the hwinfo magic, stage0 writes no hardware */
#define ZUP_HDR 96
#define ZUP_TOCE 80
#define ZUP_MAX (16u << 20)

long zb_os(void *base, long lvo, long d0, long d1, long d2, long d3, long a0, long a1);
long zb_dopkt(void *dosbase, void *port, long action);
u32 zb_tables(void *table, void *buffer, u32 ksize);
void zb_serial(const char *fmt, long *args);
void zb_go(void *table, void *buffer, u32 ksize, u32 home) __attribute__((noreturn));
long zb_onstack(long (*fn)(void *), void *arg, void *lower, u32 size);
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
#define DateStamp(d)        zb_os(DOSBase, -192, 0, (long)(d), 0, 0, 0, 0)
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

/* intuition.library and Picasso96API.library (DESIGN D43: RTG; offsets from the NDK's and Picasso96's interface
 * files, names and numbers only) */
static void *IntuitionBase, *P96Base;
#define OpenWindowTagList(nw, t) ((u8 *)zb_os(IntuitionBase, -606, 0, 0, 0, 0, (long)(nw), (long)(t)))
#define SetPointer(w, p, h, wd)  zb_os(IntuitionBase, -270, (long)(h), (long)(wd), 0, 0, (long)(w), (long)(p))
#define p96GetBitMapAttr(b, a)   ((u32)zb_os(P96Base, -42, (long)(a), 0, 0, 0, (long)(b), 0))
#define p96LockBitMap(b, ri, n)  zb_os(P96Base, -48, (long)(n), 0, 0, 0, (long)(b), (long)(ri))
#define P96BMA_WIDTH 0
#define P96BMA_HEIGHT 1
#define P96BMA_BYTESPERROW 4
#define P96BMA_BYTESPERPIXEL 5
#define P96BMA_RGBFORMAT 7
#define P96BMA_ISP96 8
#define WA_Dummy (0x80000000u + 99)

/* the stage colours of boot/handoff.S and kernel/diag.h */
#define DIAG_BLUE 0x00F
#define DIAG_CYAN 0x0FF

#define EXEC_W(off) (*(volatile u16 *)((u8 *)SysBase + (off)))
#define EXEC_L(off) (*(volatile u32 *)((u8 *)SysBase + (off)))
#define CUSTOM(off) (*(volatile u16 *)(0xDFF000 + (off)))

/* the compiler may call these (struct copies), and sha256.c does */
void *memcpy(void *d, const void *s, unsigned long n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }
int memcmp(const void *a, const void *b, unsigned long n)
{
    const u8 *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p < *q ? -1 : 1;
    return 0;
}
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

/* ---- memory (boot/bootmem.h) ---- */
#include "bootmem.h"

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
/* the bundle's detached signature (DESIGN D21 6.2): its life follows the bundle's (renamed with it, after it) */
static const char S_ZUP[] = "zenix-update.zup.sig", S_PART[] = "zenix-update.zup.sig.part", S_BAD[] = "zenix-update.bad.sig",
                  S_DONE[] = "zenix-update.done.sig", S_REJ[] = "zenix-update.rejected.sig";
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
static struct { u32 version, handovers, lastboot; u8 ksha[32], sigd[32]; int haveksha, havesigd, phase; } st;

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
static int hex32(const char *v, u8 out[32])     /* 64 hex digits first: 1, else 0 */
{
    for (int i = 0; i < 32; i++) {
        int a = hexv(v[2 * i]), b = a < 0 ? -1 : hexv(v[2 * i + 1]);
        if (b < 0) return 0;
        out[i] = (u8)(a << 4 | b);
    }
    return 1;
}
static const char hx[] = "0123456789abcdef";
static char *put_hex(char *p, const u8 *b, u32 n) { while (n--) { *p++ = hx[*b >> 4]; *p++ = hx[*b++ & 15]; } return p; }

static int state_parse(char *t, long n)
{
    if (n < 18 || !memeq(t, "zenixboot-state 1\n", 18)) return -1;
    t[n] = 0;
    int end = 0;
    st.version = st.handovers = st.lastboot = 0; st.haveksha = st.havesigd = 0; st.phase = PH_NONE;
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
        else if (streq(l, "kernelsha")) st.haveksha = hex32(v, st.ksha);
        else if (streq(l, "sigdigest")) st.havesigd = hex32(v, st.sigd) && !v[64];   /* D21 6.4: the digest verified */
        else if (streq(l, "end")) end = 1;
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
    char *p = put_s(buf, "zenixboot-state 1\nversion ");
    p = put_u(p, st.version);
    if (st.haveksha) { p = put_s(p, "\nkernelsha "); p = put_hex(p, st.ksha, 32); }
    if (st.havesigd) { p = put_s(p, "\nsigdigest "); p = put_hex(p, st.sigd, 32); }
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
static u8 bdig[32];                     /* the digest computed over header bytes 0-63 and the table (D21 6.1) */

/* every count, size and offset checked (untrusted: anyone who can write BOOT: wrote it), the table's digest computed
 * into bdig and compared with header bytes 64-95; with full, the SHA-256 of every part too (D21 6.4's step 4: only
 * after the signature, bundle_parts). 0, or a reason */
static const char *bundle_check(const u8 *p, u32 len, int full)
{
    if (len < ZUP_HDR || !memeq(p, "ZUP1", 4) || be32(p + 4) != 1 || be32(p + 8) != ZUP_HDR) return "not a ZUP1 bundle";
    u32 ntoc = be32(p + 52);
    if (be32(p + 56) != len) return "its size is not the one its header says (cut short?)";
    if (be32(p + 48) != ZUP_HDR || ntoc < 1 || ntoc > 64 || ntoc * ZUP_TOCE > len - ZUP_HDR) return "a bad table of contents";
    u8 h[32];
    struct sha256 c;
    sha256_init(&c); sha256_update(&c, p, 64); sha256_update(&c, p + ZUP_HDR, ntoc * ZUP_TOCE); sha256_final(&c, bdig);
    if (!memeq(bdig, p + 64, 32)) return "the table of contents does not match its SHA-256";
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

/* ---- the signature (DESIGN D21 6.4): Zenix/zenix-update.zup.sig over the computed digest bdig ---- */
#define KEY_RELEASE (ZPKG_KEYCLASS == 2)  /* a RELEASE=1 build: the release keys; else the TEST key (D21 6.3) */
#define SIG_STACK 6144                  /* its own stack (zb_onstack): the measured chain, docs/man/zenixboot.txt */
#define SIG_GUARD 512                   /* its lowest bytes: touched means too close (refused, never trusted) */
static int unsigned_ok;                 /* UNSIGNED_OK given to a test build: a missing .sig is accepted */
static int sig_reused;                  /* sig_check found the trial's verified digest (sigdigest): nothing checked */
static const char NOMEM[] = "not enough memory for it";
struct sigrun { const u8 *sig; int which; };
static long sig_run(void *a)
{
    struct sigrun *r = a;
    return zc_sig_verify(r->sig, ZC_SIG_SIZE, ZC_SIG_LABEL_BUNDLE, bdig, zpkg_keys, ZPKG_NKEYS, &r->which);
}
/* after bundle_check. 0 (verified, or verified at the trial's first start: sigdigest), NOMEM, or a reason */
static const char *sig_check(int trial)
{
    static char why[96];
    static u8 sig[ZC_SIG_SIZE + 1];
    const u8 *ch = bun + 40;            /* the header's channel (8 bytes, NUL-padded): the key class's (D21 6.3) */
    int rel = memeq(ch, "stable", 7) || memeq(ch, "testing", 8), tst = memeq(ch, "test", 5);
    sig_reused = 0;
    if (KEY_RELEASE ? !rel : !tst)
        return KEY_RELEASE ? "its channel is not a release channel (stable, testing): a test build's bundle"
                           : "its channel is not \"test\": a release bundle (this zenixboot is a test build)";
    if (trial && st.havesigd && memeq(st.sigd, bdig, 32)) {
        sig_reused = 1;
        say("zenixboot: update: its signature was verified at the trial's first start (sigdigest)\n", 0, 0, 0);
        return 0;
    }
    long fh = Open(S_ZUP, MODE_OLDFILE), n;
    if (!fh) {
        if (unsigned_ok) {
            st.havesigd = 0;
            say("zenixboot: update: no signature, accepted: UNSIGNED_OK (a test build)\n", 0, 0, 0);
            return 0;
        }
        return "it has no signature (Zenix/zenix-update.zup.sig)";
    }
    n = Read(fh, sig, sizeof sig);
    Close(fh);
    if (n != ZC_SIG_SIZE) return "its signature (Zenix/zenix-update.zup.sig) is not 80 bytes";
    u8 *stk = AllocMem(SIG_STACK, 0);
    if (!stk) return NOMEM;
    memset(stk, 0xA5, SIG_STACK);       /* the guard: its lowest SIG_GUARD bytes must stay untouched */
    long t0[3], t1[3];
    say("zenixboot: update: checking its signature\n", 0, 0, 0);
    DateStamp(t0);
    struct sigrun r = { sig, -1 };
    long rc = zb_onstack(sig_run, &r, stk, SIG_STACK);
    DateStamp(t1);
    u32 low = 16;                       /* (StackSwap puts a mark of its own at the stack's lowest bytes) */
    while (low < SIG_STACK && stk[low] == 0xA5) low++;
    FreeMem(stk, SIG_STACK);
    say("zenixboot: update: the check used %ld of its %ld bytes of stack\n", (long)(SIG_STACK - low), SIG_STACK, 0);
    if (low < SIG_GUARD) return "its check overran its stack (zenixboot's SIG_STACK is too small): not trusted";
    char *p = put_s(why, KEY_RELEASE ? "release key " : "test key ");
    p = put_hex(p, sig + 8, 8);
    *p = 0;
    if (rc == ZC_SIG_OK) {
        long ticks = (t1[0] - t0[0]) * 4320000 + (t1[1] - t0[1]) * 3000 + (t1[2] - t0[2]);   /* 50 a second */
        if (ticks < 0) ticks = 0;
        say("zenixboot: update: signed by %s (checked in %ld.%02ld s)\n", (long)why, ticks / 50, ticks % 50 * 2);
        memcpy(st.sigd, bdig, 32);
        st.havesigd = 1;
        return 0;
    }
    if (rc == ZC_SIG_EKEY) {
        p = put_s(why, "it is signed by key ");
        p = put_hex(p, sig + 8, 8);
        p = put_s(p, KEY_RELEASE ? ", not a release key" : ", not this build's test key");
        *p = 0;
        return why;
    }
    return rc == ZC_SIG_EBAD ? "its signature does not verify (damaged, or made for another bundle)"
                             : "its signature file is not a Zenix signature (ZSG1, Ed25519)";
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
/* the bundle renamed, then its .sig (D21 6.2: zenix-update.bad.sig and so on; a cut between the two leaves a .sig
 * without a bundle, which nothing uses), flushed */
static void ren_bundle(const char *to, const char *tosig)
{
    if (exists(B_ZUP)) ren(B_ZUP, to);
    if (exists(S_ZUP)) ren(S_ZUP, tosig);
    flushfs();
}
/* a damaged or unusable bundle: renamed .bad (never re-hashed at every start), the reason said */
static void bundle_bad(const char *why)
{
    say("zenixboot: update: Zenix/%s is not used: %s; renamed %s\n", (long)B_ZUP, (long)why, (long)B_BAD);
    bundle_free();
    ren_bundle(B_BAD, S_BAD);
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
    if (exists(B_ZUP) || exists(S_ZUP)) ren_bundle(B_REJ, S_REJ);
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
    if (exists(B_ZUP) || exists(S_ZUP)) ren_bundle(B_DONE, S_DONE);
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
    if (exists(S_PART)) {               /* a copy of a .sig that was cut off (D21 6.2: renamed before the bundle) */
        say("zenixboot: update: Zenix/%s was not copied to the end: deleted\n", (long)S_PART, 0, 0);
        del(S_PART);
        flushfs();
    }
    if (exists(B_PART)) {               /* a copy that was cut off: never used */
        say("zenixboot: update: Zenix/%s was not copied to the end: renamed %s\n", (long)B_PART, (long)B_BAD, 0);
        ren(B_PART, B_BAD);
        if (!exists(B_ZUP) && exists(S_ZUP)) ren(S_ZUP, S_BAD);     /* its .sig, copied before it */
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
    if (!fh) {
        if (exists(S_ZUP) && !exists(B_ZUP)) {  /* left by a cut between two renames: never one for a later bundle */
            say("zenixboot: update: Zenix/%s has no bundle: renamed %s\n", (long)S_ZUP, (long)S_BAD, 0);
            ren(S_ZUP, S_BAD);
            flushfs();
        }
        return;
    }
    size = file_size(fh);
    static u8 hdr[ZUP_HDR];
    long n = size >= ZUP_HDR ? Read(fh, hdr, ZUP_HDR) : -1;
    Close(fh);
    if (n != ZUP_HDR || !memeq(hdr, "ZUP1", 4)) { bundle_bad("not a ZUP1 bundle"); return; }
    u32 v = be32(hdr + 12);
    if (st.version == v && st.phase == PH_CONFIRMED) {      /* offered again after it was done: kept as it is */
        say("zenixboot: update: version %ld is installed and confirmed already: Zenix/%s renamed %s\n", (long)v,
            (long)B_ZUP, (long)B_DONE);
        ren_bundle(B_DONE, S_DONE);
        return;
    }
    if (v < st.version && st.phase != PH_REJECTED && st.phase != PH_NONE) {     /* never an older kernel than installed */
        bundle_bad("it is older than the installed version");
        return;
    }
    /* D21 6.4's order: the sizes and the table, the digest computed and compared (bundle_check); the signature over
     * that digest (sig_check); then the parts' SHA-256 (bundle_check full). In the trial (the kernel installed) the
     * same digest as the one verified at its first start skips both: nothing is re-verified at every start */
    int trial = st.version == v && st.phase == PH_TRIAL;
    const char *why = bundle_load(size);
    if (!why) why = bundle_check(bun, bunsize, 0);
    if (!why && bunv != v) why = "it changed while it was read";   /* the version decided on is the one checked */
    /* before the signature (it only refuses or waits): a volume that cannot take the kernel now costs no 6-9 s check
     * at every start, and a kernel that cannot serve this root is refused at once */
    if (!why && !trial) why = roots_check();
    if (!why && !trial && (why = volume_check(bkernsize))) {
        say("zenixboot: update: version %ld not installed this time: %s\n", (long)v, (long)why, 0);
        bundle_free();
        return;
    }
    if (!why) why = sig_check(trial);
    if (why == NOMEM) {
        say("zenixboot: update: %s: not %s this time\n", (long)why, (long)(trial ? "handed over" : "installed"), 0);
        bundle_free();
        return;
    }
    if (!why && !sig_reused) why = bundle_check(bun, bunsize, 1);
    if (why) { bundle_bad(why); return; }
    if (trial) {                        /* the kernel is in place: the root half again (harmless) */
        say("zenixboot: update: version %ld in trial: the bundle goes to Zenix again\n", (long)v, 0, 0);
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

/* ---- DESIGN D43 (docs/d43-rtg.md 3): the front screen's RTG picture handed to Zenix ---- */
struct hwfb_rec {                       /* kernel/hwinfo.h struct hwfb, at hwinfo + 408 */
    u32 magic, base;
    u16 width, height;
    u32 bpr;
    u8 format, bpp;
    u16 flags;
    u32 hvs, dl_rtg, dl_chip;
};
#define HWFB_MAGIC 0x5A464231u
#define HWFB_HVS   1
static u32 le32r(u32 a) { return __builtin_bswap32(*(volatile u32 *)a); }      /* the Pi's registers: little-endian */
/* Picasso96's RGBFormat -> its bytes a pixel, 0 for one the display server cannot draw (CLUT, planar, YUV) */
static int rtg_bpp(u32 f) { return f == 2 || f == 3 ? 3 : f >= 6 && f <= 9 ? 4 : f == 4 || f == 5 || (f >= 10 && f <= 13) ? 2 : 0; }
/* the front screen, if an RTG one, into the record (take: also locked, its pointer blanked, the Pi's display list
 * register named); said either way. -> the record, or 0 (the desktop stays on the chipset) */
static void rtg_close(void)             /* back to AmigaOS (CHECK, a refusal): the libraries closed */
{
    if (P96Base) CloseLibrary(P96Base);
    if (IntuitionBase) CloseLibrary(IntuitionBase);
    P96Base = IntuitionBase = 0;
}
static void *rtg_bm;                    /* the screen's bitmap rtg_take found, for rtg_lock */
/* the picture's memory, locked after the window, the pointer and the Pi's switch (Picasso96 switches no screen while
 * a bitmap is locked); never unlocked: AmigaOS ends here. -> 0, or -1 (the desktop stays on the chipset) */
static int rtg_lock(struct hwfb_rec *r)
{
    struct { u32 mem; u16 bpr, pad; u32 fmt; } ri = { 0, 0, 0, 0 };
    if (!p96LockBitMap(rtg_bm, &ri, sizeof ri) || !ri.mem) {
        say("zenixboot: RTG: its picture memory could not be locked: the desktop stays on the chipset\n", 0, 0, 0);
        r->magic = 0;
        return -1;
    }
    r->base = ri.mem;
    return 0;
}
static struct hwfb_rec *rtg_take(struct hwfb_rec *r, const u8 *hw, int take)
{
    static const char *const no = "zenixboot: RTG: %s: the desktop stays on the chipset\n";
    IntuitionBase = OpenLibrary("intuition.library", 37);
    P96Base = OpenLibrary("Picasso96API.library", 2);
    if (!IntuitionBase || !P96Base) {
        say(no, (long)"no Picasso96 (Picasso96API.library)", 0, 0);
        rtg_close();
        return 0;
    }
    Forbid();
    u8 *scr = *(u8 **)((u8 *)IntuitionBase + 60);       /* IntuitionBase->FirstScreen: the front one */
    void *bm = scr ? *(void **)(scr + 84 + 4) : 0;      /* its RastPort's BitMap */
    zb_os(SysBase, -138, 0, 0, 0, 0, 0, 0);             /* Permit */
    if (!bm || !p96GetBitMapAttr(bm, P96BMA_ISP96)) {
        say(no, (long)"the front screen is not an RTG screen", 0, 0);
        rtg_close();
        return 0;
    }
    u32 w = p96GetBitMapAttr(bm, P96BMA_WIDTH), h = p96GetBitMapAttr(bm, P96BMA_HEIGHT),
        bpr = p96GetBitMapAttr(bm, P96BMA_BYTESPERROW), bpp = p96GetBitMapAttr(bm, P96BMA_BYTESPERPIXEL),
        f = p96GetBitMapAttr(bm, P96BMA_RGBFORMAT);
    if (!rtg_bpp(f) || rtg_bpp(f) != (int)bpp || w < 320 || h < 200 || w > 4096 || h > 4096 || bpr < w * bpp) {
        say("zenixboot: RTG: the front screen is %ldx%ld in Picasso96's format %ld", (long)w, (long)h, (long)f);
        say(no + 16, (long)(f == 1 ? ", a palette (CLUT) screen: its palette needs the card's driver; choose a 16- or "
                         "32-bit mode" : ", not a hi- or true-colour format the desktop draws"), 0, 0);
        rtg_close();
        return 0;
    }
    const u32 *hi = (const u32 *)hw;    /* the hardware table stage0 filled: flags at +4, soc_base at +8 */
    say("zenixboot: RTG: the front screen, %ldx%ld, format %ld", (long)w, (long)h, (long)f);
    say(", %ld bytes a pixel, %ld a row%s\n", (long)bpp, (long)bpr,
        (long)((hi[1] & 1) && hi[2] ? ", the Pi's HDMI switched with the consoles" : ""));
    if (!take) {
        rtg_close();
        return 0;
    }
    /* a backdrop window, active, with an empty pointer: the card's own pointer (the Pi's HVS sprite) empty */
    u16 *ptr = (u16 *)AllocMem(16, MEMF_CHIP | MEMF_CLEAR);
    u32 tags[] = { WA_Dummy + 0x0D, (u32)scr, WA_Dummy + 0x01, 0, WA_Dummy + 0x02, 0, WA_Dummy + 0x03, w,
                   WA_Dummy + 0x04, h, WA_Dummy + 0x22, 1, WA_Dummy + 0x25, 1, WA_Dummy + 0x26, 1,
                   WA_Dummy + 0x29, 1, WA_Dummy + 0x24, 1, 0 };     /* CustomScreen, Left, Top, Width, Height,
                                                                     * Backdrop, Borderless, Activate, SimpleRefresh,
                                                                     * NoCareRefresh */
    u8 *win = ptr ? OpenWindowTagList(0, tags) : 0;
    if (win) {
        SetPointer(win, ptr, 1, 16);
        WaitTOF();
        WaitTOF();
    }
    memset(r, 0, sizeof *r);
    rtg_bm = bm;                        /* locked last (rtg_lock): a lock stops Picasso96's screen switching */
    r->width = (u16)w;
    r->height = (u16)h;
    r->bpr = bpr;
    r->format = (u8)f;
    r->bpp = (u8)bpp;
    if ((hi[1] & 1) && hi[2])           /* HW_DT (Emu68): SCALER_DISPLIST1, the Pi's bus 0x7E400024 */
        r->hvs = hi[2] + 0x400024;
    r->magic = HWFB_MAGIC;
    return r;
}

int zb_main(void)
{
    static const char tmpl[] = "KERNEL,RAM/K,CHECK/S,PREVIOUS/S,UNSIGNED_OK/S,DCACHE/K,MMU/K,RTG/S";
    long args[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
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
    u32 kopts = 0;                      /* DESIGN D37: bootinfo +0x84, the 040/060 data cache's mode */
    if (args[5]) {
        const char *d = (const char *)args[5];
        char w[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 3 && d[i]; i++)
            w[i] = (char)(d[i] >= 'a' && d[i] <= 'z' ? d[i] - 32 : d[i]);
        if (d[0] && d[1] && d[2] && d[3]) w[0] = 0;     /* longer than three letters: none of the words */
        if (streq(w, "OFF")) kopts = KOPT_DCACHE_OFF;
        else if (streq(w, "WT")) kopts = KOPT_DCACHE_WT;
        else if (!streq(w, "CB")) {
            say("zenixboot: DCACHE=%s: say OFF, WT (write-through) or CB (copyback, the default)\n", args[5], 0, 0);
            goto out;                   /* rc 20; nothing allocated yet */
        }
    }
    if (args[6]) {                      /* DESIGN D41: memory protection on or off; D60 12: STRICT, D41 */
        const char *d = (const char *)args[6];
        char w[8];
        int i = 0;
        for (; i < 7 && d[i]; i++)
            w[i] = (char)(d[i] >= 'a' && d[i] <= 'z' ? d[i] - 32 : d[i]);
        w[i] = 0;
        if (d[i]) w[0] = 0;             /* longer than STRICT: none of the words */
        if (streq(w, "ON")) kopts |= KOPT_MMU_ON;
        else if (streq(w, "OFF")) kopts |= KOPT_MMU_OFF;
        else if (streq(w, "STRICT")) kopts |= KOPT_MMU_STRICT;
        else if (streq(w, "D41")) kopts |= KOPT_MMU_D41;
        else {
            say("zenixboot: MMU=%s: say ON, OFF, STRICT or D41 (memory protection)\n", args[6], 0, 0);
            goto out;
        }
    }
    if (args[4]) {                      /* DESIGN D21 6.4: unsigned bundles only in test builds, and only when asked */
        if (KEY_RELEASE) say("zenixboot: UNSIGNED_OK is for test builds: ignored (unsigned bundles are refused)\n", 0, 0, 0);
        else unsigned_ok = 1;
    }

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
    *(u32 *)(table + BI_KOPTS) = kopts;  /* bootinfo +0x84: the boot options (D37) */
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
        if (args[7])
            rtg_take((struct hwfb_rec *)(table + 0x100 + 408), table + 0x100, 0);
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
    struct hwfb_rec *fb = args[7] ? rtg_take((struct hwfb_rec *)(table + 0x100 + 408), table + 0x100, 1) : 0;
    u32 dl = fb && fb->hvs ? le32r(fb->hvs) : 0;    /* the Pi's HDMI: the card's display list, shown now */
    if (!fb || fb->hvs) {               /* a card that is not the Pi's keeps its picture: nothing in Zenix can switch it */
        LoadView(0);
        WaitTOF();
        WaitTOF();
    }
    if (fb && fb->hvs) {                /* the chipset's list now (the C790's picture): the two Zenix switches between */
        fb->dl_rtg = dl;
        fb->dl_chip = le32r(fb->hvs);
        if (fb->dl_rtg != fb->dl_chip)
            fb->flags |= HWFB_HVS;
        else
            fb->hvs = fb->dl_rtg = fb->dl_chip = 0;    /* no other list was shown: the card's picture stays on */
    }
    if (fb)
        rtg_lock(fb);
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
