/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* Ahiru - a Ranger/Yazi-style file manager for AmigaOS 2.04+.
 *
 * Three columns, Ranger's 1:3:4: the parent directory, the current
 * one, and a preview of whatever the cursor is on. vi keys, prefix
 * keys with a which-key panel (g, y, d, p, c, u, z, o, m, '), colours
 * by file kind on a 4, 8 or 16 colour screen of its own.
 *
 * Usage: Ahiru [PATH] [SCREEN|WINDOW|TERMINAL] [LOG] [KEYS=...]   (see README)
 */
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/datetime.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <intuition/intuition.h>
#include <proto/graphics.h>
#include <stdlib.h>
#include <string.h>
#include "ahiru.h"
int sfmt(char *dst, int size, const char *f, ...);   /* core/util.h (its datestr is not ours) */
#include "fs/os.h"
#include "fs/arcfs.h"
#include "fs/arcw.h"
#include "fs/renames.h"
#include "fs/usercmd.h"
#include "fs/cx.h"
#include "fs/sums.h"
#include "fs/mount.h"
#include "fs/modload.h"
#include "core/arc.h"

static const char verstag[] __attribute__((used)) =
    "$VER: Ahiru 0.3b6 (17.10.2026) - file manager";

#define CFGFILE  "PROGDIR:Ahiru.config"
#define MARKFILE "PROGDIR:Ahiru.marks"

DList *cur, *par;
int parnone;
int hideinfo = 1, showpv = 1, sortmode = SO_NAME, sortrev = 0;
char filter[64];
char cfgeditor[128];
static char cfgstart[PATHMAX] = "SYS:";
static char lastdir[PATHMAX];
static int savedir = 1;
static char cfghotkey[48] = "ctrl alt a";  /* HOTKEY: the commodity's */
static int cfgpopup = 1;        /* CXPOPUP no: start hidden, q only hides */
static int cxon, resident;      /* a commodity; quitting only hides */
static int hidenow;             /* leave the loop to hide, not to quit */
static char search[64];

/* LOG on the command line: each startup stage appends a line to
 * RAM:Ahiru.log - the road for a machine whose screen we cannot see */
static int logging;
#ifndef LOGFILE
#define LOGFILE "RAM:Ahiru.log"
#endif
void dbg(const char *what)
{
    BPTR fh;
    if (!logging) return;
    char m[40];
    if (!(fh = Open((STRPTR)LOGFILE, MODE_READWRITE))) return;
    Seek(fh, 0, OFFSET_END);
    FPuts(fh, (STRPTR)what);
    /* the free memory at each stage: where it goes */
    fmt(m, "  (free %ld, chip %ld)\n", (long)AvailMem(MEMF_ANY), (long)AvailMem(MEMF_CHIP));
    FPuts(fh, (STRPTR)m);
    Close(fh);
}
static long freecache = -1;
static char freepath[PATHMAX];

/* ---- where the cursor was, per directory (Ranger remembers) ---- */
#define HISTN 48
static char *hpath[HISTN], *hname[HISTN];
static int hnext;

static void remember(const char *path, const char *name)
{
    int i;
    for (i = 0; i < HISTN; i++)
        if (hpath[i] && nocase(hpath[i], path) == 0) break;
    if (i == HISTN) {
        i = hnext;
        hnext = (hnext + 1) % HISTN;
        free(hpath[i]);
        free(hname[i]);
        hpath[i] = strdup(path);
        hname[i] = NULL;
    }
    free(hname[i]);
    hname[i] = strdup(name);
}

long recallsel(DList *l)
{
    int i;
    for (i = 0; i < HISTN; i++)
        if (hpath[i] && hname[i] && nocase(hpath[i], l->pl.path) == 0) {
            long k = dl_find(l, hname[i]);
            return k < 0 ? 0 : k;
        }
    return 0;
}

/* ---- bookmarks: m<key> sets, '<key> jumps ------------------------- */
static char *bmark[128];

static void loadmarks(void)
{
    BPTR fh;
    char line[PATHMAX + 4];
    /* (before 0.2b2 the program was UP-File: its marks are read once) */
    if (!(fh = Open((STRPTR)MARKFILE, MODE_OLDFILE)) && !(fh = Open((STRPTR)"PROGDIR:UP-File.marks", MODE_OLDFILE))) return;
    while (FGets(fh, (STRPTR)line, sizeof(line))) {
        int l = (int)strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (l >= 3 && line[1] == ' ' && (unsigned char)line[0] < 128) {
            free(bmark[(int)line[0]]);
            bmark[(int)line[0]] = strdup(line + 2);
        }
    }
    Close(fh);
}

static void savemarks(void)
{
    BPTR fh;
    int i;
    char line[PATHMAX + 4];
    if (!(fh = Open((STRPTR)MARKFILE, MODE_NEWFILE))) return;
    for (i = 33; i < 128; i++)
        if (bmark[i]) {
            sfmt(line, sizeof(line), "%c %s\n", i, bmark[i]);
            FPuts(fh, (STRPTR)line);
        }
    Close(fh);
}

/* ---- config --------------------------------------------------------- */

static int yes(const char *v)
{
    return nocase(v, "yes") == 0 || nocase(v, "on") == 0 || strcmp(v, "1") == 0;
}

/* MODE's words: auto, screen, window, terminal */
static int modeword(const char *v, int dflt)
{
    if (nocase(v, "auto") == 0) return UI_AUTO;
    if (nocase(v, "screen") == 0) return UI_SCREEN;
    if (nocase(v, "window") == 0) return UI_WINDOW;
    if (nocase(v, "terminal") == 0 || nocase(v, "term") == 0) return UI_TERM;
    return dflt;
}

static void loadconfig(void)
{
    BPTR fh;
    char line[PATHMAX + 32];
    if (!(fh = Open((STRPTR)CFGFILE, MODE_OLDFILE)) && !(fh = Open((STRPTR)"PROGDIR:UP-File.config", MODE_OLDFILE))) return;
    while (FGets(fh, (STRPTR)line, sizeof(line))) {
        char *k = line, *v;
        int l = (int)strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' '))
            line[--l] = 0;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == ';' || *k == '#' || !*k) continue;
        for (v = k; *v && *v != ' ' && *v != '\t' && *v != '='; v++) ;
        if (*v) *v++ = 0;
        while (*v == ' ' || *v == '\t' || *v == '=') v++;
        if (nocase(k, "COLORS") == 0 || nocase(k, "COLOURS") == 0) {
            int c = atoi(v);
            colors = c <= 4 ? 4 : c >= 16 ? 16 : 8;
        } else if (nocase(k, "FONT") == 0) strncpy(cfgfont, v, sizeof(cfgfont) - 1);
        else if (nocase(k, "HIDEINFO") == 0) hideinfo = yes(v);
        else if (nocase(k, "PREVIEW") == 0) showpv = yes(v);
        else if (nocase(k, "START") == 0) strncpy(cfgstart, v, PATHMAX - 1);
        else if (nocase(k, "SAVEDIR") == 0) savedir = yes(v);
        else if (nocase(k, "HOTKEY") == 0) strncpy(cfghotkey, v, sizeof(cfghotkey) - 1);
        else if (nocase(k, "CXPOPUP") == 0) cfgpopup = yes(v);
        else if (nocase(k, "VERIFY") == 0) verifycopy = yes(v);
        else if (nocase(k, "LASTDIR") == 0) strncpy(lastdir, v, PATHMAX - 1);
        else if (nocase(k, "EDITOR") == 0) strncpy(cfgeditor, v, sizeof(cfgeditor) - 1);
        else if (nocase(k, "EDITORCONSOLE") == 0) cfgedcon = yes(v);
        else if (nocase(k, "MODE") == 0) cfgmode = modeword(v, cfgmode);
        else if (nocase(k, "WINDOW") == 0) {
            char *q = v;
            int i;
            for (i = 0; i < 4 && *q; i++) {
                winbox[i] = atoi(q);
                while (*q && *q != ' ') q++;
                while (*q == ' ') q++;
            }
        }
        else if (nocase(k, "LINEMODE") == 0) linemode = nocase(v, "date") == 0 ? 1 : nocase(v, "bits") == 0 ? 2 : nocase(v, "none") == 0 ? 3 : 0;
        else if (nocase(k, "OPENWITH") == 0) openwith_add(v);
        else if (nocase(k, "SORT") == 0) {
            sortmode = nocase(v, "size") == 0 ? SO_SIZE : nocase(v, "date") == 0 ? SO_DATE :
                       nocase(v, "ext") == 0 ? SO_EXT : SO_NAME;
        } else if (nocase(k, "REVERSE") == 0) sortrev = yes(v);
    }
    Close(fh);
}

static void saveconfig(void)
{
    BPTR fh;
    char line[PATHMAX + 40];
    static const char *const sorts[] = { "name", "size", "date", "ext" };
    if (!(fh = Open((STRPTR)CFGFILE, MODE_NEWFILE))) return;
    FPuts(fh, (STRPTR)"; Ahiru settings - edit freely, unknown lines are ignored\n");
    fmt(line, "COLORS %d\n", colors); FPuts(fh, (STRPTR)line);
    fmt(line, "FONT %s\n", cfgfont); FPuts(fh, (STRPTR)line);
    fmt(line, "HIDEINFO %s\n", hideinfo ? "yes" : "no"); FPuts(fh, (STRPTR)line);
    fmt(line, "PREVIEW %s\n", showpv ? "yes" : "no"); FPuts(fh, (STRPTR)line);
    fmt(line, "SORT %s\n", sorts[sortmode]); FPuts(fh, (STRPTR)line);
    fmt(line, "REVERSE %s\n", sortrev ? "yes" : "no"); FPuts(fh, (STRPTR)line);
    fmt(line, "START %s\n", cfgstart); FPuts(fh, (STRPTR)line);
    fmt(line, "SAVEDIR %s\n", savedir ? "yes" : "no"); FPuts(fh, (STRPTR)line);
    FPuts(fh, (STRPTR)"; HOTKEY (2.0+): Ahiru comes up in the drawer Workbench has active\n");
    fmt(line, "HOTKEY %s\n", cfghotkey); FPuts(fh, (STRPTR)line);
    FPuts(fh, (STRPTR)"; CXPOPUP no: start hidden and wait for the hotkey; q then only hides Ahiru\n");
    fmt(line, "CXPOPUP %s\n", cfgpopup ? "yes" : "no"); FPuts(fh, (STRPTR)line);
    FPuts(fh, (STRPTR)"; VERIFY yes: every copy is read back and compared before it is kept (slower)\n");
    fmt(line, "VERIFY %s\n", verifycopy ? "yes" : "no"); FPuts(fh, (STRPTR)line);
    if (savedir) {
        char ld[PATHMAX];
        strncpy(ld, cur ? cur->pl.path : lastdir, PATHMAX - 1);
        ld[PATHMAX - 1] = 0;
        mnt_unmap(ld, sizeof(ld));      /* DAn: names nothing next time */
        fmt(line, "LASTDIR %s\n", ld);
        FPuts(fh, (STRPTR)line);
    }
    fmt(line, "EDITOR %s\n", cfgeditor); FPuts(fh, (STRPTR)line);
    FPuts(fh, (STRPTR)"; EDITORCONSOLE yes: the editor runs in a console (Sumi is known as one)\n");
    fmt(line, "EDITORCONSOLE %s\n", cfgedcon ? "yes" : "no"); FPuts(fh, (STRPTR)line);
    {
        static const char *const md[] = { "auto", "screen", "window", "terminal" };
        FPuts(fh, (STRPTR)"; MODE auto: inside the terminal when started from a Shell in CTerm, else the screen;\n"
                          ";   screen / window (on Workbench) / terminal - also SCREEN, WINDOW, TERMINAL when started\n");
        fmt(line, "MODE %s\n", md[cfgmode & 3]);
        FPuts(fh, (STRPTR)line);
        if (winbox[2] > 0) {
            fmt(line, "WINDOW %ld %ld %ld %ld\n", (long)winbox[0], (long)winbox[1], (long)winbox[2], (long)winbox[3]);
            FPuts(fh, (STRPTR)line);
        }
    }
    {
        static const char *const lm[] = { "size", "date", "bits", "none" };
        fmt(line, "LINEMODE %s\n", lm[linemode & 3]);
        FPuts(fh, (STRPTR)line);
    }
    FPuts(fh, (STRPTR)"; OPENWITH ext program: offered by O for files ending .ext (* = any)\n");
    openwith_save(fh);
    Close(fh);
}

/* ---- drawing ---------------------------------------------------------- */

#ifndef FIBF_HIDDEN
#define FIBF_HIDDEN (1 << 7)     /* the h bit: not in the NDK's dos.h */
#endif

/* in the order csh's ls takes them (CShell tools/ls.c kindof) */
int roleof(long fl, const char *name)
{
    int k, r;
    if (fl & F_ASN) return RO_ASN;
    if (fl & F_LINK) return RO_LINK;
    if (fl & F_DIR) return RO_DIR;
    k = kindof(name);
    if (k == K_INFO) return RO_INFO;
    if (fl & (FIBF_HIDDEN << 16)) return RO_HIDDEN;
    if (fl & (FIBF_SCRIPT << 16)) return RO_SCRIPT;
    if ((r = filecol_extrole(name)) >= 0) return r;
    switch (k) {
    case K_ARCH: return RO_ARCH;
    case K_IMG: return RO_IMG;
    case K_AUD: return RO_AUD;
    case K_SRC: return RO_SRC;
    }
    if (fl & F_EXEC) return RO_EXEC;
    return RO_TEXT;
}

/* one entry as w cells in rowbuf: mark, name, size at the right */
static void protstr(char *d, long prot);
static void datestr(char *d, long key);
int linemode;                   /* what follows the name: 0 size, 1 date, 2 bits, 3 nothing */

void entrytext(DList *l, long i, int w, int sizes, int *role)
{
    long f = dflags(l, i);
    const char *name = DNAME(l, i);
    char sz[32];
    int sl = 0, nl = (int)strlen(name), room, k;
    memset(rowbuf, ' ', w);
    sz[0] = 0;
    if (sizes && linemode == 0 && !(f & F_DIR)) fmtsize(sz, (unsigned long)DSIZE(l, i));
    else if (sizes && linemode == 1 && !(f & (F_VOL | F_ASN)) && DDATE(l, i)) datestr(sz, DDATE(l, i));
    else if (sizes && linemode == 2 && !(f & (F_VOL | F_ASN))) protstr(sz, (f >> 16) & 0xFF);
    sl = (int)strlen(sz);
    room = w - 2 - (sl ? sl + 2 : 0);
    if (room < 1) {
        room = w - 2;
        sl = 0;
    }
    if (f & F_MARK) rowbuf[0] = '*';
    for (k = 0; k < nl && k < room; k++) rowbuf[1 + k] = name[k];
    if (nl > room && room > 0) rowbuf[room] = '~';
    if (sl) memcpy(rowbuf + w - 1 - sl, sz, sl);
    *role = (f & F_MARK) ? RO_MARK : roleof(f, name);
}

/* the status row's count of the marked: counted again only after the
 * current list was drawn whole (every change of a mark, a filter or a
 * folder draws it), not on each move of the cursor */
static int markstale = 1;
static long marknum, markbytes;

static void drawlist(DList *l, int x, int w, int sizes, int showsel)
{
    int r;
    if (l == cur) markstale = 1;
    for (r = 0; r < listh; r++) {
        long i = l->top + r;
        if (i < l->n) {
            int role;
            entrytext(l, i, w, sizes, &role);
            if (showsel && i == l->sel)
                put(x, r + 1, (char *)rowbuf, w, 0, rolepen[role]);
            else
                put(x, r + 1, (char *)rowbuf, w, rolepen[role], 0);
        } else
            clearrow(r + 1, x, w);
    }
}

void drawtitle(void)
{
    char t[PATHMAX + 40];
    const char *p = cur->pl.path[0] ? cur->pl.path : "Volumes";
    int pl = (int)strlen(p), avail, rl = 0;
    char right[80];
    clearrow(0, 0, ncols);
    right[0] = 0;
    /* the tabs, the search the list shows, the filter */
    tab_label(right);
    {
        char rs[60];
        results_label(rs);
        if (rs[0]) fmt(right + strlen(right), "%s%s", right[0] ? "  " : "", rs);
    }
    if (filter[0]) fmt(right + strlen(right), "%sfilter: %.30s", right[0] ? "  " : "", filter);
    if (right[0]) {
        rl = (int)strlen(right);
        put(ncols - rl - 1, 0, right, rl, rolepen[RO_MARK], 0);
    }
    avail = ncols - 2 - (rl ? rl + 2 : 0);
    if (pl > avail - 12 && avail > 20) {    /* long paths keep their tail */
        fmt(t, "...%s", p + pl - (avail - 15));
        p = t;
        pl = (int)strlen(p);
    }
    put(1, 0, p, pl, rolepen[RO_DIR], 0);
    if (cur->n && cur->pl.path[0]) {
        const char *n = DNAME(cur, cur->sel);
        int need = cur->pl.path[strlen(cur->pl.path) - 1] == ':' ? 0 : 1;
        int nl = (int)strlen(n), x = 1 + pl;
        if (need && x < avail) put(x++, 0, "/", 1, rolepen[RO_DIR], 0);
        if (nl > avail - x) nl = avail - x;
        put(x, 0, n, nl, rolepen[RO_HI], 0);
    }
}

static long freebytes(const char *path)
{
    struct InfoData *id;
    BPTR lock;
    long n = -1;
    APTR old;
    if (!path[0]) return -1;
    if (freecache >= 0 && nocase(freepath, path) == 0) return freecache;
    if (!(id = AllocMem(sizeof(struct InfoData), MEMF_ANY))) return -1;
    old = quietreq();
    if ((lock = Lock((STRPTR)path, SHARED_LOCK)) != 0) {
        if (Info(lock, id)) {
            unsigned long fr = id->id_NumBlocks - id->id_NumBlocksUsed;
            unsigned long bpb = id->id_BytesPerBlock;
            if (bpb && fr > 0x7FFFFFFFUL / bpb) n = 0x7FFFFFFF;
            else n = (long)(fr * bpb);
        }
        UnLock(lock);
    }
    loudreq(old);
    FreeMem(id, sizeof(struct InfoData));
    strcpy(freepath, path);
    freecache = n;
    return n;
}

static void protstr(char *d, long prot)
{
    static const char bits[] = "hsparwed";
    int i;
    for (i = 0; i < 8; i++) {
        int b = 7 - i, set = (prot >> b) & 1;
        d[i] = (b >= 4 ? set : !set) ? bits[i] : '-';
    }
    d[8] = 0;
}

static void datestr(char *d, long key)
{
    struct DateTime dt;
    char ds[16], ts[16];
    memset(&dt, 0, sizeof(dt));
    dt.dat_Stamp.ds_Days = key / 1440;
    dt.dat_Stamp.ds_Minute = key % 1440;
    dt.dat_Format = FORMAT_DOS;
    dt.dat_StrDate = (STRPTR)ds;
    dt.dat_StrTime = (STRPTR)ts;
    ds[0] = ts[0] = 0;
    DateToStr(&dt);
    ts[5] = 0;                       /* drop the seconds */
    fmt(d, "%s %s", ds, ts);
}

void drawstatus(void)
{
    char left[MAXCOLS], right[MAXCOLS], t[40];
    long i, nm = 0, mb = 0, fr;
    int ll, rl;
    if (msgup) return;
    clearrow(nrows - 1, 0, ncols);
    left[0] = right[0] = 0;
    job_label(right);
    play_label(right + strlen(right));
    if (visual_on()) strcpy(left, "VISUAL  ");
    if (cur->n) {
        char *l0 = left + strlen(left);
        long f = dflags(cur, cur->sel);
        if (f & F_VOL) strcpy(l0, "volume");
        else if (f & F_ASN) strcpy(l0, "assign");
        else {
            char pr[10], dt[32];
            protstr(pr, (f >> 16) & 0xFF);
            datestr(dt, DDATE(cur, cur->sel));
            if (f & F_DIR) fmt(l0, "%s  %s", pr, dt);
            else fmt(l0, "%s  %ld  %s", pr, DSIZE(cur, cur->sel), dt);
        }
    } else if (!cur->ok)
        strcpy(left, "cannot read this directory");
    else
        strcpy(left, "empty");
    if (markstale) {
        for (i = 0; i < cur->n; i++)
            if (dflags(cur, i) & F_MARK) {
                nm++;
                mb += DSIZE(cur, i);
            }
        marknum = nm, markbytes = mb;
        markstale = 0;
    }
    nm = marknum, mb = markbytes;
    if (nm) {
        fmtsize(t, (unsigned long)mb);
        fmt(right + strlen(right), "%ld marked %s  ", nm, t);
    }
    if (yankn)
        fmt(right + strlen(right), "%s %ld  ", yankcut ? "cut" : "yank", yankn);
    fr = freebytes(cur->pl.path);
    if (fr >= 0) {
        fmtsize(t, (unsigned long)fr);
        fmt(right + strlen(right), "%s free  ", t);
    }
    fmt(right + strlen(right), "%ld/%ld", cur->n ? cur->sel + 1 : 0, cur->n);
    rl = (int)strlen(right);
    ll = (int)strlen(left);
    if (ll > ncols - rl - 3) ll = ncols - rl - 3;
    put(1, nrows - 1, left, ll, rolepen[cur->ok ? RO_DIM : RO_ERR], 0);
    put(ncols - rl - 1, nrows - 1, right, rl, rolepen[RO_ACC], 0);
}

static void centre(DList *l)
{
    l->top = l->sel - listh / 2;
    dl_clamp(l, listh);
}

void drawcols(void)
{
    int r;
    if (parnone) {
        for (r = 1; r <= listh; r++) clearrow(r, c0x, c0w);
    } else {
        centre(par);
        drawlist(par, c0x, c0w, 0, 1);
    }
    dl_clamp(cur, listh);
    drawlist(cur, c1x, c1w, 1, 1);
    if (cur->n == 0) putrole(c1x + 1, 1, cur->ok ? "empty" : "cannot read", cur->ok ? 5 : 11,
                             cur->ok ? RO_DIM : RO_ERR);
}

void drawall(void)
{
    int r;
    ui_clearall();
    for (r = 0; r < nrows; r++) clearrow(r, 0, ncols);     /* the grid copy too */
    drawtitle();
    drawcols();
    for (r = 1; r <= listh; r++) clearrow(r, c1x + c1w, 1);
    pv_draw();
    msgup = 0;
    drawstatus();
}

/* ---- moving about ------------------------------------------------------- */

void selpath(char *dst)
{
    if (cur->n == 0) {
        strcpy(dst, cur->pl.path);
        return;
    }
    joinpath(dst, cur->pl.path, DNAME(cur, cur->sel));
}

static void setsel(long n)
{
    long old = cur->sel, oldtop = cur->top;
    if (cur->n == 0) return;
    if (n < 0) n = 0;
    if (n >= cur->n) n = cur->n - 1;
    if (n == old) return;
    cur->sel = n;
    dl_clamp(cur, listh);
    if (cur->top != oldtop) drawlist(cur, c1x, c1w, 1, 1);
    else {
        int role;
        entrytext(cur, old, c1w, 1, &role);
        put(c1x, (int)(old - cur->top) + 1, (char *)rowbuf, c1w, rolepen[role], 0);
        entrytext(cur, n, c1w, 1, &role);
        put(c1x, (int)(n - cur->top) + 1, (char *)rowbuf, c1w, 0, rolepen[role]);
    }
    drawtitle();
    drawstatus();
    pvdirty = 1;
    visual_moved();
}

/* read cur and par for path, cursor on selname (or where it was) */
void loaddir(const char *path, const char *selname)
{
    char pp[PATHMAX];
    long k;
    visual_end();                   /* a new folder: visual mode and the hits end */
    results_end();
    if (nocase(path, cur->pl.path) != 0) habit_visit(path);    /* a reload is no visit */
    dl_read(cur, path, RD_HIDE | RD_FILTER | RD_SORT);
    cur->top = 0;
    k = selname ? dl_find(cur, selname) : -1;
    cur->sel = k >= 0 ? k : recallsel(cur);
    dl_clamp(cur, listh);
    parnone = cur->pl.path[0] == 0;
    if (!parnone) {
        parentpath(pp, cur->pl.path);
        dl_read(par, pp, RD_HIDE | RD_SORT);
        k = dl_find(par, lastpart(cur->pl.path));
        par->sel = k >= 0 ? k : 0;
    }
    freecache = -1;
    pv_invalidate();
}

void reloadall(const char *selname)
{
    char keep[110], path[PATHMAX];
    keep[0] = 0;
    if (!selname && cur->n) {
        strncpy(keep, DNAME(cur, cur->sel), sizeof(keep) - 1);
        keep[sizeof(keep) - 1] = 0;
        selname = keep;
    }
    strcpy(path, cur->pl.path);
    {
        long oldsel = cur->sel;
        loaddir(path, selname);
        if (selname && dl_find(cur, selname) < 0 && oldsel < cur->n) cur->sel = oldsel;
        dl_clamp(cur, listh);
    }
    drawall();
}

void godir(const char *path, const char *selname)
{
    char canon[PATHMAX];
    if (cur->n) remember(cur->pl.path, DNAME(cur, cur->sel));
    if (path[0]) {
        int d = isdirpath(path);
        if (d < 0) {
            /* not on a disk: a folder inside an archive or disk image? */
            char old[PATHMAX];
            strcpy(old, cur->pl.path);
            if (!place_go(&cur->pl, path)) {
                char m[PATHMAX + 20];
                place_go(&cur->pl, old);
                fmt(m, "cannot go to %.200s", path);
                say(m, 1);
                return;
            }
            strcpy(canon, path);
        } else if (!canonical(path, canon)) {
            char m[PATHMAX + 20];
            fmt(m, "cannot go to %.200s", path);
            say(m, 1);
            return;
        } else if (d == 0) {
            /* a file: an archive or disk image is entered, anything
             * else opens its drawer with the cursor on it */
            int k = arcfs_kind(canon);
            if (k == AK_NONE || k == AK_DMS) {
                char pp[PATHMAX];
                parentpath(pp, canon);
                loaddir(pp, lastpart(canon));
                drawall();
                return;
            }
        }
        path = canon;
    }
    filter[0] = 0;
    loaddir(path, selname);
    drawall();
}

static void goup(void)
{
    char pp[PATHMAX], here[PATHMAX];
    DList *t;
    if (cur->pl.path[0] == 0) return;
    visual_end();
    results_end();
    if (cur->n) remember(cur->pl.path, DNAME(cur, cur->sel));
    strcpy(here, cur->pl.path);
    parentpath(pp, here);
    filter[0] = 0;
    /* Ranger's rotation: the parent becomes current, the directory
     * we leave becomes the preview - only the new parent is read */
    t = prv;
    prv = cur;
    cur = par;
    par = t;
    if (nocase(cur->pl.path, pp) != 0 || !cur->ok)
        dl_read(cur, pp, RD_HIDE | RD_FILTER | RD_SORT);
    else
        cur->how = RD_HIDE | RD_FILTER | RD_SORT;     /* (the filter is empty now) */
    {
        long k = dl_find(cur, lastpart(here));
        cur->sel = k >= 0 ? k : 0;
    }
    dl_clamp(cur, listh);
    parnone = cur->pl.path[0] == 0;
    if (!parnone) {
        char gp[PATHMAX];
        long k;
        parentpath(gp, cur->pl.path);
        dl_read(par, gp, RD_HIDE | RD_SORT);
        k = dl_find(par, lastpart(cur->pl.path));
        par->sel = k >= 0 ? k : 0;
    }
    freecache = -1;
    pv_adopt(here);                 /* the dir we left IS the preview */
    drawall();
}

/* h: up a level - or, from a list of hits, back to the folder it was
 * searched in (the key and a click in the parent column alike) */
static void parent(void)
{
    if (results_on()) {
        char p[PATHMAX];
        strcpy(p, cur->pl.path);
        loaddir(p, NULL);
        drawall();
    } else
        goup();
}

static void enter(void)
{
    char full[PATHMAX];
    long f;
    int ak = AK_NONE;
    if (cur->n == 0) return;
    f = dflags(cur, cur->sel);
    selpath(full);
    if (!(f & F_DIR)) {
        /* an archive or disk image on a disk opens like a folder */
        if (!cur->pl.arc) ak = arcfs_kind(full);
        if (ak == AK_DMS && job_busy()) {
            say("a copy runs in the background - the DMS waits for it", 1);
            return;
        }
        if (ak == AK_DMS) {
            /* a DMS file becomes an .adf beside it (asks), and opens */
            if (arcfs_open(&cur->pl, full)) loaddir(cur->pl.path, NULL), drawall();
            return;
        }
        if (ak == AK_NONE) {
            op_openfile();
            return;
        }
    }
    if ((f & (F_VOL | F_ASN)) || filter[0]) {
        godir(full, NULL);          /* canonical names, fresh reads */
        return;
    }
    visual_end();
    results_end();
    {
        char canon[PATHMAX];
        DList *t;
        /* inside an archive (or entering one) the path is the name */
        if (cur->pl.arc || ak != AK_NONE) strcpy(canon, full);
        else if (!canonical(full, canon)) {
            showfault("cannot enter");
            return;
        }
        remember(cur->pl.path, DNAME(cur, cur->sel));
        filter[0] = 0;
        t = par;
        par = cur;
        parnone = 0;
        if (nocase(prv->pl.path, canon) == 0 && prv->ok) {
            cur = prv;              /* the preview already read it */
            prv = t;
            cur->how = RD_HIDE | RD_FILTER | RD_SORT;    /* (the filter is empty now) */
        } else {
            cur = t;
            if (!dl_read(cur, canon, RD_HIDE | RD_FILTER | RD_SORT) && ak != AK_NONE) {
                /* not an archive after all: back where we were */
                t = cur;
                cur = par;
                par = t;
                loaddir(cur->pl.path, NULL);
                drawall();
                return;
            }
        }
        cur->sel = recallsel(cur);
        cur->top = 0;
        dl_clamp(cur, listh);
        habit_visit(cur->pl.path);
        freecache = -1;
        pv_invalidate();
        drawall();
    }
}

static void togglemark(void)
{
    if (cur->n == 0 || cur->pl.path[0] == 0) return;
    DENT(cur, cur->sel)->mark ^= 1;
    if (cur->sel < cur->n - 1) {
        cur->sel++;
        dl_clamp(cur, listh);
        pvdirty = 1;
    }
    drawlist(cur, c1x, c1w, 1, 1);
    drawtitle();
    drawstatus();
}

static void searchnext(int dir)
{
    long i, n = cur->n;
    if (!search[0] || n == 0) return;
    for (i = 1; i <= n; i++) {
        long k = ((cur->sel + dir * i) % n + n) % n;
        if (nchas(DNAME(cur, k), search)) {
            setsel(k);
            return;
        }
    }
    say("no match", 1);
}

static void resort(void)
{
    char keep[256];                 /* a member name runs to 255 */
    keep[0] = 0;
    if (cur->n) strcpy(keep, DNAME(cur, cur->sel));
    visual_end();                   /* the rows change under it (the marks stay) */
    dl_sort(cur);
    if (!parnone) {
        char pk[256];
        strcpy(pk, par->n ? DNAME(par, par->sel) : "");
        dl_sort(par);
        par->sel = dl_find(par, pk) < 0 ? 0 : dl_find(par, pk);
    }
    if (keep[0]) cur->sel = dl_find(cur, keep) < 0 ? 0 : dl_find(cur, keep);
    pv_invalidate();
    drawall();
}

static int reopen(int newcolors)
{
    char keep[PATHMAX];
    if (ui_otherwindows()) {
        say("close the other windows on this screen first", 1);
        return 0;
    }
    strcpy(keep, cur->pl.path);
    ui_close();
    colors = newcolors;
    if (!ui_open()) return -1;
    return 1;
}

static void showbookmarks(void)
{
    static const char *items[2 * 96];
    static char keys[96][2];
    int n = 0, i;
    for (i = 33; i < 128 && n < 96; i++)
        if (bmark[i]) {
            keys[n][0] = (char)i;
            keys[n][1] = 0;
            items[2 * n] = keys[n];
            items[2 * n + 1] = bmark[i];
            n++;
        }
    if (n == 0) {
        items[0] = "";
        items[1] = "no bookmarks yet - m + key sets one";
        n = 1;
    }
    drawhints(items, n, 40);
}

static void gotopath(void)
{
    char p[PATHMAX];
    strcpy(p, cur->pl.path);
    if (editline("cd ", p, PATHMAX - 1, -1)) godir(p, NULL);
}

/* 1 = quit */
static int console(void)
{
    char line[PATHMAX], *arg;
    line[0] = 0;
    if (!editline(":", line, PATHMAX - 1, 0)) return 0;
    for (arg = line; *arg && *arg != ' '; arg++) ;
    if (*arg) *arg++ = 0;
    while (*arg == ' ') arg++;
    if (nocase(line, "mkdir") == 0) op_mkdir(arg);
    else if (nocase(line, "touch") == 0) op_touch(arg);
    else if (nocase(line, "cd") == 0) godir(arg, NULL);
    else if (nocase(line, "shell") == 0) op_shell(arg);
    else if (nocase(line, "rename") == 0) {
        /* :rename NEW renames the entry under the cursor (on a disk) to
         * NEW; without one, or in an archive, it asks */
        if (arg[0] && cur->n && !cur->pl.arc && cur->pl.path[0]) {
            if (fs_renameone(&cur->pl, DIDX(cur), arg)) reloadall(arg);
            else say(opmsg, 1);
        } else op_rename(2);
    } else if (nocase(line, "q") == 0 || nocase(line, "quit") == 0) {
        return 1;
    } else if (line[0])
        say("commands: mkdir, touch, cd, shell, rename, quit", 1);
    return 0;
}

/* sort ARG: name, size, time, ext ["reverse"], or flip */
static void dosort(const char *arg)
{
    char w[16];
    int i = 0;
    if (!arg) return;
    while (arg[i] && arg[i] != ' ' && i < 15) w[i] = arg[i], i++;
    w[i] = 0;
    if (nocase(w, "flip") == 0) sortrev = !sortrev;
    else {
        sortmode = nocase(w, "size") == 0 ? SO_SIZE : nocase(w, "time") == 0 || nocase(w, "date") == 0 ? SO_DATE :
                   nocase(w, "ext") == 0 ? SO_EXT : SO_NAME;
        sortrev = nchas(arg + i, "rev") != 0 && arg[i];
    }
    resort();
}

/* go ARG: a path, or volumes, root, home */
static void dogo(const char *arg)
{
    char p[PATHMAX];
    if (!arg) return;
    if (nocase(arg, "volumes") == 0) godir("", lastpart(cur->pl.path));
    else if (nocase(arg, "home") == 0) godir(cfgstart, NULL);
    else if (nocase(arg, "root") == 0) {
        char *c;
        if (!cur->pl.path[0]) return;
        strcpy(p, cur->pl.path);
        if ((c = strchr(p, ':')) != NULL) c[1] = 0;
        godir(p, NULL);
    } else godir(arg, NULL);
}

/* one action; 1 = quit, -1 = the screen could not open again */
static int run(int act, const char *arg)
{
    char buf[PATHMAX];
    /* while a copy runs in the background, what would use the core's
     * working state waits for it */
    if (job_busy()) {
        switch (act) {
        case A_PASTE: case A_PASTEOVER: case A_DELETE: case A_RENAME: case A_RENAMEEXT:
        case A_RENAMEEND: case A_RENAMESTART: case A_BULKRENAME: case A_MKDIR: case A_UNPACK:
        case A_TRASH: case A_COMMAND: case A_PACK: case A_SYNC: case A_PATRENAME: case A_CHECKSUMS: case A_MOUNT:
            say("a copy runs in the background - this waits for it (w shows it)", 1);
            return 0;
        case A_EDIT: case A_VIEW:
            if (cur->pl.arc) {
                say("a copy runs in the background - archives wait for it (w shows it)", 1);
                return 0;
            }
            break;
        case A_QUIT:
            if ((ask("a copy runs in the background - stop it and quit? (y/n)") | 32) != 'y') return 0;
            break;
        }
    }
    switch (act) {
    case A_DOWN: setsel(cur->sel + 1); break;
    case A_UP: setsel(cur->sel - 1); break;
    case A_HALFDOWN: setsel(cur->sel + listh / 2); break;
    case A_HALFUP: setsel(cur->sel - listh / 2); break;
    case A_PAGEDOWN: setsel(cur->sel + listh - 1); break;
    case A_PAGEUP: setsel(cur->sel - listh + 1); break;
    case A_TOP: setsel(0); break;
    case A_BOTTOM: setsel(cur->n - 1); break;
    case A_PARENT: parent(); break;
    case A_ENTER: enter(); break;
    case A_MARK: togglemark(); break;
    case A_INVERT:
        if (cur->pl.path[0]) {
            long i;
            for (i = 0; i < cur->n; i++) DENT(cur, i)->mark ^= 1;
            drawcols();
            drawstatus();
        }
        break;
    case A_UNMARKALL:
        {
            long i;
            for (i = 0; i < cur->n; i++) DENT(cur, i)->mark = 0;
            drawcols();
            drawstatus();
        }
        break;
    case A_VIEW: if (cur->n && !(dflags(cur, cur->sel) & F_DIR)) op_view(0); break;
    case A_EDIT: if (cur->n && !(dflags(cur, cur->sel) & F_DIR)) op_edit(); break;
    case A_UNPACK: if (cur->n && !(dflags(cur, cur->sel) & F_DIR)) op_extract(); break;
    case A_YANK: op_yank(0); break;
    case A_CUT: op_yank(1); break;
    case A_PASTE: op_paste(0); break;
    case A_PASTEOVER: op_paste(1); break;
    case A_UNYANK: op_unyank(); drawstatus(); break;
    case A_DELETE: op_delete(); break;
    case A_RENAME: op_rename(0); break;
    case A_RENAMEEXT: op_rename(1); break;
    case A_RENAMEEND: op_rename(2); break;
    case A_RENAMESTART: op_rename(3); break;
    case A_MKDIR:
        buf[0] = 0;
        if (editline("mkdir ", buf, 107, 0)) op_mkdir(buf);
        break;
    case A_SEARCH:
        search[0] = 0;              /* a new search starts empty (Ranger) */
        if (editline("/", search, sizeof(search) - 1, -1) && search[0] && cur->n) {
            if (!nchas(DNAME(cur, cur->sel), search)) searchnext(1);
        }
        break;
    case A_NEXT: searchnext(1); break;
    case A_PREV: searchnext(-1); break;
    case A_FILTER: live_filter(); break;
    case A_CLEARFILTER:
        if (visual_on()) {
            visual_end();           /* Esc ends visual mode first, the marks stay */
            drawstatus();
        } else if (filter[0]) {
            filter[0] = 0;
            reloadall(NULL);
        }
        break;
    case A_COMMAND:
        if (console()) return do_action(A_QUIT, NULL);
        break;
    case A_SHELL:
        buf[0] = 0;
        if (editline("!", buf, PATHMAX - 1, 0)) op_shell(buf);
        break;
    case A_NEWSHELL: op_newshell(); break;
    case A_RELOAD: reloadall(NULL); break;
    case A_HELP:
        {
            char *t = keys_help();
            if (t) {
                pager("Ahiru help", (unsigned char *)t, (long)strlen(t), TY_TEXT);
                xfree(t);
            }
            drawall();
        }
        break;
    case A_QUIT:
        if (ui_otherwindows()) {
            say("close the other windows on this screen first", 1);
            break;
        }
        hidenow = resident && !job_busy();     /* waiting for the hotkey: q hides */
        return 1;
    case A_TOGGLEINFO:
        hideinfo = !hideinfo;
        reloadall(NULL);
        say(hideinfo ? "icons hidden" : "icons shown", 0);
        break;
    case A_TOGGLEPREVIEW:
        showpv = !showpv;
        ui_layout();
        pv_invalidate();
        drawall();
        break;
    case A_COLOURS:
        {
            int r = reopen(colors == 4 ? 8 : colors == 8 ? 16 : 4);
            if (r < 0) return -1;
            if (r > 0) {
                char m[40];
                pv_invalidate();
                drawall();
                fmt(m, "%d colours", colors);
                say(m, 0);
            }
        }
        break;
    case A_SORT: dosort(arg); break;
    case A_GO: dogo(arg); break;
    case A_CD: gotopath(); break;
    case A_TABNEW: tab_new(); break;
    case A_TABCLOSE: tab_close(); break;
    case A_TABNEXT: tab_next(1); break;
    case A_TABPREV: tab_next(-1); break;
    case A_TAB: tab_go(arg); break;
    case A_VISUAL: visual_toggle(); break;
    case A_BULKRENAME: bulk_rename(); break;
    case A_FIND: find_run(0); break;
    case A_GREP: find_run(1); break;
    case A_TRASH: trash_run(); break;
    case A_PLAY: play_toggle(); break;
    case A_COPYPATH: clip_copy(0); break;
    case A_COPYNAME: clip_copy(1); break;
    case A_COPYDIR: clip_copy(2); break;
    case A_OPENWITH: open_with(); break;
    case A_HABIT: habit_jump(); break;
    case A_JOBS: job_show(); break;
    case A_COMPARE: tab_compare(0); break;
    case A_PATRENAME: fs_patrename(&cur->pl, DIDX(cur)); break;
    case A_CHECKSUMS: fs_checksums(&cur->pl, DIDX(cur)); break;
    case A_MOUNT: {
        /* an .adf into a drive and in, or out and back to its folder */
        char go[PATHMAX];
        const char *o = tab_other();
        if (fs_mount(&cur->pl, DIDX(cur), o ? o : "", go, sizeof(go))) {
            char m[160];
            strcpy(m, opmsg);
            godir(go, NULL);
            say(m, 0);
        }
        break;
    }
    case A_RUN: case A_LAUNCH:
        /* a command of the key file's, its {words} filled in */
        if (!arg) break;
        if (cur->pl.arc || !cur->pl.path[0]) say("commands run in a folder on a disk", 1);
        else if (!uc_expand(arg, &cur->pl, DIDX(cur), tab_other(), buf, sizeof(buf)))
            say("the command could not be filled in (no file here, too long, or a name with \" in it)", 1);
        else if (act == A_RUN) op_shell(buf);
        else if (!os_launch(cur->pl.path, buf)) showfault("could not start it");
        else {
            char m[120];
            fmt(m, "started: %.100s", buf);
            say(m, 0);
        }
        break;
    case A_SYNC: tab_compare(1); break;
    case A_PACK:
        /* into an archive in this same folder (the core asks the name) */
        if (cur->pl.arc) say("packing works in a folder on a disk", 1);
        else if (cur->n) arcw_pack(&cur->pl, &cur->pl, DIDX(cur));
        break;
    case A_LINEMODE:
        {
            static const char *const lm[] = { "sizes", "dates", "protection bits", "names only" };
            char m[40];
            linemode = (linemode + 1) & 3;
            drawcols();
            fmt(m, "showing %s", lm[linemode]);
            say(m, 0);
        }
        break;
    }
    return 0;
}

int do_action(int act, const char *arg)
{
    return run(act, arg);
}

/* a click: on an entry selects it, on the selected one again (quickly)
 * opens it; in the parent column goes up onto what was clicked; in the
 * preview of a folder goes into it */
static void mouseclick(int col, int row, ULONG secs, ULONG mics)
{
    static ULONG ls, lm;
    static long lidx = -1;
    long idx;
    if (row < 1 || row > listh) return;
    if (col >= c1x && col < c1x + c1w) {
        idx = cur->top + row - 1;
        if (idx >= cur->n) return;
        if (idx == cur->sel && idx == lidx && DoubleClick(ls, lm, secs, mics)) {
            lidx = -1;
            enter();
            return;
        }
        setsel(idx);
        lidx = idx, ls = secs, lm = mics;
    } else if (col >= c0x && col < c0x + c0w && results_on()) {
        parent();                   /* the hits end: back where they were found */
    } else if (col >= c0x && col < c0x + c0w && !parnone) {
        char name[110];
        idx = par->top + row - 1;
        if (idx >= par->n) return;
        strncpy(name, DNAME(par, idx), sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        goup();
        idx = dl_find(cur, name);
        if (idx >= 0) setsel(idx);
    } else if (showpv && col >= c2x && cur->n && (dflags(cur, cur->sel) & F_DIR)) {
        enter();
    }
}

/* the bookmark actions take the next key as their argument */
static int bookmarkkey(int act, int k)
{
    if (k <= 32 || k >= 128) return 0;
    if (act == A_BOOKMARK) {
        char m[60];
        free(bmark[k]);
        bmark[k] = strdup(cur->pl.path);
        savemarks();
        fmt(m, "bookmark %c set", k);
        say(m, 0);
    } else if (bmark[k]) {
        godir(bmark[k], NULL);
    } else {
        say("no such bookmark", 1);
    }
    return 0;
}

/* the drawer Workbench has active, or NULL */
static const char *wbdir(void)
{
    static char d[PATHMAX], canon[PATHMAX];
    if (wb_activedir(d, sizeof(d)) && isdirpath(d) == 1 && canonical(d, canon)) return canon;
    return NULL;
}

/* hidden: wait for the hotkey or Exchange. 1 = come up, 0 = quit */
static int waitpop(void)
{
    for (;;) {
        ULONG got = Wait(cx_sig() | SIGBREAKF_CTRL_C | rexx_sig());
        int ev;
        if (got & SIGBREAKF_CTRL_C) return 0;
        if ((got & rexx_sig()) && rexx_busy(1)) return 0;     /* ARexx QUIT */
        ev = cx_poll();
        if (ev == CX_QUIT) return 0;
        if (ev == CX_POPUP) return 1;
    }
}

static void mainloop(void)
{
    int pre = 0, pending = 0;
    for (;;) {
        UiEv ev;
        int k, act;
        const char *arg;
        if (rexx_handle()) return;      /* ARexx: QUIT came */
        if (job_sig() && (SetSignal(0, job_sig()) & job_sig()) && job_poll() && (pre || pending)) {
            /* the job asked or ended over a prefix panel: the panel goes,
             * and the next key starts afresh */
            pre = pending = 0;
            drawcols();
            pv_draw();
            drawstatus();
        }
        if ((k = injected()) != 0) {
            if (pvdirty) pv_compute();
            goto havekey;
        }
        if (uiresized) {            /* a new size while a question was up */
            uiresized = 0;
            ui_layout();
            drawall();
        }
        if (!ui_event(&ev)) {
            ULONG got;
            if (pvdirty) {          /* idle: now the preview */
                pv_compute();
                continue;
            }
            if (breakseen) return;  /* Ctrl-C reached a nested loop */
            mod_idle();             /* code not in use leaves memory while Ahiru waits */
            if (inject && !*inject) {       /* the played keys are done */
                ui_dumpgrid("RAM:Ahiru.grid");
                dbg("grid dumped");
                inject = NULL;
            }
            ui_flush();
            got = Wait(ui_sig() | SIGBREAKF_CTRL_C | rexx_sig() | job_sig() | cx_sig());
            if ((got & job_sig()) && job_poll() && (pre || pending)) {
                pre = pending = 0;      /* as above */
                drawcols();
                pv_draw();
                drawstatus();
            }
            if (got & cx_sig()) {
                int ev = cx_poll();
                if (ev == CX_POPUP) {
                    /* the hotkey while up: to the front, in Workbench's drawer */
                    const char *d = wbdir();
                    if (scr) ScreenToFront(scr);
                    if (d) godir(d, NULL);
                } else if (ev == CX_HIDE) {
                    if (ui_otherwindows()) say("close the other windows on this screen first", 1);
                    else if (!job_busy()) {
                        hidenow = 1;
                        return;
                    }
                    say("a copy runs in the background - Ahiru stays up until it is done", 1);
                } else if (ev == CX_QUIT) {
                    return;
                }
            }
            if (got & SIGBREAKF_CTRL_C) return;
            continue;
        }
        if (ev.type == EV_CLICK) {
            if (!pre && !pending) mouseclick(ev.col, ev.row, ev.secs, ev.mics);
            continue;
        }
        if (ev.type == EV_WHEEL) {     /* three rows a notch */
            setsel(cur->sel + 3 * ev.key);
            continue;
        }
        if (ev.type == EV_RESIZE) {
            ui_layout();
            drawall();
            continue;
        }
        if (ev.type == EV_CLOSE) return;
        if (ev.type != EV_KEY) continue;
        k = ev.key;
    havekey:
        if (msgup) {
            msgup = 0;
            drawstatus();
        }
        if (pre || pending) {
            /* the second key: take the panel away, then act */
            int p = pre, b = pending;
            pre = pending = 0;
            drawcols();
            pv_draw();
            drawstatus();
            if (k == 27) continue;
            if (b) {
                bookmarkkey(b, k);
                continue;
            }
            act = keys_find(CTX_MAIN, p, k, &arg);
        } else if (keys_isprefix(CTX_MAIN, k)) {
            /* a prefix: the panel says what can follow */
            static const char *items[2 * 40];
            static char lab[40][12], desc[40][48];
            int n = keys_hints(CTX_MAIN, k, items, 40, lab, desc);
            drawhints(items, n, 26);
            pre = k;
            continue;
        } else {
            act = keys_find(CTX_MAIN, k, 0, &arg);
        }
        if (act == A_BOOKMARK || act == A_JUMP) {
            if (act == A_JUMP) showbookmarks();
            else {
                static const char *const h[] = { "key", "bookmark this directory on that key" };
                drawhints(h, 1, 50);
            }
            pending = act;
            continue;
        }
        {
            char m2[40];
            fmt(m2, "key %d act %d", k, act);
            dbg(m2);
        }
        if ((act = run(act, arg)) != 0) return;
    }
}

/* ---- start and finish ----------------------------------------------------------- */

static void *wbmsg;             /* the WBStartup message (a Workbench start) */

static int smain(int argc, char **argv)
{
    char start[PATHMAX];
    int rc = 0, argmode = -1, keepmode;
    static char keysbad[120];
    dbg("start");
    loadconfig();
    keys_load(keysbad);             /* PROGDIR:Ahiru.keys, written when missing */
    habit_load();                   /* PROGDIR:Ahiru.places: the folders used */
    theme_load();                   /* PROGDIR:Ahiru.theme, written when missing */
    filecol_load();                 /* ENVARC:FileColours, csh's ls's file kinds */
    loadmarks();
    start[0] = 0;
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (strncmp(argv[i], "KEYS=", 5) == 0 || strncmp(argv[i], "keys=", 5) == 0)
                inject = argv[i] + 5;
            else if (nocase(argv[i], "LOG") == 0)
                logging = 1;
            else if (modeword(argv[i], -1) > UI_AUTO)
                argmode = modeword(argv[i], -1);   /* SCREEN, WINDOW, TERMINAL: this time */
            else
                strncpy(start, argv[i], PATHMAX - 1);
        }
    }
    fromwb = wbmsg != NULL;
    keepmode = cfgmode;             /* a word on the command line is not saved */
    if (argmode > UI_AUTO) cfgmode = argmode;
    if (start[0]) ;
    else if (savedir && lastdir[0]) strcpy(start, lastdir);
    else strcpy(start, cfgstart);
    start[PATHMAX - 1] = 0;
    cur = dl_new();
    par = dl_new();
    if (!os_init() || !cur || !par || !pv_init()) {
        PutStr((STRPTR)"Ahiru: out of memory\n");
        rc = 20;
        goto end;                   /* (everything there copes with not begun) */
    }
    /* where the archive and media code is: beside Ahiru, or inside it */
    mod_self(wbmsg ? 0 : argc, wbmsg ? (char **)wbmsg : argv);
    dbg("lists ok (preview buffer in)");
    {   /* a commodity (2.0+): the hotkey; a second Ahiru pops this one up */
        int dup = 0;
        if (wbmsg) cx_tooltypes(wbmsg, &cfgpopup, cfghotkey, sizeof(cfghotkey));
        if (ui_choose() == UI_TERM) goto nocx;  /* inside a terminal: no hotkey, no popping up another */
        cxon = cx_open("Ahiru", "Ahiru - file manager", "the hotkey opens it in Workbench's drawer",
                       cfghotkey, &dup);
        if (!cxon && dup) goto end;     /* the other one comes up; this one goes */
        resident = cxon && !cfgpopup;
    }
nocx:
    if (logging) ui_gridon(), dbg("text grid for LOG in (not there without LOG)");
    rexx_open();                    /* the ARexx port AHIRU (none: no rexxsyslib) */
    if (resident) {
        const char *d;
        if (!waitpop()) goto end;
        if ((d = wbdir())) strcpy(start, d);
    }
    for (;;) {
        if (!ui_open()) {
            dbg("ui_open failed");
            PutStr((STRPTR)"Ahiru: cannot open a screen or window\n");
            rc = 20;
            break;
        }
        {
            char canon[PATHMAX];
            if (start[0] && isdirpath(start) == 1 && canonical(start, canon))
                loaddir(canon, NULL);
            else if (canonical("SYS:", canon))
                loaddir(canon, NULL);
            else
                loaddir("", NULL);
        }
        dbg("dir loaded");
        pv_invalidate();
        drawall();
        if (keysbad[0]) say(keysbad, 1), keysbad[0] = 0;   /* a key line not understood: said, the rest work */
        else if (cxon && !cx_hotkeyok()) say("the HOTKEY in Ahiru.config was not understood", 1);
        dbg("drawn, entering loop");
        hidenow = 0;
        mainloop();
        dbg("loop left");
        ui_keepbox();                   /* the window's place, for the config */
        {
            int m = cfgmode;
            cfgmode = keepmode;
            saveconfig();
            cfgmode = m;
        }
        if (!hidenow) job_end();        /* stopped before the window it could use goes */
        if (win && ui_otherwindows()) say("close the other windows on this screen - then Ahiru goes", 1);
        if (win || uimode == UI_TERM) ui_close();
        if (hidenow && !breakseen && waitpop()) {
            /* the hotkey: back up, in Workbench's drawer (else where it was) */
            const char *d = wbdir();
            strcpy(start, d ? d : cur->pl.path);
            continue;
        }
        break;
    }
end:
    cx_close();
    mnt_exit();
    dl_free(cur);
    dl_free(par);
    op_unyank();
    job_end();                      /* a running copy is stopped and waited for */
    rexx_close();
    more_exit();
    more2_exit();
    pv_free();
    keys_free();
    mod_exit();
    arc_freetables();
    os_exit();
    return rc;
}

/* the stack trampoline, cdiff's: libnix ignores __stack, so main
 * swaps onto a 64K heap stack itself. Everything crossing the swap
 * lives in statics, and the swapped call takes no arguments. */
#define STACKSZ 65536
static struct StackSwapStruct sss;
static char *bigstk;
static int sargc, sret;
static char **sargv;

static void __attribute__((noinline)) runswapped(void)
{
    sret = smain(sargc, sargv);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    long have = (char *)me->tc_SPUpper - (char *)me->tc_SPLower;
    if (argc == 0) {                /* Workbench: no arguments */
        wbmsg = argv;
        static char *noargs[] = { "Ahiru", NULL };
        argc = 1;
        argv = noargs;
    }
    if (me->tc_SPLower == NULL || me->tc_SPUpper == NULL || have >= 32768)
        return smain(argc, argv);
    sargc = argc;
    sargv = argv;
    if (!(bigstk = malloc(STACKSZ))) return smain(argc, argv);
    sss.stk_Lower = bigstk;
    sss.stk_Upper = (ULONG)(bigstk + STACKSZ);
    sss.stk_Pointer = (APTR)(bigstk + STACKSZ);
    StackSwap(&sss);
    runswapped();
    StackSwap(&sss);
    free(bigstk);
    return sret;
}
