/* SPDX-License-Identifier: GPL-2.0-only */
/* bootmenu: a boot menu for an Amiga with several systems on one disk (docs/man/bootmenu.txt). It runs from the
 * S:Startup-Sequence of a small AmigaDOS boot partition, reads S:BootMenu.conf there, shows the entries in the Shell
 * window and starts the one chosen (or the default when the countdown runs out):
 *   - "partition NAME": boot that AmigaOS partition. The menu leaves a small helper in chip RAM (kt_blob in
 *     boot/bootmenu-start.S) that moves the partition to the head of Kickstart's boot list at the next reset, and
 *     reboots. Nothing is written to the disk; the next power-on or reset shows the menu again.
 *   - "run COMMAND": run an AmigaDOS command (zenixboot starts Zenix and does not come back).
 *   - "shell": leave the menu; the Startup-Sequence goes on (a rescue Shell).
 * Freestanding (no C library): every AmigaOS call goes through bm_os. Built like zenixboot (Makefile). */

typedef unsigned long u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define BM_VERSION "0.1b2"
#define BM_DATE "24.10.2026"
static const char version[] __attribute__((used, section(".text.start"))) = "$VER: bootmenu " BM_VERSION " (" BM_DATE ")";

long bm_os(void *base, long lvo, long d0, long d1, long d2, long d3, long a0, long a1);
void *SysBase;
static void *DOSBase;

extern u8 kt_blob[], kt_blob_end[], kt_memlist[], kt_tags[], kt_res[], kt_name[], kt_code[], kt_target[];
extern u8 kt_oldmem[], kt_oldtag[];

/* exec.library */
#define Forbid()            bm_os(SysBase, -132, 0, 0, 0, 0, 0, 0)
#define Permit()            bm_os(SysBase, -138, 0, 0, 0, 0, 0, 0)
#define AllocMem(n, f)      ((u8 *)bm_os(SysBase, -198, (long)(n), (long)(f), 0, 0, 0, 0))
#define FreeMem(a, n)       bm_os(SysBase, -210, (long)(n), 0, 0, 0, 0, (long)(a))
#define OpenLibrary(n, v)   ((void *)bm_os(SysBase, -552, (long)(v), 0, 0, 0, 0, (long)(n)))
#define CloseLibrary(b)     bm_os(SysBase, -414, 0, 0, 0, 0, 0, (long)(b))
#define SumKickData()       ((u32)bm_os(SysBase, -612, 0, 0, 0, 0, 0, 0))
#define CacheClearU()       bm_os(SysBase, -636, 0, 0, 0, 0, 0, 0)
#define ColdReboot()        bm_os(SysBase, -726, 0, 0, 0, 0, 0, 0)
#define MEMF_CHIP  (1L << 1)
#define MEMF_CLEAR (1L << 16)
/* dos.library */
#define Open(n, m)          bm_os(DOSBase, -30, 0, (long)(n), (long)(m), 0, 0, 0)
#define Close(f)            bm_os(DOSBase, -36, 0, (long)(f), 0, 0, 0, 0)
#define Read(f, b, n)       bm_os(DOSBase, -42, 0, (long)(f), (long)(b), (long)(n), 0, 0)
#define Write(f, b, n)      bm_os(DOSBase, -48, 0, (long)(f), (long)(b), (long)(n), 0, 0)
#define Input()             bm_os(DOSBase, -54, 0, 0, 0, 0, 0, 0)
#define Output()            bm_os(DOSBase, -60, 0, 0, 0, 0, 0, 0)
#define WaitForChar(f, us)  bm_os(DOSBase, -204, 0, (long)(f), (long)(us), 0, 0, 0)
#define SetMode(f, m)       bm_os(DOSBase, -426, 0, (long)(f), (long)(m), 0, 0, 0)
#define SystemTagList(c, t) bm_os(DOSBase, -606, 0, (long)(c), (long)(t), 0, 0, 0)
#define MODE_OLDFILE 1005

#define EXEC_L(off) (*(volatile u32 *)((u8 *)SysBase + (off)))
#define KICKMEMPTR   0x222
#define KICKTAGPTR   0x226
#define KICKCHECKSUM 0x22A

#define MAXENTRY 9
#define LINEMAX 80

enum { A_PARTITION, A_RUN, A_SHELL };
struct entry { char label[LINEMAX]; int action; char arg[LINEMAX]; };
static struct entry entries[MAXENTRY];
static int nentries, timeout = 5, defchoice;
static long out;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void puts_(const char *s) { Write(out, s, slen(s)); }
static void putn(int n) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = '0' + n % 10; n /= 10; } while (n && i); puts_(b + i); }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int startswith(const char *s, const char *w)
{
    while (*w) if (lower(*s++) != *w++) return 0;
    return *s == ' ' || *s == '\t' || *s == 0;
}
static const char *skipsp(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static void copytrim(char *d, const char *s, int max)
{
    int n = 0;
    s = skipsp(s);
    while (*s && n < max - 1) d[n++] = *s++;
    while (n && (d[n - 1] == ' ' || d[n - 1] == '\t')) n--;
    d[n] = 0;
}
static int atoi_(const char *s) { int n = 0; s = skipsp(s); while (*s >= '0' && *s <= '9') n = n * 10 + *s++ - '0'; return n; }

/* One line of S:BootMenu.conf: "timeout N", "default N", or "LABEL = partition NAME | run COMMAND | shell".
 * Anything else (blank lines, "#" comments, unknown words) is ignored, so an older menu reads a newer file. */
static void conf_line(char *l)
{
    l = (char *)skipsp(l);
    if (*l == '#' || *l == 0) return;
    if (startswith(l, "timeout")) { timeout = atoi_(l + 7); return; }
    if (startswith(l, "default")) { defchoice = atoi_(l + 7) - 1; return; }
    char *eq = l;
    while (*eq && *eq != '=') eq++;
    if (*eq != '=' || nentries == MAXENTRY) return;
    *eq = 0;
    struct entry *e = &entries[nentries];
    copytrim(e->label, l, LINEMAX);
    const char *a = skipsp(eq + 1);
    if (startswith(a, "partition")) { e->action = A_PARTITION; copytrim(e->arg, a + 9, 32); }
    else if (startswith(a, "run")) { e->action = A_RUN; copytrim(e->arg, a + 3, LINEMAX); }
    else if (startswith(a, "shell")) { e->action = A_SHELL; e->arg[0] = 0; }
    else return;
    int n = slen(e->arg);
    if (e->action == A_PARTITION && n && e->arg[n - 1] == ':') e->arg[n - 1] = 0;
    if (e->label[0] && (e->action == A_SHELL || e->arg[0])) nentries++;
}

static int read_conf(void)
{
    static char buf[4096];
    long f = Open("S:BootMenu.conf", MODE_OLDFILE);
    if (!f) return 0;
    long n = Read(f, buf, sizeof buf - 1);
    Close(f);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *l = buf;
    for (long i = 0; i <= n; i++)
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == 0) { buf[i] = 0; conf_line(l); l = buf + i + 1; }
    return nentries;
}

/* ---- the screen: the Shell window, drawn with the console's own control sequences ---- */
static void draw(int sel, int left)
{
    static int drawn;
    /* the first time: clear the window and hide the cursor; later: go home and write over the same text, line by
     * line with "erase to the end of the line", so that a real display does not flash at every countdown second */
    puts_(drawn ? "\x9b" "H" : "\x9b" "0 p\x9b" "H\x9b" "J");
    drawn = 1;
    puts_("\x9b" "K\n  \x9b" "1mBoot menu\x9b" "0m\x9b" "K\n\x9b" "K\n");
    for (int i = 0; i < nentries; i++) {
        puts_(i == sel ? "  \x9b" "7m " : "   ");
        putn(i + 1);
        puts_("  ");
        puts_(entries[i].label);
        puts_(" \x9b" "0m\x9b" "K\n");
    }
    puts_("\x9b" "K\n  Up/Down or 1-");
    putn(nentries);
    puts_(", Return to start.");
    if (left >= 0) { puts_("  Starting in "); putn(left); puts_(" s."); }
    puts_("\x9b" "K");
}

/* A key: '1'..'9', 'U'p, 'D'own, '\r', or 0 when nothing came within us microseconds. */
static int key(long in, long us)
{
    u8 c;
    if (!WaitForChar(in, us)) return 0;
    if (Read(in, &c, 1) != 1) return 0;
    if (c == 0x1b) {                                    /* ESC [ A as well as CSI A */
        if (!WaitForChar(in, 50000) || Read(in, &c, 1) != 1 || c != '[') return -1;
        c = 0x9b;
    }
    if (c == 0x9b) {
        u8 d = 0;
        while (WaitForChar(in, 50000) && Read(in, &d, 1) == 1)
            if (d >= 0x40 && d <= 0x7e) break;              /* the end of the sequence */
        return d == 'A' ? 'U' : d == 'B' ? 'D' : -1;
    }
    if (c == '\n') c = '\r';
    return c;
}

static int choose(void)
{
    long in = Input();
    int sel = defchoice, left = timeout;
    SetMode(in, 1);                                     /* raw: keys arrive one by one, not echoed */
    draw(sel, left);
    for (;;) {
        int k = key(in, left >= 0 ? 1000000 : 0x7fffffff);
        if (k == 0) {                                   /* one more second without a key */
            if (left < 0) continue;
            if (--left < 0) break;
            draw(sel, left);
            continue;
        }
        left = -1;                                      /* any key stops the countdown */
        if (k == '\r') break;
        if (k == 'U' && sel > 0) sel--;
        else if (k == 'D' && sel < nentries - 1) sel++;
        else if (k >= '1' && k < '1' + nentries) { sel = k - '1'; draw(sel, left); break; }
        draw(sel, left);
    }
    SetMode(in, 0);
    puts_("\x9b" " p\n\n");                         /* the cursor back on */
    return sel;
}

/* Boot partition NAME at the next reset (kt_blob, boot/bootmenu-start.S), then reboot. Returns only on failure. */
static void boot_partition(const char *name)
{
    u32 n = kt_blob_end - kt_blob;
    u8 *p = AllocMem(n, MEMF_CHIP | MEMF_CLEAR);
    if (!p) { puts_("bootmenu: no chip memory for the boot helper\n"); return; }
    for (u32 i = 0; i < n; i++) p[i] = kt_blob[i];
#define AT(sym) (p + ((sym) - kt_blob))
#define L(sym, i) (((u32 *)AT(sym))[i])
    for (int i = 0; i < 31 && name[i]; i++) AT(kt_target)[i] = name[i];
    Forbid();
    u32 oldmem = EXEC_L(KICKMEMPTR), oldtag = EXEC_L(KICKTAGPTR);
    L(kt_memlist, 0) = oldmem;                          /* ln_Succ: the lists already there stay */
    *(u32 *)(AT(kt_memlist) + 16) = (u32)p;             /* me_Addr */
    *(u32 *)(AT(kt_memlist) + 20) = n;                  /* me_Length */
    L(kt_tags, 0) = (u32)AT(kt_res);
    L(kt_tags, 1) = oldtag ? (oldtag | 0x80000000u) : 0;
    L(kt_oldmem, 0) = oldmem;
    L(kt_oldtag, 0) = oldtag;
    *(u32 *)(AT(kt_res) + 2) = (u32)AT(kt_res);         /* rt_MatchTag */
    *(u32 *)(AT(kt_res) + 6) = (u32)(p + n);            /* rt_EndSkip */
    *(u32 *)(AT(kt_res) + 14) = (u32)AT(kt_name);       /* rt_Name */
    *(u32 *)(AT(kt_res) + 18) = (u32)AT(kt_name);       /* rt_IdString */
    *(u32 *)(AT(kt_res) + 22) = (u32)AT(kt_code);       /* rt_Init */
    EXEC_L(KICKMEMPTR) = (u32)AT(kt_memlist);
    EXEC_L(KICKTAGPTR) = (u32)AT(kt_tags);
    EXEC_L(KICKCHECKSUM) = SumKickData();
    CacheClearU();                                      /* the helper is code: out of the data cache, into memory */
    ColdReboot();
    Permit();
}

long bm_main(void)
{
    DOSBase = OpenLibrary("dos.library", 37);
    if (!DOSBase) return 20;
    out = Output();
    if (!read_conf()) {
        puts_("bootmenu: no entries in S:BootMenu.conf\n");
        CloseLibrary(DOSBase);
        return 5;
    }
    if (defchoice < 0 || defchoice >= nentries) defchoice = 0;
    for (;;) {
        struct entry *e = &entries[choose()];
        puts_("Starting ");
        puts_(e->label);
        puts_("...\n");
        if (e->action == A_SHELL) break;
        if (e->action == A_PARTITION) boot_partition(e->arg);
        else {
            u32 tags[1] = { 0 };                        /* TAG_DONE */
            long rc = SystemTagList(e->arg, tags);
            puts_("bootmenu: ");
            puts_(e->arg);
            puts_(" ended with code ");
            putn((int)rc);
            puts_("\n");
        }
        puts_("Press Return for the menu.\n");
        long in = Input();
        SetMode(in, 1);
        while (key(in, 0x7fffffff) != '\r') ;
        SetMode(in, 0);
    }
    CloseLibrary(DOSBase);
    return 0;
}
