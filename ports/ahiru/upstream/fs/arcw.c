/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* arcw - see arcw.h. Every change to an archive is a checked commit:
 *
 *   1. new members are staged in a scratch folder beside the archive
 *      (same volume: the room is there, nothing goes through RAM:)
 *   2. the archive is copied beside itself and the tool works on the copy
 *   3. CFile's own reader lists the copy: every member that should be
 *      there is, with its size, and every one that should be gone is gone
 *   4. only then does the copy replace the archive (swapin)
 *
 * lha and lzx return 0 after SKIPPING a name already in the archive, so
 * a code proves nothing (the E CFile's b52 lesson): old members are
 * deleted first, and step 3 is what decides. A tool that fails, a full
 * disk, a damaged result - the archive stays as it was.
 *
 * LhA cannot delete a directory member, and no tool renames: those are
 * a rebuild - everything unpacked by our reader, changed, packed afresh.
 */
#include <string.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include "arcw.h"
#include "arcfs.h"
#include "place.h"
#include "fe.h"
#include "os.h"
#include "../core/core.h"
#include "../core/util.h"
#include "../core/arc.h"

#define CMDBUF 1024

/* arcw_putfile's one file: staged from this path instead of a pane */
static const char *onesrc;
#define MAXDEPTH 20

/* ---- running a tool ----------------------------------------------------
 * Through Execute() on every Kickstart (SystemTagList is V36), from a
 * two-line script beside the archive: the tool, then an Echo that marks
 * the end. A tool failing (code 10 or more) stops the script before the
 * mark - the default FailAt - so no FailAt, If or $RC is needed (a bare
 * 1.3 disk has none of them; Echo and Execute it has). A warning (5)
 * reaches the mark: the read-back check is what decides. Input and
 * output are NIL:, so a tool that would ask a question gets an answer. */

static int cmdmax(void)
{
    return v_dos >= 36 ? 900 : 240;     /* the 1.3 command line is 255 */
}

/* the packing bar: while a tool adds files, its output is read as it
 * comes and each file it names moves the bar by that file's size (the
 * E CFile moved it per member; the sizes make it even). Set by the
 * callers around an add; 1.3 runs the tool in the foreground: the bar
 * moves when it ends. */
static int toolprog;
static void credit(const char *line, int len);
static void creditrest(void);

static int runtool(const char *cwd, const char *cmd, const char *near)
{
    char scr[CPATH], dn[CPATH], lg[CPATH], *txt;
    BPTR fh, lk, old, nin, nout, rfh = 0;
    int r = 20;
    LONG l, got = 0;
    sidename(scr, near, 's');
    sidename(dn, near, 'k');
    sidename(lg, near, 'l');
    DeleteFile((STRPTR)dn);
    DeleteFile((STRPTR)lg);
    if (!(txt = xalloc(CMDBUF + 2 * CPATH + 40))) return 20;
    sfmt(txt, CMDBUF + 2 * CPATH + 40, "%s\nEcho >\"%s\" \"d\"\n", cmd, dn);
    l = strlen(txt);
    if ((fh = Open((STRPTR)scr, MODE_NEWFILE))) {
        if (Write(fh, txt, l) == l) r = 0;
        Close(fh);
    }
    if (r == 0 && (lk = Lock((STRPTR)cwd, ACCESS_READ))) {
        nin = Open((STRPTR)"NIL:", MODE_OLDFILE);
#ifdef TOOLLOG
        {   /* debug builds: each run's output beside the archive */
            static int runs;
            char lg[CPATH];
            sidename(lg, near, (char)('0' + runs++ % 10));
            strcat(lg, ".log");
            nout = Open((STRPTR)lg, MODE_NEWFILE);
            if (nout) Write(nout, (APTR)cmd, strlen(cmd)), Write(nout, "\n", 1);
        }
#else
        /* with the bar: the output into a file CFile reads as it grows
         * (shared, as the console does); else NIL: */
        nout = toolprog && v_dos >= 36 ? Open((STRPTR)lg, MODE_READWRITE) : Open((STRPTR)"NIL:", MODE_NEWFILE);
#endif
        old = CurrentDir(lk);
        sfmt(txt, CPATH + 16, "Execute \"%s\"", scr);
        if (v_dos >= 36 && nin && nout) {
            /* from 2.0 detached, so Esc reaches it: a break (CTRL-C) to
             * its process, found by a unique name; done when it is gone.
             * lha and lzx rebuild into a temp and rename at the end, and
             * our copy is all they touch - a break changes nothing. */
            char pname[32];
            int x;
            sfmt(pname, sizeof(pname), "CFile archiver %lx", (ULONG)FindTask(NULL));
            if (SystemTags((STRPTR)txt, SYS_Input, nin, SYS_Output, nout, SYS_Asynch, TRUE, NP_Name, (ULONG)pname,
                           TAG_DONE) == 0) {
                nin = nout = 0;                 /* the child closes them */
                for (;;) {
                    Delay(5);
                    if (toolprog) {
                        /* new output: a fresh handle each time (a RAM-disk
                         * handle keeps the size it first saw) */
                        static UBYTE buf[512];
                        static char line[256];
                        static int ll, esc;
                        LONG n, i;
                        if (!got) ll = esc = 0;
                        if ((rfh = Open((STRPTR)lg, MODE_OLDFILE))) {
                            Seek(rfh, got, OFFSET_BEGINNING);
                            while ((n = Read(rfh, buf, sizeof(buf))) > 0) {
                                got += n;
                                for (i = 0; i < n; i++) {
                                    int c = buf[i];
                                    /* LhA draws with CSI codes (a name is
                                     * followed by 9B 'K', the byte count
                                     * by 9B "14C"): they are dropped */
                                    if (esc == 1) {
                                        esc = c == '[' ? 2 : 0;
                                        continue;
                                    }
                                    if (esc == 2) {
                                        if (c >= 0x40 && c <= 0x7E) esc = 0;
                                        continue;
                                    }
                                    if (c == 0x1B) esc = 1;
                                    else if (c == 0x9B) esc = 2;
                                    else if (c == 10 || c == 13 || c == 8) {
                                        /* LZX backs over its "%NN" with
                                         * backspaces: each one counts */
                                        if (ll) credit(line, ll);
                                        ll = 0;
                                    } else if (ll < (int)sizeof(line) - 1) {
                                        line[ll++] = (char)c;
                                    }
                                }
                            }
                            Close(rfh);
                            rfh = 0;
                        }
                    }
                    if (escpressed()) {
                        struct Task *t;
                        Forbid();
                        if ((t = FindTask((STRPTR)pname))) Signal(t, SIGBREAKF_CTRL_C);
                        Permit();
                        abortop = 1;
                    }
                    Forbid();
                    x = FindTask((STRPTR)pname) != NULL;
                    Permit();
                    if (!x) break;
                }
            }
        } else {
            Execute((STRPTR)txt, nin, nout);
        }
        CurrentDir(old);
        UnLock(lk);
        if (nin) Close(nin);
        if (nout) Close(nout);
        r = os_pathtype(dn) == 1 && !abortop ? 0 : 20;
    } else {
        r = 20;
    }
    xfree(txt);
    DeleteFile((STRPTR)scr);
    DeleteFile((STRPTR)dn);
    DeleteFile((STRPTR)lg);
    return r;
}

/* ---- command lines, filled and run in batches ------------------------- */

struct batch {
    char *cmd;
    int base, n, lzx;
    const char *cwd, *near;
    int rc;                     /* the worst code of the runs */
};

/* a name in AmigaDOS quotes, where * is the escape (** is a star); for
 * lzx d the pattern characters get its ' escape first (the E lzxesc) */
static int qadd(char *d, int max, const char *name, int lzx)
{
    int n = strlen(d), n0 = n;
    const char *s;
    d[n++] = ' ';
    d[n++] = '"';
    if (strchr(name, '"')) return -1;  /* no way to quote it for every shell */
    for (s = name; *s; s++) {
        int c = (unsigned char)*s;
        if (n + 5 >= max) {
            d[n0] = 0;
            return 0;
        }
        if (lzx && strchr("'#?%*()|[]~", c)) d[n++] = '\'';
        if (c == '*') d[n++] = '*';
        d[n++] = (char)c;
    }
    d[n++] = '"';
    d[n] = 0;
    return 1;
}

static int bstart(struct batch *b, const char *base, const char *cwd, const char *near, int lzx)
{
    if (!(b->cmd = xalloc(CMDBUF))) return 0;
    sfmt(b->cmd, CMDBUF, "%s", base);
    b->base = strlen(b->cmd);
    b->n = 0;
    b->lzx = lzx;
    b->cwd = cwd;
    b->near = near;
    b->rc = 0;
    return 1;
}

static int bflush(struct batch *b)
{
    int r;
    if (!b->n) return 1;
    r = runtool(b->cwd, b->cmd, b->near);
    if (r > b->rc) b->rc = r;
    b->cmd[b->base] = 0;
    b->n = 0;
    return r < 20;
}

static int badd(struct batch *b, const char *name)
{
    int r = qadd(b->cmd, cmdmax(), name, b->lzx);
    if (r < 0) {
        sfmt(opmsg, sizeof(opmsg), "a name with \" in it cannot go to the archiver: %s", name);
        return 0;
    }
    if (r) {
        b->n++;
        return 1;
    }
    if (!b->n || !bflush(b) || !qadd(b->cmd, cmdmax(), name, b->lzx)) return 0;
    b->n++;
    return 1;
}

static int bend(struct batch *b)
{
    int ok = bflush(b);
    xfree(b->cmd);
    b->cmd = NULL;
    return ok && b->rc < 20;
}

/* ---- what the result must hold ------------------------------------------ */

struct want {
    char *name;
    ULONG size;
    UBYTE gone;                 /* must NOT be in the result */
    UBYTE seen;
    UBYTE repl;                 /* replaced: the old member is deleted first */
    UBYTE fix;                  /* days/mins known: the date to check (zip) */
    UBYTE patch;                /* stored a day late: put days/mins in */
    UBYTE hascrc;               /* crc known (a staged file): the contents are checked too */
    LONG days, mins, ticks;
    ULONG crc;
    int hnext;                  /* the next want in its hash chain, -1 = none */
    UBYTE credited;             /* the packing bar has counted it */
};

static int wkind;               /* the archive kind being checked (which CRC it stores) */

static struct want *wv;
static int wn, wcap;
static struct pool *wpl;
static char wbad[120];

/* names found through a small hash (case folded): thousands of members
 * were O(n^2) compares - minutes on a 68000 (the audit) */
#define WH 256
static int whead[WH];

static unsigned hashname(const char *s)
{
    unsigned h = 0;
    int c;
    while ((c = (unsigned char)*s++)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 32;
        h = h * 31 + c;
    }
    return h & (WH - 1);
}

static void wanthash(void)
{
    int i;
    for (i = 0; i < WH; i++) whead[i] = -1;
    for (i = 0; i < wn; i++) {
        unsigned h = hashname(wv[i].name);
        wv[i].hnext = whead[h];
        whead[h] = i;
    }
}

static void wantfree(void)
{
    xfree(wv);
    wv = NULL;
    wn = wcap = 0;
    pool_free(&wpl);
    wanthash();
}

static struct want *wantfind(const char *name)
{
    int i;
    for (i = wn ? whead[hashname(name)] : -1; i >= 0 && i < wn; i = wv[i].hnext)
        if (nocase(wv[i].name, name) == 0) return &wv[i];
    return NULL;
}

static struct want *wantadd(const char *name, ULONG size)
{
    struct want *w;
    if (wn == wcap) {
        int nc = wcap ? wcap * 2 : 128;
        struct want *nw = xalloc(nc * sizeof(struct want));
        if (!nw) return NULL;
        if (wn) memcpy(nw, wv, wn * sizeof(struct want));
        xfree(wv);
        wv = nw;
        wcap = nc;
    }
    w = &wv[wn];
    if (!(w->name = pool_str(&wpl, name))) return NULL;
    {
        unsigned h = hashname(name);
        w->hnext = whead[h];
        whead[h] = wn;
    }
    w->size = size;
    w->gone = w->seen = w->repl = w->fix = w->patch = w->hascrc = w->credited = 0;
    wn++;
    return w;
}

/* a line of the tool's output: every word that names a staged file it
 * has not named before moves the bar by that file's size. lha may print
 * a member without its folder, so a bare name matches the first
 * uncounted staged file of that name. */
static struct want *curw;       /* the file the tool is packing now */
static int zipline;             /* this line is zip's "adding: name" */
static ULONG curdone;           /* its bytes the bar has counted */

/* the file being packed is done: the rest of its bytes count */
static void finishcur(void)
{
    if (curw && curw->size > curdone) progadd(curw->size - curdone);
    curw = NULL;
    curdone = 0;
}

static void credit(const char *line, int len)
{
    char word[256];
    int i = 0, n, j;
    ULONG num = 0;
    /* a line of just a number: LhA's bytes done of the file it packs -
     * the bar creeps inside a big file */
    for (j = 0; j < len && (line[j] == ' ' || (line[j] >= '0' && line[j] <= '9')); j++)
        if (line[j] != ' ') num = num * 10 + (ULONG)(line[j] - '0');
    if (j == len && num) {
        if (curw && num > curdone && num <= curw->size) {
            progadd(num - curdone);
            curdone = num;
        }
        return;
    }
    /* zip names a file once it has packed it: count it whole */
    for (j = 0; j + 7 <= len; j++)
        if (!strncmp(line + j, "adding:", 7)) break;
    if (j + 7 <= len) zipline = 1;
    else zipline = 0;
    while (i < len) {
        while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '(' || line[i] == ')')) i++;
        for (n = 0; i < len && line[i] != ' ' && line[i] != '\t' && n < 255; i++) word[n++] = line[i];
        word[n] = 0;
        if (n < 1) continue;
        if (word[0] == '%' && word[1] >= '0' && word[1] <= '9') {
            /* LZX's "%NN" of the file it packs */
            ULONG pc = 0, want;
            for (j = 1; word[j] >= '0' && word[j] <= '9'; j++) pc = pc * 10 + (ULONG)(word[j] - '0');
            if (curw && pc <= 100) {
                want = curw->size / 100 * pc + curw->size % 100 * pc / 100;
                if (want > curdone) progadd(want - curdone), curdone = want;
            }
            continue;
        }
        {
            struct want *w = wantfind(word);
            if (!w || !w->fix || w->credited)
                for (w = NULL, j = 0; j < wn; j++)
                    if (wv[j].fix && !wv[j].credited && !nocase(filepart(wv[j].name), word)) {
                        w = &wv[j];
                        break;
                    }
            if (w && w->fix && !w->credited) {
                /* the tool names a new file: the last one is done */
                finishcur();
                w->credited = 1;
                curw = w;
                curdone = 0;
                if (zipline) finishcur();       /* zip: already packed */
                else continue;                  /* lzx: "%NN" may follow on the line */
                return;
            }
        }
    }
}

/* the tool is done: what it named in a way we did not catch counts now */
static void creditrest(void)
{
    int j;
    finishcur();
    for (j = 0; j < wn; j++)
        if (wv[j].fix && !wv[j].credited) {
            wv[j].credited = 1;
            progadd(wv[j].size);
        }
}

/* the bytes the packing will count (the staged files) */
static ULONG packbytes(void)
{
    int j;
    ULONG t = 0;
    for (j = 0; j < wn; j++)
        if (wv[j].fix) t += wv[j].size;
    return t;
}

/* every file member of pane p's archive, as it is now */
static int wantcurrent(struct place *p)
{
    int i, n = arcfs_nmem(p), isdir;
    ULONG size;
    for (i = 0; i < n; i++) {
        const char *m = arcfs_mem(p, i, &size, &isdir);
        if (!isdir && !wantadd(m, size)) return 0;
    }
    return 1;
}

static int wantcb(void *ctx, const struct arcent *e)
{
    struct want *w;
    (void)ctx;
    if (e->isdir || !(w = wantfind(e->name))) return 1;
    if (w->gone) {
        if (!wbad[0]) sfmt(wbad, sizeof(wbad), "\"%s\" is still in it", e->name);
    } else if (w->seen) {
        /* twice (a zip keeps Foo and foo apart): which one is read? */
        if (!wbad[0]) sfmt(wbad, sizeof(wbad), "\"%s\" is in it twice", e->name);
    } else if (w->size != e->size) {
        if (!wbad[0]) sfmt(wbad, sizeof(wbad), "\"%s\" has the wrong size", e->name);
    } else if (w->hascrc && w->crc != (wkind == AK_LHA ? (e->aux1 & 0xFFFF) : wkind == AK_LZX ? e->aux3 : e->aux1)) {
        /* a same-size old member left where the new one should be */
        if (!wbad[0]) sfmt(wbad, sizeof(wbad), "\"%s\" has the old contents", e->name);
    } else {
        w->seen = 1;
        /* Zip 2.0.1 (Amiga) stores every date from March 2000 on a day
         * late (a 2000 leap-year slip): such a member is put right */
        if (w->fix && e->days == w->days + 1 && e->mins == w->mins) w->patch = 1;
    }
    return 1;
}

/* step 3: our own reader over the new archive */
static int verify(const char *file, int kind)
{
    int i, r;
    wbad[0] = 0;
    wkind = kind;
    for (i = 0; i < wn; i++) wv[i].seen = 0;
    if ((r = arcfs_listfile(file, kind, wantcb, NULL))) {
        sfmt(wbad, sizeof(wbad), "it does not read back (%s)", arc_errstr(r));
        return 0;
    }
    for (i = 0; i < wn && !wbad[0]; i++)
        if (!wv[i].gone && !wv[i].seen) sfmt(wbad, sizeof(wbad), "\"%s\" is missing", wv[i].name);
    return !wbad[0];
}

/* ---- putting Zip 2.0.1's dates right ------------------------------------ */

static ULONG z16(const UBYTE *b) { return b[0] | (ULONG)b[1] << 8; }
static ULONG z32(const UBYTE *b) { return z16(b) | z16(b + 2) << 16; }

/* an AmigaDOS datestamp as MS-DOS time (low word) and date (high) */
static ULONG dosdate(LONG days, LONG mins, LONG ticks)
{
    static const UBYTE ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int y = 1978, m = 0, n;
    for (;;) {
        n = (y & 3) ? 365 : 366;
        if (days < n) break;
        days -= n;
        y++;
    }
    for (;;) {
        n = ml[m] + (m == 1 && !(y & 3));
        if (days < n || m == 11) break;
        days -= n;
        m++;
    }
    if (y < 1980) return 0;
    return ((ULONG)((y - 1980) << 9 | (m + 1) << 5 | (days + 1)) << 16) |
           (ULONG)((mins / 60) << 11 | (mins % 60) << 5 | (ticks / 50) / 2);
}

/* the marked members' dates, in the central directory and the local
 * headers (no checksum covers them) */
static void zippatch(const char *file)
{
    BPTR fh;
    UBYTE *buf, h[46];
    LONG size, tail, i, pos, end;
    ULONG cd, cnt, k, nl, xl, cl, lh, dd;
    char nm[256];
    struct want *w;
    int any = 0;
    for (i = 0; i < wn; i++) any |= wv[i].patch;
    if (!any || !(buf = xalloc(4096))) return;
    if (!(fh = Open((STRPTR)file, MODE_OLDFILE))) {
        xfree(buf);
        return;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    /* the end record: Zip 2.0.1 writes no archive comment unless asked,
     * so it sits in the last few hundred bytes */
    tail = size < 4096 ? size : 4096;
    if (Seek(fh, size - tail, OFFSET_BEGINNING) < 0 || Read(fh, buf, tail) != tail) goto out;
    for (i = tail - 22; i >= 0; i--)
        if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 5 && buf[i + 3] == 6) break;
    if (i < 0) goto out;
    cnt = z16(buf + i + 10);
    cd = z32(buf + i + 16);
    end = size - tail + i;
    for (pos = cd, k = 0; k < cnt && pos + 46 <= end; k++) {
        if (Seek(fh, pos, OFFSET_BEGINNING) < 0 || Read(fh, h, 46) != 46) break;
        if (h[0] != 'P' || h[1] != 'K' || h[2] != 1 || h[3] != 2) break;
        nl = z16(h + 28);
        xl = z16(h + 30);
        cl = z16(h + 32);
        lh = z32(h + 42);
        if (nl > 255 || Read(fh, nm, nl) != (LONG)nl) break;
        nm[nl] = 0;
        if ((w = wantfind(nm)) && w->patch && (dd = dosdate(w->days, w->mins, w->ticks))) {
            UBYTE t[4];
            t[0] = (UBYTE)dd;
            t[1] = (UBYTE)(dd >> 8);
            t[2] = (UBYTE)(dd >> 16);
            t[3] = (UBYTE)(dd >> 24);
            if (Seek(fh, pos + 12, OFFSET_BEGINNING) >= 0) Write(fh, t, 4);
            if (Seek(fh, lh + 10, OFFSET_BEGINNING) >= 0) Write(fh, t, 4);
        }
        pos += 46 + nl + xl + cl;
    }
out:
    Close(fh);
    xfree(buf);
}

/* ---- small things ------------------------------------------------------- */

static const char *toolname(int k)
{
    return k == AK_LHA ? "lha" : k == AK_LZX ? "lzx" : "zip";
}

static int toolthere(int k)
{
    char c[12];
    int t;
    sfmt(c, sizeof(c), "C:%s", toolname(k));
    os_quiet(1);
    t = os_pathtype(c);
    os_quiet(0);
    if (t == 1) return 1;
    sfmt(opmsg, sizeof(opmsg), "writing %s archives needs C:%s - it is not there", toolname(k), toolname(k));
    showmsg(opmsg);
    return 0;
}

/* the start of every tool line; the archive name follows */
static void addline(char *d, int k, const char *arc)
{
    if (k == AK_LHA) sfmt(d, CPATH + 32, "lha -M -x -r -e a \"%s\"", arc);
    else if (k == AK_LZX) sfmt(d, CPATH + 32, "lzx -r -e a \"%s\"", arc);
    else sfmt(d, CPATH + 32, "zip -r \"%s\"", arc);     /* not -q: its "adding:" lines move the bar */
}

/* a path that still means the same after a CurrentDir: from its lock */
static int abspath(const char *path, char *dst)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    int ok;
    if (!l) return 0;
    ok = os_lockname(l, dst, CPATH);
    UnLock(l);
    return ok;
}

/* the scratch copy keeps the archive's extension: LZX adds ".lzx" to
 * a name without it and would write a file nobody checks */
static void copyname(char *copy, const char *arc)
{
    const char *f = filepart(arc), *dot = strrchr(f, '.');
    sidename(copy, arc, 'a');
    if (dot && strlen(copy) + strlen(dot) < CPATH) strcat(copy, dot);
}

/* the archive's absolute name, and its scratch names beside it */
static int names3(const char *file, char *arc, char *stage, char *copy, int tag)
{
    stage[0] = copy[0] = 0;             /* nothing to tidy unless set below */
    if (!abspath(file, arc)) {
        sfmt(opmsg, sizeof(opmsg), "cannot find the archive file");
        return 0;
    }
    sidename(stage, arc, tag);
    copyname(copy, arc);
    wipetree(stage);                    /* a leftover of ours, if any */
    DeleteFile((STRPTR)copy);
    return 1;
}

/* make dir and every folder of sub inside it; dst = the deepest */
static int mkpath(const char *dir, const char *sub, char *dst)
{
    char part[110];
    BPTR l;
    strcpy(dst, dir);
    while (*sub) {
        int n = 0;
        while (*sub == '/') sub++;
        while (*sub && *sub != '/' && n < 107) part[n++] = *sub++;
        part[n] = 0;
        if (!n) break;
        if (!addpart(dst, part, CPATH)) return 0;
        if ((l = CreateDir((STRPTR)dst))) UnLock(l);
        else if (os_pathtype(dst) != 2) return 0;
    }
    return 1;
}

/* a staged file's checksum the way the archive kind stores it: lha
 * CRC-16, zip and lzx CRC-32 (wkind is set by the caller) */
static int filecrc(const char *path, ULONG *crc)
{
    BPTR fh;
    UBYTE *b;
    LONG n;
    ULONG c = 0;
    if (!(b = xalloc(4096))) return 0;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) {
        xfree(b);
        return 0;
    }
    while ((n = Read(fh, b, 4096)) > 0)
        c = wkind == AK_LHA ? arc_crc16(c, b, n) : arc_crc32(c, b, n);
    Close(fh);
    xfree(b);
    if (n < 0) return 0;
    *crc = c;
    return 1;
}

/* the staged files, as the archive will name them: add each to the
 * wants (an existing name is a replacement: its old member goes first) */
static int stagewalk(const char *dir, char *rel, int depth)
{
    struct dscan *d;
    struct dent e;
    char *child;
    int rl = strlen(rel), ok = 1, r;
    if (depth > MAXDEPTH || !(d = ds_open(dir))) return 0;
    if (!(child = xalloc(CPATH))) {
        ds_close(d);
        return 0;
    }
    while (ok && (r = ds_next(d, &e)) > 0) {
        if (rl + strlen(e.name) + 2 > CPATH || !buildfull(child, dir, e.name)) {
            ok = 0;
            break;
        }
        if (rl) sfmt(rel + rl, CPATH - rl, "/%s", e.name);
        else strcpy(rel, e.name);
        if (e.type > 0) {
            ok = stagewalk(child, rel, depth + 1);
        } else {
            struct want *w = wantfind(rel);
            if (w) {
                w->size = e.size;
                w->repl = 1;
            } else if (!(w = wantadd(rel, e.size))) {
                ok = 0;
            }
            if (w) {
                w->fix = 1;
                w->days = e.days;
                w->mins = e.mins;
                w->ticks = 0;
                w->hascrc = filecrc(child, &w->crc);
            }
        }
        rel[rl] = 0;
    }
    if (r < 0) ok = 0;
    ds_close(d);
    xfree(child);
    return ok;
}

/* every top-level entry of dir onto the batch (the tool runs in dir,
 * so the archive stores clean relative paths) */
static int addtops(struct batch *b, const char *dir)
{
    struct dscan *d;
    struct dent e;
    int ok = 1, r, any = 0;
    if (!(d = ds_open(dir))) return 0;
    while (ok && (r = ds_next(d, &e)) > 0) {
        ok = badd(b, e.name);
        any = 1;
    }
    if (r < 0) ok = 0;
    ds_close(d);
    return ok && any;
}

/* delete the replaced/removed members from the copy */
static int delmembers(int k, const char *copy, const char *cwd, int all)
{
    struct batch b;
    char base[CPATH + 32];
    int i, ok = 1;
    if (k == AK_LHA) sfmt(base, sizeof(base), "lha -M -x d \"%s\"", copy);
    else if (k == AK_LZX) sfmt(base, sizeof(base), "lzx d \"%s\"", copy);
    else sfmt(base, sizeof(base), "zip -d -q \"%s\"", copy);
    if (!bstart(&b, base, cwd, copy, k == AK_LZX)) return 0;
    for (i = 0; i < wn && ok; i++)
        if (wv[i].repl || (all && wv[i].gone)) ok = badd(&b, wv[i].name);
    return bend(&b) && ok;
}

/* zip 2.0.1 reads a name to delete as a pattern and cannot be told not to */
static int zipsafe(const char *name)
{
    return !strpbrk(name, "*?[]");
}

/* step 4, and the tidying either way */
static int commit(int ok, const char *copy, const char *arc, const char *stage)
{
    if (ok && !swapin(copy, arc, 1)) {
        sfmt(opmsg, sizeof(opmsg), "cannot replace the archive (write-protected?) - it is unchanged");
        ok = 0;
    }
    if (!ok && copy[0]) DeleteFile((STRPTR)copy);
    if (stage[0]) wipetree(stage);
    return ok;
}

static void dirof(const char *path, char *dst)
{
    int n = pathlen(path);
    memcpy(dst, path, n);
    dst[n] = 0;
}

/* ---- the operations ---------------------------------------------------- */

typedef int (*editfn)(const char *stage, void *ctx);
static int rebuild(struct place *p, editfn edit, void *ctx);

/* the files of one tree moved over another (same volume: Renames),
 * an existing file replaced - the add's stage laid over a rebuild's */
static int overlay(const char *from, const char *to, int depth)
{
    struct dscan *d;
    struct dent e;
    char *a, *b;
    BPTR l;
    int ok = 1, r;
    if (depth > MAXDEPTH || !(d = ds_open(from))) return 0;
    a = xalloc(CPATH);
    b = xalloc(CPATH);
    if (!a || !b) ok = 0;
    while (ok && (r = ds_next(d, &e)) > 0) {
        if (!buildfull(a, from, e.name) || !buildfull(b, to, e.name)) {
            ok = 0;
            break;
        }
        if (e.type > 0) {
            if ((l = CreateDir((STRPTR)b))) UnLock(l);
            ok = os_pathtype(b) == 2 && overlay(a, b, depth + 1);
        } else {
            if (os_pathtype(b) == 1) wipetree(b);
            ok = Rename((STRPTR)a, (STRPTR)b);
        }
    }
    ds_close(d);
    xfree(a);
    xfree(b);
    return ok;
}

static int overstage(const char *stage, void *ctx)
{
    return overlay((const char *)ctx, stage, 0);
}

int arcw_canwrite(struct place *p)
{
    int k = arcfs_kindof(p);
    return pl_inarc(p) && (k == AK_LHA || k == AK_LZX || k == AK_ZIP) && !arcfs_damaged(p);
}

/* one real file into archive pane q as name (in the pane's folder):
 * the editor's way back into an archive. 1 = the archive took it */
int arcw_putfile(struct place *q, const char *src, const char *name)
{
    char tn[1][NAMEMAX + 4];
    int idx[1] = { 0 }, ok;
    sfmt(tn[0], sizeof(tn[0]), "%s", name);
    onesrc = src;
    ok = arcw_add(NULL, q, idx, tn, 1, 0);
    onesrc = NULL;
    return ok;
}

int arcw_has(struct place *p, const char *name)
{
    int i = place_find(p, name);
    return i < 0 ? 0 : p->ent[i].kind == KD_FILE ? 1 : 2;
}

int arcw_add(struct place *p, struct place *q, const int *idx, char (*tn)[NAMEMAX + 4], int n, int ismove)
{
    int k = arcfs_kindof(q), s, ok = 1, showbar;
    /* static: arcw_add is never re-entered, and these 2.5K on the stack
     * sat under a rebuild and a recursive wipe (the 1.3 stack audit) */
    static char arc[CPATH], stage[CPATH], copy[CPATH], dst[CPATH], src[CPATH], dir[CPATH];
    static char rel[CPATH], line[CPATH + 32], iname[NAMEMAX + 8], itname[NAMEMAX + 8];
    char *icok = NULL;                  /* per entry: its icon was staged */
    struct batch b;
    ULONG total = 0;
    LONG asz;
    struct place *P = onesrc ? NULL : p;
    if (!toolthere(k)) return 0;
    opmsg[0] = 0;
    if (!onesrc) {
        /* the archive itself among the sources, or a folder that holds
         * it: the move would delete the new archive, and the stage
         * would grow inside what it copies (the audit) */
        if (!abspath(arcfs_file(q), arc)) arc[0] = 0;
        for (s = 0; s < n && arc[0]; s++) {
            struct ent *e = &P->ent[idx[s]];
            if (pl_inarc(p)) {
                if (nocase(arcfs_file(p), arcfs_file(q)) == 0 && ismove) {
                    showmsg("source and target are the same archive - rename inside it instead");
                    return 0;
                }
                continue;
            }
            if (!buildfull(src, P->path, e->name)) continue;
            if (insidedir(src, arc)) {
                showmsg("the archive itself is among what would go into it");
                return 0;
            }
        }
        if (!(icok = xalloc(n + 1))) {
            showmsg("not enough memory");
            return 0;
        }
    }
    if (!names3(arcfs_file(q), arc, stage, copy, 'w')) {
        showmsg(opmsg);
        xfree(icok);
        return 0;
    }
    {
        BPTR l = CreateDir((STRPTR)stage);
        if (l) UnLock(l);
    }
    if (os_pathtype(stage) != 2 || !mkpath(stage, arcfs_sub(q), dir)) {
        wipetree(stage);
        showmsg("cannot make the scratch folder beside the archive");
        xfree(icok);
        return 0;
    }
    if (!cbuf_get()) {
        wipetree(stage);
        showmsg("not enough memory");
        xfree(icok);
        return 0;
    }
    /* the bar: the bytes staged, then the archive copied */
    asz = 0;
    {
        struct dent e;
        struct dscan *d;
        char adir[CPATH];
        dirof(arc, adir);
        if ((d = ds_open(adir))) {
            while (ds_next(d, &e) > 0)
                if (nocase(e.name, filepart(arc)) == 0) asz = (LONG)e.size;
            ds_close(d);
        }
    }
    statbytes = 0;
    for (s = 0; s < n && !onesrc; s++) {
        struct ent *e = &P->ent[idx[s]];
        if (e->kind == KD_FILE) statbytes += e->size;
        else if (pl_inarc(p)) statbytes += arcfs_treesize(p, e->name);
        else if (buildfull(src, P->path, e->name)) treestat(src, 0);
    }
    total = statbytes * 2 + (ULONG)asz;    /* staged, the archive copied, then packed */
    cancelok = 1;
    abortop = 0;
    showbar = 1;
    progshow(total);
    /* 1: stage */
    if (onesrc) {
        if (!buildfull(dst, dir, tn[0])) ok = 0, sfmt(opmsg, sizeof(opmsg), "the path is too long");
        else ok = copyfile(onesrc, dst);
    }
    if (!onesrc && pl_inarc(p)) arcfs_hold(p);     /* one session for the whole stage */
    for (s = 0; s < n && ok && !abortop && !onesrc; s++) {
        struct ent *e = &P->ent[idx[s]];
        int isdir = e->kind != KD_FILE;
        if (!buildfull(dst, dir, tn[s]) || (!pl_inarc(p) && !buildfull(src, P->path, e->name))) {
            sfmt(opmsg, sizeof(opmsg), "the path is too long");
            ok = 0;
            break;
        }
        if (pl_inarc(p)) ok = arcfs_copyout(p, e->name, dst, isdir);
        else ok = isdir ? copytree(src, dst, 0) : copyfile(src, dst);
        if (ok && icons && !isinfo(e->name) &&
            (pl_inarc(p) ? hasinfo(p, e->name) : sidecarof(P->path, e->name, src))) {
            sfmt(iname, sizeof(iname), "%s.info", e->name);
            sfmt(itname, sizeof(itname), "%s.info", tn[s]);
            if (buildfull(dst, dir, itname)) {
                if (pl_inarc(p)) icok[s] = (char)arcfs_copyout(p, iname, dst, 0);
                else icok[s] = (char)copyfile(src, dst);
            }
        }
    }
    if (!onesrc && pl_inarc(p)) arcfs_release(p);
    /* 2: the copy the tool works on */
    if (ok && !abortop) ok = copyfile(arc, copy);
    if (abortop) ok = 0;
    cancelok = 0;
    /* what the result must hold: everything now, plus the staged files */
    wantfree();
    rel[0] = 0;
    wkind = k;
    if (ok && (!wantcurrent(q) || !stagewalk(stage, rel, 0))) {
        sfmt(opmsg, sizeof(opmsg), "not enough memory");
        ok = 0;
    }
    if (ok && k == AK_ZIP)
        for (s = 0; s < wn; s++)
            if (wv[s].repl && !zipsafe(wv[s].name)) {
                sfmt(opmsg, sizeof(opmsg), "zip cannot replace a name with * ? [ ] in it");
                ok = 0;
            }
    if (ok) {
        sfmt(line, sizeof(line), "packing with %s - Esc stops", toolname(k));
        showmsg(line);
        dirof(arc, src);
        /* lha and lzx skip a name they hold: the old member goes first */
        if (k != AK_ZIP) ok = delmembers(k, copy, src, 0);
        addline(line, k, copy);
        toolprog = 1;
        if (ok && bstart(&b, line, stage, copy, 0)) {
            ok = addtops(&b, stage);
            ok = bend(&b) && ok;
        } else {
            ok = 0;
        }
        toolprog = 0;
        creditrest();
        if (!ok && !opmsg[0]) sfmt(opmsg, sizeof(opmsg), "%s failed - the archive is unchanged", toolname(k));
        if (ok && !verify(copy, k)) {
            sfmt(opmsg, sizeof(opmsg), "%s did not do it: %s - the archive is unchanged", toolname(k), wbad);
            ok = 0;
        }
        if (ok && k == AK_ZIP) zippatch(copy);
    }
    {
        int repl = 0;
        for (s = 0; s < wn; s++) repl |= wv[s].repl;
        if (!ok && !abortop && k == AK_LHA && repl) {
            /* an old LhA (1.38) deletes the wrong members when told a
             * path: no d at all then - unpack, lay the new files over,
             * pack afresh, and the same check decides */
            DeleteFile((STRPTR)copy);
            cbuf_free();
            progoff();
            ok = rebuild(q, overstage, stage);
            wipetree(stage);
            cbuf_get();
        } else {
            ok = commit(ok, copy, arc, stage);
        }
    }
    cbuf_free();
    wantfree();
    progoff();
    /* a move: the sources go only now that the archive holds them */
    if (ok && ismove && !onesrc && pl_inarc(p)) {
        /* out of another image or archive: removed from it now */
        if (arcfs_isadf(p)) {
            for (s = 0; s < n; s++)
                if (!arcfs_adfdelone(p, P->ent[idx[s]].name, P->ent[idx[s]].kind != KD_FILE)) break;
        } else if (arcw_canwrite(p)) {
            arcw_delents(p, idx, n);
        }
    } else if (ok && ismove && !onesrc && !pl_inarc(p)) {
        for (s = 0; s < n; s++) {
            struct ent *e = &P->ent[idx[s]];
            if (!buildfull(src, P->path, e->name)) continue;
            if (e->kind != KD_FILE) deltree(src, 0, 0);
            else zap(src, 1);
            /* the icon only when it is in the archive too */
            if (icok[s] && icons && !isinfo(e->name) && sidecarof(P->path, e->name, src)) zap(src, 0);
        }
    }
    arcfs_reloadsame(q);
    placechanged(NULL, NULL, showbar ? 2 : 1);
    if (abortop) showmsg("cancelled - the archive is unchanged");
    else if (!ok) showmsg(opmsg[0] ? opmsg : "the archive is unchanged");
    abortop = 0;
    xfree(icok);
    return ok;
}

/* ---- rebuild: unpack all, change, pack afresh -------------------------- */

static int rebuild(struct place *p, editfn edit, void *ctx)
{
    int k = arcfs_kindof(p), ok;
    static char arc[CPATH], stage[CPATH], copy[CPATH], line[CPATH + 32];   /* (stack) */
    struct batch b;
    BPTR l;
    if (!toolthere(k)) return 0;
    opmsg[0] = 0;
    if (!names3(arcfs_file(p), arc, stage, copy, 'x')) return 0;
    if ((l = CreateDir((STRPTR)stage))) UnLock(l);
    if (!cbuf_get()) {
        wipetree(stage);
        sfmt(opmsg, sizeof(opmsg), "not enough memory");
        return 0;
    }
    cancelok = 1;
    abortop = 0;
    {
        /* the whole archive's bytes: unpacked, then packed again */
        ULONG all = 0;
        int i, n = arcfs_nmem(p), isdir;
        ULONG size;
        for (i = 0; i < n; i++) {
            arcfs_mem(p, i, &size, &isdir);
            if (!isdir) all += size;
        }
        if (arcfs_isadf(p) || !n) all = arcfs_treesize(p, "");
        progshow(all * 2 + 1);
    }
    ok = l && arcfs_extractall(p, stage) && !abortop && edit(stage, ctx);
    cancelok = 0;
    if (ok) {
        /* every unpacked file's checksum (and for a zip its date): the
         * new archive is held to them */
        char rel[CPATH];
        rel[0] = 0;
        wkind = k;
        ok = stagewalk(stage, rel, 0);
    }
    if (ok) {
        sfmt(line, sizeof(line), "packing with %s - Esc stops", toolname(k));
        showmsg(line);
        addline(line, k, copy);
        toolprog = 1;
        if (bstart(&b, line, stage, copy, 0)) {
            ok = addtops(&b, stage);
            ok = bend(&b) && ok;
        } else {
            ok = 0;
        }
        toolprog = 0;
        creditrest();
        if (!ok && !opmsg[0]) sfmt(opmsg, sizeof(opmsg), "%s failed - the archive is unchanged", toolname(k));
        if (ok && !verify(copy, k)) {
            sfmt(opmsg, sizeof(opmsg), "%s did not do it: %s - the archive is unchanged", toolname(k), wbad);
            ok = 0;
        }
        if (ok && k == AK_ZIP) zippatch(copy);
    }
    ok = commit(ok && !abortop, copy, arc, stage);
    cbuf_free();
    progoff();
    return ok;
}

/* the member path of a pane entry: sub/name */
static void memberof(struct place *p, const char *name, char *dst)
{
    const char *sub = arcfs_sub(p);
    if (*sub) sfmt(dst, CPATH, "%s/%s", sub, name);
    else sfmt(dst, CPATH, "%s", name);
}

/* m equals pre, or lies inside it (without case, as AmigaDOS) */
static int inside(const char *m, const char *pre)
{
    int l = strlen(pre), i;
    for (i = 0; i < l; i++) {
        int a = (unsigned char)m[i], b = (unsigned char)pre[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return m[l] == 0 || m[l] == '/';
}

struct delctx {
    struct place *p;
    const int *idx;
    int n;
};

int arcw_delents(struct place *p, const int *idx, int n);

static int delstage(const char *stage, void *ctx)
{
    struct delctx *c = ctx;
    char m[CPATH], path[CPATH];
    int i;
    for (i = 0; i < c->n; i++) {
        memberof(c->p, c->p->ent[c->idx[i]].name, m);
        strcpy(path, stage);
        if (!addpart(path, m, CPATH)) return 0;
        wipetree(path);
    }
    return 1;
}

void arcw_delete(struct place *p, int cur)
{
    int nmark = place_marks(p), *idx = NULL, n = 0, i;
    char mb[160];
    if (p->fail || (!nmark && cur < 0)) return;
    if (nmark) sfmt(mb, sizeof(mb), "delete %d marked entries from the archive? (y)es (n)o", nmark);
    else sfmt(mb, sizeof(mb), "delete \"%s\" from the archive? (y)es (n)o", p->ent[cur].name);
    if ((askkey(mb) | 32) != 'y') return;
    /* every marked entry, icons too (each is a member of its own), and
     * the icon of each file that has one, as everywhere else */
    if (!(idx = xalloc(p->n * sizeof(int)))) return;
    for (i = 0; i < p->n; i++)
        if (nmark ? p->ent[i].mark : i == cur) idx[n++] = i;
    if (icons) {
        int m = n, j, k;
        char nm[NAMEMAX + 8];
        for (j = 0; j < m; j++) {
            if (isinfo(p->ent[idx[j]].name)) continue;
            sfmt(nm, sizeof(nm), "%s.info", p->ent[idx[j]].name);
            if ((i = place_find(p, nm)) < 0) continue;
            for (k = 0; k < n && idx[k] != i; k++)
                ;
            if (k == n) idx[n++] = i;
        }
    }
    arcw_delents(p, idx, n);
    xfree(idx);
}

/* pane entries idx[] out of the archive (delete, and the second half
 * of a move out); 1 = done. The pane is read again either way. */
int arcw_delents(struct place *p, const int *idx, int n)
{
    struct place *P = p;
    int i, j, k = arcfs_kindof(p), ok = 1, lhadir = 0, left = 0;
    int nm = arcfs_nmem(p), isdir;
    /* static: off the 1.3 stack (a rebuild and a recursive wipe sit below) */
    static char m[CPATH], arc[CPATH], stage[CPATH], copy[CPATH], cwd[CPATH];
    ULONG size;
    int checkfail = 0;
    arc[0] = stage[0] = copy[0] = 0;
    opmsg[0] = 0;
    abortop = 0;
    /* the wants: every file now, the chosen ones marked gone */
    wantfree();
    if (!wantcurrent(p)) ok = 0;
    for (i = 0; i < n && ok; i++) {
        memberof(p, P->ent[idx[i]].name, m);
        for (j = 0; j < wn; j++)
            if (inside(wv[j].name, m)) wv[j].gone = 1;
        for (j = 0; j < nm; j++) {
            const char *mm = arcfs_mem(p, j, &size, &isdir);
            if (isdir && inside(mm, m)) lhadir = 1;
        }
    }
    for (j = 0; j < wn; j++) left += !wv[j].gone;
    if (ok && !left) {
        sfmt(opmsg, sizeof(opmsg), "that would empty the archive - delete the archive file itself instead");
        showmsg(opmsg);
        wantfree();
        return 0;
    }
    for (j = 0; j < wn && ok; j++)
        if (wv[j].gone && k == AK_ZIP && !zipsafe(wv[j].name)) lhadir = 1;    /* zip: rebuild */
    if (ok && lhadir && k != AK_LZX) {
        /* LhA cannot delete a directory member (zip one with a pattern
         * character in its name): unpack, prune, pack afresh */
        struct delctx c;
        c.p = p;
        c.idx = idx;
        c.n = n;
        ok = rebuild(p, delstage, &c);
    } else if (ok) {
        if (!toolthere(k)) ok = 0;
        else if (!names3(arcfs_file(p), arc, stage, copy, 'w')) ok = 0;
        if (ok) {
            if (!cbuf_get()) ok = 0;
            else {
                ok = copyfile(arc, copy);
                cbuf_free();
            }
        }
        if (ok) {
            struct batch b;
            char base[CPATH + 32];
            showmsg("deleting - please wait");
            dirof(arc, cwd);
            if (k == AK_LHA) sfmt(base, sizeof(base), "lha -M -x d \"%s\"", copy);
            else if (k == AK_LZX) sfmt(base, sizeof(base), "lzx d \"%s\"", copy);
            else sfmt(base, sizeof(base), "zip -d -q \"%s\"", copy);
            if (bstart(&b, base, cwd, copy, k == AK_LZX)) {
                for (j = 0; j < wn && ok; j++)
                    if (wv[j].gone) ok = badd(&b, wv[j].name);
                /* lzx and zip delete directory members too, by "name/" */
                for (j = 0; j < nm && ok && k != AK_LHA; j++) {
                    const char *mm = arcfs_mem(p, j, &size, &isdir);
                    for (i = 0; isdir && i < n; i++) {
                        memberof(p, P->ent[idx[i]].name, m);
                        if (inside(mm, m)) {
                            sfmt(m, sizeof(m), "%s/", mm);
                            ok = badd(&b, m);
                            break;
                        }
                    }
                }
                ok = bend(&b) && ok;
            } else {
                ok = 0;
            }
            if (!ok) sfmt(opmsg, sizeof(opmsg), "%s failed - the archive is unchanged", toolname(k));
            else if (!verify(copy, k)) {
                sfmt(opmsg, sizeof(opmsg), "%s did not do it: %s - the archive is unchanged", toolname(k), wbad);
                ok = 0;
                checkfail = 1;
            }
        }
        ok = commit(ok, copy, arc, stage);
        if (!ok && k == AK_LHA && checkfail && !abortop) {
            /* an old LhA's d misfires on paths (1.38): rebuild instead */
            struct delctx c;
            c.p = p;
            c.idx = idx;
            c.n = n;
            ok = rebuild(p, delstage, &c);
        }
    }
    wantfree();
    arcfs_reloadsame(p);
    placechanged(NULL, NULL, 2);
    if (abortop) showmsg("stopped - the archive is unchanged");
    else if (!ok) showmsg(opmsg[0] ? opmsg : "the archive is unchanged");
    abortop = 0;
    return ok;
}

struct renctx {
    const char *from, *to;      /* member paths */
    int icon;                   /* the .info beside it goes along */
};

static int renstage(const char *stage, void *ctx)
{
    struct renctx *c = ctx;
    char a[CPATH], b[CPATH];
    strcpy(a, stage);
    strcpy(b, stage);
    if (!addpart(a, c->from, CPATH) || !addpart(b, c->to, CPATH)) return 0;
    if (!Rename((STRPTR)a, (STRPTR)b)) {
        sfmt(opmsg, sizeof(opmsg), "cannot rename inside the archive");
        return 0;
    }
    if (c->icon && strlen(a) + 6 < CPATH && strlen(b) + 6 < CPATH) {
        strcat(a, ".info");
        strcat(b, ".info");
        Rename((STRPTR)a, (STRPTR)b);
    }
    return 1;
}

void arcw_rename(struct place *p, const char *from, const char *to)
{
    char mf[CPATH], mt[CPATH], nn[CPATH], mfi[CPATH + 6], mti[CPATH + 6], fi[NAMEMAX + 8];
    struct renctx c;
    int j, l, ok = 1;
    memberof(p, from, mf);
    memberof(p, to, mt);
    l = strlen(mf);
    /* its icon follows, when it has one in the archive */
    sfmt(fi, sizeof(fi), "%s.info", from);
    c.icon = icons && !isinfo(from) && place_find(p, fi) >= 0;
    if (c.icon && nocase(from, to) != 0) {
        char ti[NAMEMAX + 8];
        sfmt(ti, sizeof(ti), "%s.info", to);
        if (place_find(p, ti) >= 0) {
            /* say so now: the rebuild's check would only say "unchanged" */
            sfmt(opmsg, sizeof(opmsg), "%.60s is already in the archive", ti);
            showmsg(opmsg);
            return;
        }
    }
    sfmt(mfi, sizeof(mfi), "%s.info", mf);
    sfmt(mti, sizeof(mti), "%s.info", mt);
    wantfree();
    if (!wantcurrent(p)) ok = 0;
    /* the result has the same files, the renamed ones under the new name */
    for (j = 0; j < wn && ok; j++)
        if (inside(wv[j].name, mf)) {
            sfmt(nn, sizeof(nn), "%s%s", mt, wv[j].name + l);
            if (!(wv[j].name = pool_str(&wpl, nn))) ok = 0;
        } else if (c.icon && nocase(wv[j].name, mfi) == 0) {
            if (!(wv[j].name = pool_str(&wpl, mti))) ok = 0;
        }
    wanthash();                         /* the names changed */
    c.from = mf;
    c.to = mt;
    if (ok) ok = rebuild(p, renstage, &c);
    wantfree();
    arcfs_reloadsame(p);
    placechanged(p, to, 2);
    if (abortop) showmsg("stopped - the archive is unchanged");
    else if (!ok) showmsg(opmsg[0] ? opmsg : "the archive is unchanged");
    abortop = 0;
}

void arcw_mkdir(struct place *p, const char *name)
{
    int k = arcfs_kindof(p), ok;
    static char arc[CPATH], stage[CPATH], copy[CPATH], dir[CPATH], m[CPATH], line[CPATH + 32];
    struct batch b;
    BPTR l;
    if (!toolthere(k)) return;
    opmsg[0] = 0;
    if (!names3(arcfs_file(p), arc, stage, copy, 'w')) {
        showmsg(opmsg);
        return;
    }
    if ((l = CreateDir((STRPTR)stage))) UnLock(l);
    memberof(p, name, m);
    ok = l && mkpath(stage, m, dir);
    wantfree();
    if (ok) ok = wantcurrent(p);
    if (ok && cbuf_get()) {
        ok = copyfile(arc, copy);
        cbuf_free();
    } else {
        ok = 0;
    }
    if (ok) {
        showmsg("adding the folder - please wait");
        addline(line, k, copy);
        if (bstart(&b, line, stage, copy, 0)) {
            ok = addtops(&b, stage);
            ok = bend(&b) && ok;
        } else {
            ok = 0;
        }
        if (!ok) sfmt(opmsg, sizeof(opmsg), "%s failed - the archive is unchanged", toolname(k));
        else if (!verify(copy, k)) {
            sfmt(opmsg, sizeof(opmsg), "%s did not do it: %s - the archive is unchanged", toolname(k), wbad);
            ok = 0;
        }
    }
    ok = commit(ok, copy, arc, stage);
    wantfree();
    arcfs_reloadsame(p);
    placechanged(p, name, 1);
    if (abortop) showmsg("stopped - the archive is unchanged");
    else if (!ok) showmsg(opmsg[0] ? opmsg : "the archive is unchanged");
    abortop = 0;
}

/* ---- u: unpack archives into the other pane ---------------------------- */

void arcw_unpack(struct place *p, struct place *q, int cur)
{
    int nmark = place_marks(p), i, n = 0, done = 0, bad = 0, r;
    struct place *P = p;
    static struct place up;             /* each archive in turn, not shown */
    char src[CPATH], mb[160];
    ULONG total = 0;
    if (pl_involume(p) || P->fail || (!nmark && cur < 0)) return;
    if (pl_inarc(p)) {
        showmsg("u unpacks archive files - copy (c) takes files out of this one");
        return;
    }
    if (pl_involume(q) || pl_inarc(q) || q->fail) {
        showmsg("archives unpack into a folder - not the volume list or an archive");
        return;
    }
    /* the archives in the set, sized for one bar */
    for (i = 0; i < P->n; i++) {
        struct ent *e = &P->ent[i];
        if (nmark ? !e->mark : i != cur) continue;
        if (e->kind != KD_FILE || !buildfull(src, P->path, e->name)) continue;
        if (arcfs_load(&up, src) == ARC_OK) {
            total += arcfs_treesize(&up, "");
            n++;
        }
        arcfs_leave(&up);
    }
    if (!n) {
        showmsg("not an archive CFile can unpack");
        return;
    }
    {
        /* the target named, its tail when it is long */
        const char *t = q->path;
        int tl = strlen(t);
        if (tl > 40) t += tl - 40;
        sfmt(mb, sizeof(mb), "unpack %d archive%s into %s%s? (y)es (n)o", n, n == 1 ? "" : "s", tl > 40 ? "..." : "", t);
    }
    if ((askkey(mb) | 32) != 'y') return;
    if (!cbuf_get()) {
        showmsg("not enough memory");
        return;
    }
    cancelok = 1;
    abortop = 0;
    progshow(total);
    opmsg[0] = 0;
    for (i = 0; i < P->n && !abortop; i++) {
        struct ent *e = &P->ent[i];
        if (nmark ? !e->mark : i != cur) continue;
        if (e->kind != KD_FILE || !buildfull(src, P->path, e->name)) continue;
        if ((r = arcfs_load(&up, src)) != ARC_OK) {
            arcfs_leave(&up);
            continue;
        }
        /* into the directory itself; an existing file is replaced safely */
        if (arcfs_copyout(&up, "", q->path, 1)) done++;
        else bad++;
        arcfs_leave(&up);
    }
    cancelok = 0;
    progoff();
    cbuf_free();
    placechanged(NULL, NULL, 2);
    if (abortop) sfmt(mb, sizeof(mb), "cancelled - %d of %d unpacked", done, n);
    else if (bad) sfmt(mb, sizeof(mb), "%s", opmsg[0] ? opmsg : "an archive could not be unpacked");
    else sfmt(mb, sizeof(mb), "unpacked %d archive%s", done, done == 1 ? "" : "s");
    showmsg(mb);
    abortop = 0;
}

/* ---- p: pack the set into a new archive in the other pane ------------- */

static int kindbyname(const char *name)
{
    int l = strlen(name);
    if (l > 4 && !nocase(name + l - 4, ".lha")) return AK_LHA;
    if (l > 4 && !nocase(name + l - 4, ".lzh")) return AK_LHA;
    if (l > 4 && !nocase(name + l - 4, ".lzx")) return AK_LZX;
    if (l > 4 && !nocase(name + l - 4, ".zip")) return AK_ZIP;
    return AK_NONE;
}

/* the source files under a pane entry, as the archive will name them */
static int packwalk(const char *path, char *rel, int depth, int isdir, ULONG size, LONG days, LONG mins)
{
    struct dscan *d;
    struct dent e;
    char *child;
    int rl = strlen(rel), ok = 1, r;
    if (!isdir) {
        struct want *w = wantadd(rel, size);
        if (w) {
            w->fix = 1;
            w->days = days;
            w->mins = mins;
        }
        return w != NULL;
    }
    if (depth > MAXDEPTH || !(d = ds_open(path))) return 0;
    if (!(child = xalloc(CPATH))) {
        ds_close(d);
        return 0;
    }
    while (ok && (r = ds_next(d, &e)) > 0) {
        if (rl + strlen(e.name) + 2 > CPATH || !buildfull(child, path, e.name)) {
            ok = 0;
            break;
        }
        sfmt(rel + rl, CPATH - rl, "/%s", e.name);
        if (e.type == ST_SOFTLINK) {
            /* the tools follow or skip links as they please: not checked */
        } else {
            ok = packwalk(child, rel, depth + 1, e.type > 0, e.size, e.days, e.mins);
        }
        rel[rl] = 0;
    }
    if (r < 0) ok = 0;
    ds_close(d);
    xfree(child);
    return ok;
}

static void pack(struct place *p, struct place *q, const int *idx, int nsel)
{
    int i, k, ok = 1, t, n = 0, s;
    struct place *P = p;
    char name[NAMEMAX + 4], target[CPATH], arc[CPATH], copy[CPATH], cwd[CPATH], rel[CPATH];
    char line[CPATH + 32], src[CPATH], mb[160];
    struct batch b;
    if (pl_inarc(p)) {
        showmsg("p packs files from a directory - copy (c) them out first");
        return;
    }
    if (pl_involume(q) || pl_inarc(q) || q->fail) {
        showmsg("archives are made in a folder - not the volume list or an archive");
        return;
    }
    sfmt(name, sizeof(name), "%s.lha", nsel > 1 ? filepart(P->path) : P->ent[idx[0]].name);
    if (!name[0] || name[0] == '.') sfmt(name, sizeof(name), "archive.lha");
    if (!lineinput("pack into (.lha .lzx .zip): ", name, NAMEMAX, 1) || !name[0]) {
        askdone();
        return;
    }
    if ((k = kindbyname(name)) == AK_NONE) {
        showmsg("the name must end in .lha, .lzh, .lzx or .zip");
        return;
    }
    if (!toolthere(k)) return;
    if (!buildfull(target, q->path, name)) {
        showmsg("the path is too long");
        return;
    }
    if ((t = os_pathtype(target)) == 2) {
        showmsg("a directory with that name is in the way");
        return;
    }
    if (t == 1) {
        sfmt(mb, sizeof(mb), "\"%s\" exists - replace it? (y)es (n)o", name);
        if ((askkey(mb) | 32) != 'y') return;
    }
    if (!abspath(q->path, arc) || !addpart(arc, name, CPATH) || !abspath(P->path, cwd)) {
        showmsg("the path is too long");
        return;
    }
    copyname(copy, arc);
    DeleteFile((STRPTR)copy);
    /* what the new archive must hold */
    wantfree();
    for (s = 0; s < nsel && ok; s++) {
        struct ent *e = &P->ent[idx[s]];
        if (!buildfull(src, P->path, e->name)) ok = 0;
        strcpy(rel, e->name);
        if (ok) ok = packwalk(src, rel, 0, e->kind != KD_FILE, e->size, e->date / 1440, e->date % 1440);
        if (ok && icons && !isinfo(e->name) && sidecarof(P->path, e->name, src)) {
            sfmt(rel, sizeof(rel), "%s.info", e->name);
            ok = wantadd(rel, 0) != NULL;
            if (ok) wv[wn - 1].size = (ULONG)-1;        /* size not known here */
        }
        n++;
    }
    for (i = 0; i < wn; i++)
        if (wv[i].size == (ULONG)-1) {
            /* an icon: read its size now */
            struct dent e;
            struct dscan *d = ds_open(P->path);
            if (d) {
                while (ds_next(d, &e) > 0)
                    if (nocase(e.name, wv[i].name) == 0) wv[i].size = e.size;
                ds_close(d);
            }
        }
    if (!ok) {
        wantfree();
        showmsg("cannot read the files to pack");
        return;
    }
    sfmt(mb, sizeof(mb), "packing with %s - Esc stops", toolname(k));
    showmsg(mb);
    progshow(packbytes() + 1);
    toolprog = 1;
    addline(line, k, copy);
    if (bstart(&b, line, cwd, copy, 0)) {
        for (s = 0; s < nsel && ok; s++) {
            struct ent *e = &P->ent[idx[s]];
            ok = badd(&b, e->name);
            if (ok && icons && !isinfo(e->name) && sidecarof(P->path, e->name, src)) {
                sfmt(rel, sizeof(rel), "%s.info", e->name);
                ok = badd(&b, rel);
            }
        }
        ok = bend(&b) && ok;
    } else {
        ok = 0;
    }
    toolprog = 0;
    creditrest();
    progoff();
    opmsg[0] = 0;
    if (!ok) sfmt(opmsg, sizeof(opmsg), "%s failed - nothing was made", toolname(k));
    else if (!verify(copy, k)) {
        sfmt(opmsg, sizeof(opmsg), "%s did not do it: %s - nothing was made", toolname(k), wbad);
        ok = 0;
    }
    if (ok && k == AK_ZIP) zippatch(copy);
    if (ok && !swapin(copy, arc, 0)) {
        sfmt(opmsg, sizeof(opmsg), "cannot put the archive in place");
        ok = 0;
    }
    if (!ok) DeleteFile((STRPTR)copy);
    wantfree();
    placechanged(q, name, 1);
    if (ok) sfmt(mb, sizeof(mb), "packed %d entr%s into %s", n, n == 1 ? "y" : "ies", name);
    showmsg(ok ? mb : abortop ? "stopped - nothing was made" : opmsg);
    abortop = 0;
}

void arcw_pack(struct place *p, struct place *q, int cur)
{
    int *idx, n;
    if (pl_involume(p) || p->fail || !(n = place_pick(p, cur, &idx))) return;
    pack(p, q, idx, n);
    xfree(idx);
}
