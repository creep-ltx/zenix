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

#define BM_VERSION "0.1b9"
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
#define AllocAbs(n, a)      ((u8 *)bm_os(SysBase, -204, (long)(n), 0, 0, 0, 0, (long)(a)))
#define FreeMem(a, n)       bm_os(SysBase, -210, (long)(n), 0, 0, 0, 0, (long)(a))
#define OpenLibrary(n, v)   ((void *)bm_os(SysBase, -552, (long)(v), 0, 0, 0, 0, (long)(n)))
#define CloseLibrary(b)     bm_os(SysBase, -414, 0, 0, 0, 0, 0, (long)(b))
#define SumKickData()       ((u32)bm_os(SysBase, -612, 0, 0, 0, 0, 0, 0))
#define CacheClearU()       bm_os(SysBase, -636, 0, 0, 0, 0, 0, 0)
#define ColdReboot()        bm_os(SysBase, -726, 0, 0, 0, 0, 0, 0)
#define FindTask(n)         ((void *)bm_os(SysBase, -294, 0, 0, 0, 0, 0, (long)(n)))
#define SetTaskPri(t, p)    bm_os(SysBase, -300, (long)(p), 0, 0, 0, 0, (long)(t))
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
#define DateStamp(d)        bm_os(DOSBase, -192, 0, (long)(d), 0, 0, 0, 0)
#define GetProgramName(b, n) bm_os(DOSBase, -576, 0, (long)(b), (long)(n), 0, 0, 0)
#define MODE_OLDFILE 1005

#define EXEC_L(off) (*(volatile u32 *)((u8 *)SysBase + (off)))
#define KICKMEMPTR   0x222
#define KICKTAGPTR   0x226
#define KICKCHECKSUM 0x22A

/* The boot note: a one-shot choice left by Zenix's "reboot amigaos" or by "bootzenix" here, in the spare bytes of the
 * first header of Zenix's log area at the top of chip RAM (kernel/kernel.h: BOOTNOTE_*, KLOG_AREA_SIZE; the same
 * numbers). Read and written only after AllocAbs() succeeded on it, so memory AmigaOS is using is never touched. */
#define KLOG_AREA_SIZE   0x4200u
#define BOOTNOTE_OFF     0xF0u
#define BOOTNOTE_MAGIC   0x5A4E5854u
#define BOOTNOTE_AMIGAOS 1u
#define BOOTNOTE_ZENIX   2u

#define MAXENTRY 9
#define LINEMAX 80

enum { A_PARTITION, A_RUN, A_SHELL };
struct entry { char label[LINEMAX]; int action; char arg[LINEMAX]; };
static struct entry entries[MAXENTRY];
static int nentries, timeout = 5, defchoice, black;
static long out;

/* the compiler copies initialised local arrays (the tag lists) with these */
void *memcpy(void *d, const void *s, unsigned long n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }

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
    if (startswith(l, "background")) { black = lower(*skipsp(l + 10)) == 'b'; return; }     /* black or grey */
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

/* seconds since an earlier call, from the system clock (DateStamp: days, minutes, ticks of 1/50 s) */
static u32 now_ticks(void)
{
    long d[3];
    DateStamp(d);
    return ((u32)d[0] * 1440u + (u32)d[1]) * 3000u + (u32)d[2];
}

/* ---- the menu's own screen: no title bar, no mouse pointer, the text drawn here and the keys read directly ----
 * A screen like the Workbench's (SA_LikeWorkbench: the same mode and font), quiet (no title bar), with one borderless
 * backdrop window that takes the keys (IDCMP VANILLAKEY for 1-9 and Return, RAWKEY for the cursor keys) and has a
 * blank pointer. Values from the NDK headers (intuition/screens.h, intuition.h). If any of it fails, the console
 * window below is used. */
static void *IntuitionBase, *GfxBase;
static u8 *gui_scr, *gui_win, *gui_ptr;
#define TAG_DONE 0u
#define SA_Depth 0x80000025u
#define SA_ShowTitle 0x80000036u
#define SA_Quiet 0x80000038u
#define SA_LikeWorkbench 0x80000047u
#define WA_Left 0x80000064u
#define WA_Top 0x80000065u
#define WA_Width 0x80000066u
#define WA_Height 0x80000067u
#define WA_IDCMP 0x8000006Au
#define WA_Flags 0x8000006Bu
#define WA_CustomScreen 0x80000070u
#define WFLG_SIMPLE_REFRESH 0x40u
#define WFLG_BACKDROP 0x100u
#define WFLG_BORDERLESS 0x800u
#define WFLG_ACTIVATE 0x1000u
#define WFLG_RMBTRAP 0x10000u
#define WFLG_NOCAREREFRESH 0x20000u
#define IDCMP_RAWKEY 0x400u
#define IDCMP_VANILLAKEY 0x200000u
#define GUI_RP() ((void *)*(u32 *)(gui_win + 50))           /* Window.RPort */
#define GUI_PORT() ((void *)*(u32 *)(gui_win + 86))         /* Window.UserPort */

static void gui_close(void)
{
    if (gui_win) bm_os(IntuitionBase, -72, 0, 0, 0, 0, (long)gui_win, 0);       /* CloseWindow */
    if (gui_scr) bm_os(IntuitionBase, -66, 0, 0, 0, 0, (long)gui_scr, 0);       /* CloseScreen */
    if (gui_ptr) FreeMem(gui_ptr, 16);
    gui_win = gui_scr = gui_ptr = 0;
}

static int gui_open(void)
{
    if (!IntuitionBase) IntuitionBase = OpenLibrary("intuition.library", 39);
    if (!GfxBase) GfxBase = OpenLibrary("graphics.library", 39);
    if (!IntuitionBase || !GfxBase)
        return 0;
    u32 st[] = { SA_LikeWorkbench, 1, SA_Depth, 2, SA_Quiet, 1, SA_ShowTitle, 0, TAG_DONE };
    gui_scr = (u8 *)bm_os(IntuitionBase, -612, 0, 0, 0, 0, 0, (long)st);        /* OpenScreenTagList(NULL, tags) */
    if (!gui_scr)
        return 0;
    u32 wt[] = { WA_CustomScreen, (u32)gui_scr, WA_Left, 0, WA_Top, 0,
                 WA_Width, *(u16 *)(gui_scr + 12), WA_Height, *(u16 *)(gui_scr + 14),
                 WA_Flags, WFLG_BORDERLESS | WFLG_BACKDROP | WFLG_ACTIVATE | WFLG_RMBTRAP | WFLG_SIMPLE_REFRESH |
                           WFLG_NOCAREREFRESH,
                 WA_IDCMP, IDCMP_VANILLAKEY | IDCMP_RAWKEY, TAG_DONE };
    gui_win = (u8 *)bm_os(IntuitionBase, -606, 0, 0, 0, 0, 0, (long)wt);        /* OpenWindowTagList(NULL, tags) */
    gui_ptr = AllocMem(16, MEMF_CHIP | MEMF_CLEAR);                             /* a blank one-line sprite */
    if (!gui_win || !gui_ptr) {
        gui_close();
        return 0;
    }
    bm_os(IntuitionBase, -270, 1, 16, 0, 0, (long)gui_win, (long)gui_ptr);     /* SetPointer: invisible */
    if (black) {                                        /* colour 0 the background, 1 the text, 2 the text on the bar */
        void *vp = gui_scr + 44;                        /* Screen.ViewPort */
        bm_os(GfxBase, -288, 0, 0, 0, 0, (long)vp, 0);                         /* SetRGB4(vp, 0, 0, 0, 0) */
        bm_os(GfxBase, -288, 1, 0xC, 0xC, 0xC, (long)vp, 0);                   /* light grey */
        bm_os(GfxBase, -288, 2, 0, 0, 0, (long)vp, 0);                         /* black */
    }
    return 1;
}

/* one line of text at text row `row`, padded with spaces to the full width, so it covers what was there */
static void gui_line(int row, const char *s, int inverse)
{
    void *rp = GUI_RP();
    u32 h = *(u16 *)((u8 *)rp + 58), base = *(u16 *)((u8 *)rp + 62);          /* TxHeight, TxBaseline */
    u32 x = 2 * *(u16 *)((u8 *)rp + 60);                                       /* two characters in (TxWidth) */
    static char buf[84];
    int n = 0;
    while (*s && n < 78) buf[n++] = *s++;
    int end = inverse ? n : 78;
    while (n < end) buf[n++] = ' ';
    bm_os(GfxBase, -354, 1, 0, 0, 0, 0, (long)rp);                               /* SetDrMd(JAM2) */
    if (inverse) {                                       /* the padding outside the bar in the normal colours */
        bm_os(GfxBase, -342, 1, 0, 0, 0, 0, (long)rp);
        bm_os(GfxBase, -348, 0, 0, 0, 0, 0, (long)rp);
        bm_os(GfxBase, -240, 0, (long)(8 + row * h + base), 0, 0, 0, (long)rp);
        static const char sp[] = "                                                                                ";
        bm_os(GfxBase, -60, 80, 0, 0, 0, (long)sp, (long)rp);
    }
    bm_os(GfxBase, -342, inverse ? 2 : 1, 0, 0, 0, 0, (long)rp);                /* SetAPen */
    bm_os(GfxBase, -348, inverse ? 1 : 0, 0, 0, 0, 0, (long)rp);                /* SetBPen */
    bm_os(GfxBase, -240, (long)x, (long)(8 + row * h + base), 0, 0, 0, (long)rp);   /* Move(rp, x, y) */
    bm_os(GfxBase, -60, n, 0, 0, 0, (long)buf, (long)rp);                       /* Text(rp, buf, n) */
}

static void gui_draw(int sel, int left)
{
    static char t[100];
    int row = 1;
    gui_line(row++, "Boot menu", 0);
    gui_line(row++, "", 0);
    for (int i = 0; i < nentries; i++) {
        int n = 0;
        t[n++] = ' '; t[n++] = (char)('1' + i); t[n++] = ' '; t[n++] = ' ';
        for (const char *s = entries[i].label; *s && n < 70; ) t[n++] = *s++;
        t[n++] = ' '; t[n] = 0;
        gui_line(row++, t, i == sel);
    }
    gui_line(row++, "", 0);
    int n = 0;
    for (const char *s = "Up/Down or 1-"; *s; ) t[n++] = *s++;
    t[n++] = (char)('0' + nentries);
    for (const char *s = ", Return to start."; *s; ) t[n++] = *s++;
    if (left >= 0) {
        for (const char *s = "  Starting in "; *s; ) t[n++] = *s++;
        if (left >= 10) t[n++] = (char)('0' + left / 10);
        t[n++] = (char)('0' + left % 10);
        for (const char *s = " s."; *s; ) t[n++] = *s++;
    }
    t[n] = 0;
    gui_line(row, t, 0);
}

/* the next key from the window: '1'..'9', '\r', 'U', 'D', -1 for any other, 0 when there is none */
static int gui_key(void)
{
    u8 *m;
    while ((m = (u8 *)bm_os(SysBase, -372, 0, 0, 0, 0, (long)GUI_PORT(), 0))) {   /* GetMsg */
        u32 cls = *(u32 *)(m + 20);
        u32 code = *(u16 *)(m + 24);
        bm_os(SysBase, -378, 0, 0, 0, 0, 0, (long)m);                            /* ReplyMsg */
        if (cls == IDCMP_VANILLAKEY)
            return code == '\n' ? '\r' : (int)code;
        if (cls == IDCMP_RAWKEY && !(code & 0x80))
            return code == 0x4C ? 'U' : code == 0x4D ? 'D' : -1;
    }
    return 0;
}

/* The menu window: a borderless console window as big as the Workbench screen, so the menu fills the screen. 0 if it
 * cannot be opened (then the Shell's own window is used, as before). */
static long menu_con;
static long menu_in(void) { return menu_con ? menu_con : Input(); }
static void menu_open(void)
{
    if (gui_open())
        return;
    void *ib = OpenLibrary("intuition.library", 37);
    if (!ib)
        return;
    u8 *scr = (u8 *)bm_os(ib, -510, 0, 0, 0, 0, 0, 0);          /* LockPubScreen(NULL): the default screen */
    if (scr) {
        u32 w = *(u16 *)(scr + 12), h = *(u16 *)(scr + 14);
        bm_os(ib, -516, 0, 0, 0, 0, 0, (long)scr);              /* UnlockPubScreen(NULL, scr) */
        static char spec[100];
        int n = 0;
        for (const char *q = "CON:0/0/"; *q; ) spec[n++] = *q++;
        u32 v[2] = { w, h };
        for (int k = 0; k < 2; k++) {
            char b[8]; int i = 7; b[i] = 0;
            do { b[--i] = '0' + v[k] % 10; v[k] /= 10; } while (v[k] && i);
            while (b[i]) spec[n++] = b[i++];
            spec[n++] = '/';
        }
        const char *rest = "/NOBORDER/NOSIZE/NOCLOSE/NODEPTH/NODRAG/SIMPLE";    /* no title: no title bar */
        while (*rest) spec[n++] = *rest++;
        spec[n] = 0;
        menu_con = Open(spec, MODE_OLDFILE);
        if (menu_con)
            out = menu_con;
    }
    CloseLibrary(ib);
}
static void menu_close(void)
{
    gui_close();
    if (!menu_con)
        return;
    Close(menu_con);
    menu_con = 0;
    out = Output();
}

/* The menu runs at priority 21 while it is shown, above the disk tasks (their filesystems run at 10) and the input
 * handler (20): at boot those are busy mounting the other partitions and would otherwise leave it no time to draw or
 * answer a key for seconds (seen on the A1200: the countdown stood at 5 for 3 s, then jumped to 2). It only waits for
 * keys, so it takes nothing from them. The countdown follows the clock, not the number of waits. */
#define Delay(t) bm_os(DOSBase, -198, 0, (long)(t), 0, 0, 0, 0)
static int choose_gui(void)
{
    void *me = FindTask(0);
    long oldpri = SetTaskPri(me, 21);
    int sel = defchoice, left = timeout;
    u32 start = now_ticks();
    gui_draw(sel, left);
    for (;;) {
        int k = gui_key();
        if (k == 0) {
            Delay(2);                                   /* 40 ms between looks at the keys */
            if (left < 0) continue;
            int l = timeout - (int)((now_ticks() - start) / 50u);
            if (l < 0) break;
            if (l != left) { left = l; gui_draw(sel, left); }
            continue;
        }
        left = -1;                                      /* any key stops the countdown */
        if (k == '\r') break;
        if (k == 'U' && sel > 0) sel--;
        else if (k == 'D' && sel < nentries - 1) sel++;
        else if (k >= '1' && k < '1' + nentries) { sel = k - '1'; gui_draw(sel, left); break; }
        gui_draw(sel, left);
    }
    SetTaskPri(me, oldpri);
    return sel;
}

static int choose(void)
{
    if (gui_win)
        return choose_gui();
    long in = menu_in();
    void *me = FindTask(0);
    long oldpri = SetTaskPri(me, 21);
    int sel = defchoice, left = timeout;
    u32 start = now_ticks();
    SetMode(in, 1);                                     /* raw: keys arrive one by one, not echoed */
    draw(sel, left);
    for (;;) {
        int k = key(in, left >= 0 ? 200000 : 0x7fffffff);
        if (k == 0) {
            if (left < 0) continue;
            int l = timeout - (int)((now_ticks() - start) / 50u);
            if (l < 0) break;
            if (l != left) { left = l; draw(sel, left); }
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
    SetTaskPri(me, oldpri);
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

/* the klog area (the note lies inside it). The whole area is taken with AllocAbs(), never just the note's 16 bytes:
 * exec writes a free-memory header (8 bytes) at the start of whatever free remainder an allocation leaves, and a
 * remainder starting at the note's end was slot 0's first text bytes, which FreeMem() then left damaged. The area
 * ends at the (rounded) top of chip RAM, so taking all of it leaves no remainder inside it */
static u32 *klog_area(void)
{
    u32 top = EXEC_L(0x3E) & ~0xFFFu;                   /* MaxLocMem: the end of chip RAM, rounded as the kernel does */
    return (u32 *)(top - KLOG_AREA_SIZE);
}
#define NOTE(a) ((u32 *)((u8 *)(a) + BOOTNOTE_OFF))

/* take the note if there is one: its kind, or 0. FreeMem() writes a free-memory header (8 bytes: slot 0's magic and
 * size) at the area's start unless the free memory just below it ends there, so the two calls run under Forbid()
 * (audit3 B8): no other task can take the memory below meanwhile, and FreeMem() then merges the area back into the
 * free block it came from. A header is written at the area's start only when a free block already began there,
 * whose header was in those bytes before bootmenu ran. Restoring the 8 bytes after FreeMem() (the audit's proposal)
 * would overwrite that header and break AmigaOS's memory list, so it is not done. */
static u32 note_take(void)
{
    Forbid();
    u32 *a = (u32 *)AllocAbs(KLOG_AREA_SIZE, klog_area()), kind = 0;
    if (a) {                                            /* else in use by AmigaOS: no note can be there */
        u32 *n = NOTE(a);
        if (n[0] == BOOTNOTE_MAGIC && n[2] == ~(BOOTNOTE_MAGIC + n[1]) &&
            (n[1] == BOOTNOTE_AMIGAOS || n[1] == BOOTNOTE_ZENIX))
            kind = n[1];
        n[0] = n[1] = n[2] = n[3] = 0;
        FreeMem(a, KLOG_AREA_SIZE);
    }
    Permit();
    return kind;
}

/* started as "bootzenix" (or "bootamigaos"): leave the note and reboot */
static long note_reboot(u32 kind)
{
    u32 *a = (u32 *)AllocAbs(KLOG_AREA_SIZE, klog_area());
    if (!a) {
        puts_("bootmenu: the memory for the boot note is in use; reboot and choose in the menu\n");
        return 10;
    }
    u32 *n = NOTE(a);
    n[0] = BOOTNOTE_MAGIC;
    n[1] = kind;
    n[2] = ~(BOOTNOTE_MAGIC + kind);
    n[3] = 0;
    CacheClearU();
    ColdReboot();
    return 10;
}

/* the entry a note asks for: the first "partition" entry for AmigaOS, the first "run ...zenixboot" for Zenix */
static int note_entry(u32 kind)
{
    for (int i = 0; i < nentries; i++) {
        if (kind == BOOTNOTE_AMIGAOS && entries[i].action == A_PARTITION)
            return i;
        if (kind == BOOTNOTE_ZENIX && entries[i].action == A_RUN) {
            const char *a = entries[i].arg;
            for (; *a; a++)
                if (lower(a[0]) == 'z' && lower(a[1]) == 'e' && lower(a[2]) == 'n' && lower(a[3]) == 'i' &&
                    lower(a[4]) == 'x' && lower(a[5]) == 'b')
                    return i;
        }
    }
    return -1;
}

long bm_main(void)
{
    DOSBase = OpenLibrary("dos.library", 37);
    if (!DOSBase) return 20;
    out = Output();
    {
        char name[108];
        const char *me = name;
        if (GetProgramName(name, sizeof name)) {
            for (char *q = name; *q; q++)
                if (*q == ':' || *q == '/')
                    me = q + 1;
            int z = 1, a = 1;
            const char *zw = "bootzenix", *aw = "bootamigaos";
            for (int i = 0; i < 12; i++) {
                if (z && lower(me[i]) != zw[i]) z = 0;
                if (a && lower(me[i]) != aw[i]) a = 0;
                if (!me[i]) break;
            }
            if (z || a) {
                long rc = note_reboot(z ? BOOTNOTE_ZENIX : BOOTNOTE_AMIGAOS);
                CloseLibrary(DOSBase);
                return rc;
            }
        }
    }
    if (!read_conf()) {
        puts_("bootmenu: no entries in S:BootMenu.conf\n");
        CloseLibrary(DOSBase);
        return 5;
    }
    if (defchoice < 0 || defchoice >= nentries) defchoice = 0;
    menu_open();
    int asked = note_entry(note_take());            /* reboot amigaos / bootzenix: no menu this time */
    for (;;) {
        struct entry *e = &entries[asked >= 0 ? asked : choose()];
        asked = -1;
        puts_("Starting ");
        puts_(e->label);
        puts_("...\n");
        if (e->action != A_PARTITION)
            menu_close();                               /* the command or the rescue Shell gets the Shell's window */
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
        menu_open();
    }
    CloseLibrary(DOSBase);
    return 0;
}
