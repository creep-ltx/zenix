// SPDX-License-Identifier: GPL-2.0-only
/* zdb - the debugger (DESIGN D54, docs/d54-debugger.md section 18, docs/man/zdb.txt).
 *   zdb [-m MAP] PROG [ARG...]   start PROG stopped at its entry
 *   zdb [-m MAP] -p PID PROG     stop a running process of your own (PROG: its file, for its header and symbols)
 *   zdb [-m MAP] -c LOG PROG     the last crash of PROG in a kernel log (dmesg's text; - standard input)
 * Commands from standard input, one a line. Symbols from a map in nm -n's format (PROG.map, or -m). */
#include "libz.h"
typedef unsigned short u16;

static int pid = -1, started, zex2;
static u32 tsize, base, code, csize;
static struct sym { u32 off; char *name; } *syms;
static int nsyms;
static volatile int intr;
static int quiet;                       /* report: a step's line left out (the step over a breakpoint in c) */
static u32 lastwhy;
static struct rd in;
#define NB 16
static u32 bps[NB];
static u16 bpw[NB];                     /* the program's words under them (the kernel shows ILLEGAL there) */
static int nb;

static void die(const char *m) { oflush(); dprintf(2, "zdb: %s\n", m); _zexit(1); }

/* ---- the program file and its symbols ---- */
static void header(const char *path)
{
    u32 h[10];
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0 || sys_read(fd, h, sizeof h) < 12)
        die("cannot read the program's header");
    sys_close(fd);
    if (h[1] == 0x5a455832) { zex2 = 1; tsize = h[2]; }
    else if (h[1] != 0x5a455845) die("not a Zenix program");
}

static int symcmp(const void *a, const void *b)
{
    u32 x = ((const struct sym *)a)->off, y = ((const struct sym *)b)->off;
    return x < y ? -1 : x > y;
}

static void loadmap(const char *map)
{
    int fd = sys_open(map, O_RDONLY), cap = 0;
    if (fd < 0)
        return;
    struct rd *r = xmalloc(sizeof *r);
    char *l = 0;
    u32 lc = 0;
    rd_init(r, fd);
    while (rd_getline(r, &l, &lc) >= 0) {
        char *e;
        u32 off = (u32)strtoul(l, &e, 16);
        if (e == l || e[0] != ' ' || !e[1] || e[2] != ' ' || !strchr("TtDdBbRr", e[1]))
            continue;                   /* defined symbols only: no U, A, w lines */
        if (nsyms == cap) {
            cap = cap ? cap * 2 : 256;
            struct sym *n = realloc(syms, (u32)cap * sizeof *syms);
            if (!n) die("out of memory");
            syms = n;
        }
        syms[nsyms].off = off;
        if (!(syms[nsyms].name = strdup(e + 3))) die("out of memory");
        nsyms++;
    }
    free(l);
    free(r);
    sys_close(fd);
    qsort(syms, (u32)nsyms, sizeof *syms, symcmp);
}

static u32 place(u32 off) { return !zex2 ? base + off : off < tsize ? code + off : base + off - tsize; }

static u32 unplace(u32 a, int *ok)      /* an address back to an offset in the program's file layout */
{
    *ok = 1;
    if (zex2 && a - code < csize) return a - code;
    if (a - base < 0x1000000) return zex2 ? a - base + tsize : a - base;
    *ok = 0;
    return 0;
}

/* "name+0xOFF", or "" */
static const char *symof(u32 a)
{
    static char b[80];
    int ok, best = -1;
    u32 off = unplace(a, &ok);
    b[0] = 0;
    if (!ok)
        return b;
    for (int i = 0; i < nsyms && syms[i].off <= off; i++)
        best = i;
    if (best >= 0 && off - syms[best].off < 0x10000)
        snprintf(b, sizeof b, off == syms[best].off ? "%s" : "%s+0x%x", syms[best].name, off - syms[best].off);
    return b;
}

static void where(const char *what, u32 a)
{
    const char *s = symof(a);
    oprintf("%s 0x%x%s%s%s\n", what, a, *s ? " (" : "", s, *s ? ")" : "");
}

/* ---- the target ---- */
static struct udbgregs R;
static int getregs(void) { R.size = sizeof R; return sys_debug(DBG_GETREGS, pid, (u32)&R); }

static int peek(u32 a, void *buf, u32 n)
{
    struct udbgmem m = { a, n, (u32)buf };
    return sys_debug(DBG_PEEK, pid, (u32)&m);
}
static u32 peekl(u32 a, int *ok) { u32 v = 0; *ok = peek(a, &v, 4) == 0; return v; }
static int bpat(u32 a);
static u16 peekw(u32 a)
{
    u16 v = 0;
    int i = bpat(a);
    if (i >= 0)
        return bpw[i];
    peek(a, &v, 2);
    return v;
}
static int addbp(u32 a)                 /* a breakpoint the kernel keeps, and its word here */
{
    u16 w = 0;
    if (nb == NB) return -ENOSPC;
    peek(a, &w, 2);
    int e = sys_debug(DBG_BREAK, pid, a);
    if (!e) { bpw[nb] = w; bps[nb++] = a; }
    return e;
}
static void delbp(int i) { bps[i] = bps[--nb]; bpw[i] = bpw[nb]; }

static const char *vecname(u32 v)
{
    static const char *n[] = { 0, 0, "bus error", "address error", "illegal instruction", "division by zero", "CHK",
                               "TRAPV", "privilege violation", "trace", "line A", "line F", 0, 0, "format error" };
    return v < 15 && n[v] ? n[v] : "exception";
}

static const char *regname[] = { "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
                                 "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7" };

/* a location: $reg, $pc, $sp, 0xADDR, a number, symbol[+off]; 1 = understood */
static int loc(const char *s, u32 *a)
{
    char *e;
    if (*s == '$') {
        if (getregs()) return 0;
        if (!strcmp(s + 1, "pc")) { *a = R.pc; return 1; }
        if (!strcmp(s + 1, "sp")) { *a = R.a[7]; return 1; }
        for (int i = 0; i < 16; i++)
            if (!strcmp(s + 1, regname[i])) { *a = i < 8 ? R.d[i] : R.a[i - 8]; return 1; }
        return 0;
    }
    if (isdigit((unsigned char)*s)) {
        *a = (u32)strtoul(s, &e, 0);
        return !*e;
    }
    const char *plus = strchr(s, '+');
    u32 n = plus ? (u32)(plus - s) : strlen(s), add = 0;
    if (plus) {
        add = (u32)strtoul(plus + 1, &e, 0);
        if (*e) return 0;
    }
    for (int i = 0; i < nsyms; i++)
        if (strlen(syms[i].name) == n && !strncmp(syms[i].name, s, n)) {
            *a = place(syms[i].off) + add;
            return 1;
        }
    return 0;
}

static void onint(int s) { (void)s; intr = 1; }

/* wait for the next stop and say what it is; 1 = it has ended */
static int report(void)
{
    struct udbgstop st;
    for (;;) {
        oflush();
        st.size = sizeof st;
        int e = sys_debug(DBG_WAIT, pid, (u32)&st);
        if (e == -EINTR) {
            if (intr) { intr = 0; sys_debug(DBG_STOP, pid, 0); }
            continue;
        }
        if (e < 0) { oprintf("zdb: %s\n", zwhy("the process is gone")); return 1; }
        break;
    }
    lastwhy = st.why;
    switch (st.why) {
    case DBGW_EXIT: oprintf("exited, status %d\n", st.status); pid = -1; return 1;
    case DBGW_EXEC: where("started, stopped at its entry", st.pc); break;
    case DBGW_BREAK: where("breakpoint", st.pc); break;
    case DBGW_STEP: if (!quiet) where("step", st.pc); break;
    case DBGW_SIGNAL: where("stopped by SIGINT at", st.pc); break;
    case DBGW_JOBSTOP: where("stopped by job control at", st.pc); break;
    case DBGW_FAULT:
        oprintf("fault: vector %u (%s), address 0x%x\n", st.info, vecname(st.info), st.addr);
        where("at", st.pc);
        oprintf("(it ends when continued: k, c or q)\n");
        break;
    default: where("stopped at", st.pc);
    }
    return 0;
}

static int bpat(u32 a) { for (int i = 0; i < nb; i++) if (bps[i] == a) return i; return -1; }

/* one instruction, over the breakpoint under the PC if there is one; a TRAP is run to the word after it */
static int step1(void)
{
    if (getregs()) return -1;
    u32 pc = R.pc;
    u16 w = peekw(pc);
    if ((w & 0xFFF0) == 0x4E40) {       /* TRAP #n: never traced (section 8) */
        int had = bpat(pc + 2) >= 0;
        if (!had && sys_debug(DBG_BREAK, pid, pc + 2) < 0) return -1;
        int b = bpat(pc) >= 0;
        if (b) sys_debug(DBG_UNBREAK, pid, pc);
        int e = sys_debug(DBG_CONT, pid, 0);
        if (e) return e;
        int r = report();
        if (!r && b) sys_debug(DBG_BREAK, pid, pc);
        if (!r && !had) sys_debug(DBG_UNBREAK, pid, pc + 2);
        return r;
    }
    int b = bpat(pc) >= 0;
    if (b) sys_debug(DBG_UNBREAK, pid, pc);
    int e = sys_debug(DBG_STEP, pid, 0);
    if (e) {
        if (b) sys_debug(DBG_BREAK, pid, pc);
        return e;
    }
    int r = report();
    if (!r && b) sys_debug(DBG_BREAK, pid, pc);
    return r;
}

static void cont(void)
{
    if (getregs()) { oprintf("zdb: %s\n", zwhy("cannot continue")); return; }
    int b = bpat(R.pc);
    if (b >= 0) {                       /* over the breakpoint first, then put it back */
        sys_debug(DBG_UNBREAK, pid, R.pc);
        int e = sys_debug(DBG_STEP, pid, 0);
        if (e == -EOPNOTSUPP) {         /* Emu68: no steps; the breakpoint goes */
            oprintf("(no single steps here: breakpoint 0x%x taken out)\n", bps[b]);
            delbp(b);
        } else if (e) {
            sys_debug(DBG_BREAK, pid, R.pc);
            oprintf("zdb: %s\n", zwhy("cannot step"));
            return;
        } else {
            u32 at = R.pc;
            quiet = 1;
            int r = report();           /* a step over the breakpoint is quiet; any other stop is said */
            quiet = 0;
            if (r) return;
            sys_debug(DBG_BREAK, pid, at);
            if (lastwhy != DBGW_STEP) return;
        }
    }
    if (sys_debug(DBG_CONT, pid, 0)) { oprintf("zdb: %s\n", zwhy("cannot continue")); return; }
    report();
}

static int callen(u16 w)                /* the length of a jsr/bsr at the PC, 0 if it is none (n) */
{
    if ((w & 0xFF00) == 0x6100) return (w & 0xFF) == 0 ? 4 : (w & 0xFF) == 0xFF ? 6 : 2;
    if ((w & 0xFFF8) == 0x4E90) return 2;
    if ((w & 0xFFF0) == 0x4EA0 && (w & 0xF) >= 8) return 4;
    if (w == 0x4EB8 || w == 0x4EBA || w == 0x4EBB || (w & 0xFFF8) == 0x4EB0) return 4;
    if (w == 0x4EB9) return 6;
    return 0;
}

static void regs(void)
{
    if (getregs()) { oprintf("zdb: %s\n", zwhy("no registers")); return; }
    for (int i = 0; i < 16; i++)
        oprintf("%s 0x%08x%s", regname[i], i < 8 ? R.d[i] : R.a[i - 8], (i & 3) == 3 ? "\n" : "  ");
    oprintf("sr 0x%04x  ", R.sr);
    where("pc", R.pc);
}

static void backtrace(void)
{
    if (getregs()) return;
    where("#0", R.pc);
    u32 fp = R.a[6];
    int n = 1, ok;
    u16 w = peekw(R.pc);
    if (w == 0x4E56 || w == 0x2F0E) {   /* at a function's first instruction (link / move.l a6,-(sp)): its caller's
                                         * return address is still on top of the stack, a6 is the caller's frame */
        where("#1", peekl(R.a[7], &ok));
        n = 2;
    }
    for (; n < 32; n++) {               /* the frame chain (a6): saved a6, then the return address */
        u32 ret = peekl(fp + 4, &ok), nx;
        if (!ok || !ret || (fp & 1)) break;
        char b[8];
        snprintf(b, sizeof b, "#%d", n);
        where(b, ret);
        nx = peekl(fp, &ok);
        if (!ok || nx <= fp) break;
        fp = nx;
    }
    if (n < 3) {                        /* no chain (gcc -O2 keeps none): words on the stack just after a call */
        for (u32 a = R.a[7], k = 0; k < 256; k++, a += 2) {
            u32 v = peekl(a, &ok);
            if (!ok) break;
            int in;
            unplace(v, &in);
            if (!in || (v & 1) || (zex2 ? v - code >= csize : v - base >= tsize + 0x1000000)) continue;
            u16 w2 = peekw(v - 2), w4 = peekw(v - 4), w6 = peekw(v - 6);
            if (callen(w2) == 2 || callen(w4) == 4 || callen(w6) == 6) {
                const char *s = symof(v);
                if (*s) oprintf("scan 0x%x (%s)\n", v, s);
            }
        }
    }
}

static void info(void)
{
    struct udbginfo i = { sizeof i, 0, 0, 0, 0, 0, 0 };
    if (sys_debug(DBG_INFO, pid, (u32)&i) == 0) {
        base = i.base;
        code = i.code;
        csize = i.csize;
    }
}

static void command(char *l)
{
    char *v[4];
    int n = 0;
    for (char *p = l; n < 4;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        v[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    if (!n) return;
    const char *c = v[0];
    u32 a, x;
    int ok, e;
    if (pid < 0 && strcmp(c, "q")) { oprintf("zdb: no process\n"); return; }
    if (!strcmp(c, "b") || !strcmp(c, "d")) {
        if (n < 2 || !loc(v[1], &a)) { oprintf("zdb: where?\n"); return; }
        e = *c == 'b' ? addbp(a) : sys_debug(DBG_UNBREAK, pid, a);
        if (e == -ETXTBSY) { oprintf("zdb: its code is shared with other processes: start it under zdb\n"); return; }
        if (e) { zerrno = -e; oprintf("zdb: %s\n", zwhy("cannot")); return; }
        if (*c == 'b') where("breakpoint at", a);
        else if ((e = bpat(a)) >= 0) delbp(e);
    } else if (!strcmp(c, "info")) {
        struct udbginfo i = { sizeof i, 0, 0, 0, 0, 0, 0 };
        if (sys_debug(DBG_INFO, pid, (u32)&i)) { oprintf("zdb: %s\n", zwhy("no information")); return; }
        oprintf("base 0x%x span %u code 0x%x csize %u shared %u\n", i.base, i.span, i.code, i.csize, i.shared);
    } else if (!strcmp(c, "i")) {
        for (int i = 0; i < nb; i++) where("breakpoint", bps[i]);
    } else if (!strcmp(c, "c")) cont();
    else if (!strcmp(c, "s")) {
        int k = n > 1 ? atoi(v[1]) : 1;
        while (k-- > 0)
            if ((e = step1())) { if (e < 0) oprintf("zdb: %s\n", zwhy("cannot step")); break; }
    } else if (!strcmp(c, "n")) {
        if (getregs()) return;
        int len = callen(peekw(R.pc));
        if (!len) { if ((e = step1()) < 0) oprintf("zdb: %s\n", zwhy("cannot step")); return; }
        u32 at = R.pc + (u32)len;
        int had = bpat(at) >= 0;
        if (!had && addbp(at)) { oprintf("zdb: %s\n", zwhy("cannot")); return; }
        cont();
        if (!had && pid >= 0 && (e = bpat(at)) >= 0) { sys_debug(DBG_UNBREAK, pid, at); delbp(e); }
    } else if (!strcmp(c, "r")) regs();
    else if (!strcmp(c, "x")) {
        if (n < 2 || !loc(v[1], &a)) { oprintf("zdb: where?\n"); return; }
        int k = n > 2 ? atoi(v[2]) : 4;
        for (int i = 0; i < k; i++) {
            x = peekl(a + 4u * (u32)i, &ok);
            if (!ok) { oprintf("\nzdb: %s\n", zwhy("cannot read there")); return; }
            if (!(i & 3)) oprintf("%s0x%08x:", i ? "\n" : "", a + 4u * (u32)i);
            oprintf(" %08x", x);
        }
        oprintf("\n");
    } else if (!strcmp(c, "p")) {
        if (n < 2 || !loc(v[1], &a)) { oprintf("zdb: where?\n"); return; }
        x = peekl(a, &ok);
        if (!ok) { oprintf("zdb: %s\n", zwhy("cannot read there")); return; }
        oprintf("%s = 0x%x (%d)\n", v[1], x, (int)x);
    } else if (!strcmp(c, "set")) {
        if (n < 3 || !loc(v[1], &a)) { oprintf("zdb: where?\n"); return; }
        x = (u32)strtoul(v[2], 0, 0);
        struct udbgmem m = { a, 4, (u32)&x };
        if (sys_debug(DBG_POKE, pid, (u32)&m)) oprintf("zdb: %s\n", zwhy("cannot write there"));
    } else if (!strcmp(c, "bt")) backtrace();
    else if (!strcmp(c, "k")) {
        kill(pid, SIGKILL);
        report();
        pid = -1;
    } else if (!strcmp(c, "detach")) {
        sys_debug(DBG_DETACH, pid, 0);
        oprintf("detached\n");
        pid = -1;
    } else if (!strcmp(c, "q")) {
        if (pid >= 0) {
            if (started) { kill(pid, SIGKILL); report(); }
            else sys_debug(DBG_DETACH, pid, 0);
        }
        oflush();
        _zexit(0);
    } else oprintf("zdb: %s: b d i c s n r x p set bt info k detach q\n", c);
}

/* ---- after a crash: the kernel log's lines (docs/d54-debugger.md section 17) ---- */
static int crash(const char *log, const char *prog)
{
    int fd = strcmp(log, "-") ? sys_open(log, O_RDONLY) : 0;
    if (fd < 0) die("cannot open the log");
    const char *nm = strrchr(prog, '/') ? strrchr(prog, '/') + 1 : prog;
    char want[24], *l = 0, *p;
    u32 lc = 0, vec = 0, pc = 0, fa = 0, rg[16], sr = 0, fr[8];
    int cpid = -1, nfr = 0, have = 0;
    snprintf(want, sizeof want, "(%.15s) killed: vector", nm);
    rd_init(&in, fd);
    while (rd_getline(&in, &l, &lc) >= 0) {
        char *v, *pp, *a;
        if (strstr(l, want) && (p = strstr(l, "[kernel] pid ")) && (v = strstr(l, "vector ")) &&
            (pp = strstr(l, "pc=")) && (a = strstr(l, "addr="))) {
            cpid = atoi(p + 13);
            vec = (u32)atoi(v + 7);
            pc = (u32)strtoul(pp + 3, 0, 16);
            fa = (u32)strtoul(a + 5, 0, 16);
            have = 1;
            nfr = 0;
        } else if (have && (p = strstr(l, "[kernel] pid ")) && atoi(p + 13) == cpid && strstr(p, " regs: ")) {
            char *q = strstr(p, "base "), *c2, *s2, *r1, *r2;   /* a line missing a field is skipped */
            if (!q || !(c2 = strstr(q, "code ")) || !(s2 = strstr(q, "sr ")) || !(r1 = strstr(q, "d0..d7")) ||
                !(r2 = strstr(r1, "a0..a7")))
                continue;
            base = (u32)strtoul(q + 5, 0, 16);
            code = (u32)strtoul(c2 + 5, 0, 16);
            sr = (u32)strtoul(s2 + 3, 0, 16);
            q = r1 + 6;
            for (int i = 0; i < 16; i++) {
                if (i == 8) q = r2 + 6;
                rg[i] = (u32)strtoul(q, &q, 16);
            }
            have = 2;
        } else if (have == 2 && (p = strstr(l, "[kernel] pid ")) && atoi(p + 13) == cpid && (p = strstr(p, " frames: "))) {
            nfr = (int)strtoul(p + 9, &p, 10);
            if (nfr < 0 || nfr > 8) nfr = 0;
            for (int i = 0; i < nfr; i++) fr[i] = (u32)strtoul(p, &p, 16);
        }
    }
    if (!have) { oprintf("zdb: no crash of %s in the log\n", nm); oflush(); return 1; }
    csize = zex2 ? tsize : 0;
    oprintf("crash: pid %d, vector %u (%s), address 0x%x\n", cpid, vec, vecname(vec), fa);
    where("at", pc);
    if (have < 2) { oprintf("(no register lines: an older kernel, or a process the log may not show)\n"); oflush(); return 0; }
    for (int i = 0; i < 16; i++)
        oprintf("%s 0x%08x%s", regname[i], rg[i], (i & 3) == 3 ? "\n" : "  ");
    oprintf("sr 0x%04x\n", sr);
    for (int i = 0; i < nfr && i < 8; i++) {
        char b[8];
        snprintf(b, sizeof b, "#%d", i + 1);
        where(b, fr[i]);
    }
    oflush();
    return 0;
}

static const char USAGE[] = "usage: zdb [-m MAP] PROG [ARG...] | zdb [-m MAP] -p PID PROG | zdb [-m MAP] -c LOG PROG\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    const char *map = 0, *log = 0;
    int i = 1, apid = -1;
    while (i + 1 < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-m")) map = argv[i + 1];
        else if (!strcmp(argv[i], "-p")) apid = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-c")) log = argv[i + 1];
        else zbadopt(argv[i], USAGE);
        i += 2;
    }
    if (i < argc && argv[i][0] == '-') zbadopt(argv[i], USAGE);
    if (i >= argc) zusage(USAGE);
    char path[ZPATH_MAX];
    if (!path_find(argv[i], path, sizeof path)) { dprintf(2, "zdb: %s: not found\n", argv[i]); return 1; }
    header(path);
    if (map) loadmap(map);
    else {
        char *m = xmalloc(strlen(path) + 5);
        strcpy(m, path);
        strcat(m, ".map");
        loadmap(m);
        free(m);
    }
    if (log) return crash(log, path);
    signal(SIGINT, onint);
    if (apid > 0) {
        if (sys_debug(DBG_ATTACH, apid, 0)) { dprintf(2, "zdb: pid %d: %s\n", apid, zwhy("cannot attach")); return 1; }
        pid = apid;
    } else {
        if (sys_debug(DBG_NEXT, 0, 0)) { dprintf(2, "zdb: %s\n", zwhy("cannot debug")); return 1; }
        pid = sys_spawn(path, argv + i, 0);
        if (pid < 0) { dprintf(2, "zdb: %s: %s\n", argv[i], zwhy("cannot run")); return 1; }
        started = 1;
    }
    info();
    if (report()) return 0;
    if (nsyms) oprintf("%d symbols\n", nsyms);
    char *l = 0;
    u32 lc = 0;
    rd_init(&in, 0);
    int tty = sys_isatty(0) > 0;        /* the prompt only for a person */
    for (;;) {
        if (tty) oprintf("(zdb) ");
        oflush();
        if (rd_getline(&in, &l, &lc) < 0) break;
        command(l);
    }
    command("q");
    return 0;
}
