/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* ops - everything that changes the disk or runs something.
 *
 * Ranger's model: yy/dd put the marked set (or the entry under the
 * cursor) on the yank list, pp pastes it where you are now. The core
 * (fs/ in the CFile repo) does the copying, moving, deleting and
 * renaming, with its safety rules: new bytes go beside a file and
 * replace it only when whole, a move deletes only what arrived, links
 * stay links, and a file's .info icon always travels with it, shown or
 * not. Archives and disk images are places like folders: yank out of
 * one, paste into one. */
#include <exec/memory.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <stdlib.h>
#include <string.h>
#include "ahiru.h"
#include "fs/fe.h"
#include "fs/ops.h"
#include "fs/arcfs.h"
#include "fs/arcw.h"
#include "fs/os.h"
#include "core/util.h"

/* ---- the yank list: where from, and the names --------------------- */

int yankn, yankcut;
static char ypath[CPATH];
static char **ynames;
static struct pool *ypool;
static struct place ysrc;               /* read again when pasting */

void op_unyank(void)
{
    yankn = 0;
    free(ynames);
    ynames = NULL;
    pool_free(&ypool);
    place_free(&ysrc);
}

void op_yank(int cut)
{
    char m[80];
    long i, nm = 0;
    if (cur->pl.path[0] == 0 || cur->n == 0) {
        say("nothing to yank here", 1);   /* volumes are not files */
        return;
    }
    op_unyank();
    for (i = 0; i < cur->n; i++) nm += DENT(cur, i)->mark != 0;
    /* find results are named "sub/name": the copy reads one folder, so
     * they must all be in the same one (that folder is the source) */
    {
        const char *sub = NULL;
        int sl = 0;
        for (i = 0; i < cur->n; i++) {
            const char *nmi, *sl2;
            if (nm ? !DENT(cur, i)->mark : i != cur->sel) continue;
            nmi = DNAME(cur, i);
            sl2 = strrchr(nmi, '/');
            if (!sub) sub = nmi, sl = sl2 ? (int)(sl2 - nmi) : 0;
            else if ((sl2 ? (int)(sl2 - nmi) : 0) != sl || strncmp(nmi, sub, sl)) {
                say("these are in different folders - yank them one folder at a time", 1);
                return;
            }
        }
        strcpy(ypath, cur->pl.path);
        if (sub && sl) {
            char d[PATHMAX];
            if (sl >= PATHMAX || strlen(cur->pl.path) + sl + 2 >= PATHMAX) {
                say("the path is too long", 1);
                return;
            }
            memcpy(d, sub, sl);
            d[sl] = 0;
            joinpath(ypath, cur->pl.path, d);
        }
    }
    if (!(ynames = malloc((nm ? nm : 1) * sizeof(char *)))) {
        say("not enough memory", 1);
        return;
    }
    for (i = 0; i < cur->n; i++) {
        const char *nmi, *sl2;
        if (nm ? !DENT(cur, i)->mark : i != cur->sel) continue;
        nmi = DNAME(cur, i);
        if ((sl2 = strrchr(nmi, '/'))) nmi = sl2 + 1;
        if (!(ynames[yankn] = pool_str(&ypool, nmi))) break;
        yankn++;
        DENT(cur, i)->mark = 0;
    }
    yankcut = cut;
    drawcols();
    fmt(m, "%s %d item%s - pp pastes, po overwrites", cut ? "cut" : "yanked",
            yankn, yankn == 1 ? "" : "s");
    say(m, 0);
}

void op_paste(int overwrite)
{
    int i, k, found = 0;
    if (yankn == 0) {
        say("nothing yanked - yy copies, dd cuts", 1);
        return;
    }
    if (cur->pl.path[0] == 0) {
        say("cannot paste into the volume list", 1);
        return;
    }
    /* the place it came from, as it is now, the yanked names marked */
    if (!place_go(&ysrc, ypath)) {
        say("the yanked entries are gone", 1);
        op_unyank();
        drawstatus();
        return;
    }
    place_read(&ysrc, PS_NAME, 0);
    for (i = 0; i < yankn; i++)
        if ((k = place_find(&ysrc, ynames[i])) >= 0) {
            ysrc.ent[k].mark = 1;
            found++;
        }
    if (!found) {
        say("the yanked entries are gone", 1);
        op_unyank();
        drawstatus();
        return;
    }
    /* po: overwrite without asking - but never a newer version with an
     * older one (the core still asks then) */
    /* between folders on disks: in the background (jobs.c), the job takes
     * the places over; into or out of an archive: here and now */
    if (!ysrc.arc && !cur->pl.arc) {
        struct place dst;
        memset(&dst, 0, sizeof(dst));
        if (place_go(&dst, cur->pl.path) && job_start(&ysrc, &dst, yankcut, overwrite, found)) {
            memset(&ysrc, 0, sizeof(ysrc));     /* (the job's now) */
            if (yankcut) op_unyank();
            drawstatus();
            return;
        }
        place_free(&dst);
    }
    fs_copy(&ysrc, &cur->pl, -1, yankcut, overwrite);
    if (yankcut) {
        op_unyank();
        drawstatus();
    }
}

/* ---- delete, rename, mkdir, touch, unpack --------------------------- */

void op_delete(void)
{
    if (cur->n == 0) return;
    fs_delete(&cur->pl, DIDX(cur));
}

/* mode 0: empty field, 1: cursor before the extension, 2: at the end,
 * 3: at the start (fe.c's lineinput reads renamemode) */
int renamemode = -1;

void op_rename(int mode)
{
    if (cur->n == 0 || cur->pl.path[0] == 0) return;
    renamemode = mode;
    fs_rename(&cur->pl, DIDX(cur));
    renamemode = -1;
}

void op_mkdir(const char *name)
{
    if (name[0]) fs_mkdir(&cur->pl, name);
}

void op_touch(const char *name)
{
    char p[PATHMAX];
    BPTR fh;
    if (!name[0] || cur->pl.path[0] == 0) return;
    if (cur->pl.arc) {
        say("touch works on a disk - not inside an archive", 1);
        return;
    }
    joinpath(p, cur->pl.path, name);
    if (pathexists(p)) {
        struct DateStamp now;
        DateStamp(&now);
        SetFileDate((STRPTR)p, &now);
    } else if ((fh = Open((STRPTR)p, MODE_NEWFILE)) != 0)
        Close(fh);
    else {
        showfault("cannot create the file");
        return;
    }
    reloadall(name);
}

/* x: the marked archives (or the one under the cursor) unpacked here */
void op_extract(void)
{
    if (cur->n == 0) return;
    if (cur->pl.arc) {
        say("x unpacks archive files on a disk - yank and paste copy out of this one", 1);
        return;
    }
    arcw_unpack(&cur->pl, &cur->pl, DIDX(cur));
}

/* ---- running things -------------------------------------------------------- */

static void conspec(char *dst, const char *title, int wait)
{
    int w = scr ? scr->Width : 640, h = scr ? scr->Height : 256;
    fmt(dst, "CON:0/%d/%d/%d/%s/AUTO/CLOSE%s%s%s", h / 3, w, h - h / 3, title,
            wait ? "/WAIT" : "", ownscr ? "/SCREEN " : "", ownscr ? PUBNAME : "");
}

/* an editor that lives in a terminal (Sumi, or EDITORCONSOLE yes in
 * Ahiru.config): on Ahiru's own screen or in its window it gets a
 * console window as big as the screen; inside a terminal, the terminal */
int cfgedcon;
static int termeditor(const char *ed)
{
    char name[40];
    const char *b = ed, *q;
    int i;
    if (cfgedcon) return 1;
    for (q = ed; *q && *q != ' '; q++)
        if (*q == ':' || *q == '/') b = q + 1;
    for (i = 0; i < 39 && b[i] && b[i] != ' '; i++)
        name[i] = (char)(b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i]);
    name[i] = 0;
    return !strcmp(name, "sumi");
}

static void editspec(char *dst)
{
    int w = 640, h = 256, top = 0;
    struct Screen *wb;
    if (scr) {
        w = scr->Width;
        h = scr->Height;
    } else if ((wb = LockPubScreen(NULL)) != NULL) {
        w = wb->Width;
        top = wb->BarHeight + 1;
        h = wb->Height - top;
        UnlockPubScreen(NULL, wb);
    }
    fmt(dst, "CON:0/%d/%d/%d/Ahiru - edit/CLOSE%s%s", top, w, h,
        ownscr ? "/SCREEN " : "", ownscr ? PUBNAME : "");
}

/* run cmd in the current directory, its output in a console window
 * on our screen that waits for a close (console 1); console 2: a
 * screen-sized console window that goes when the command ends (an
 * editor of the terminal's) */
static LONG runin(const char *cmd, int console, int async)
{
    char spec[160];
    BPTR lock = 0, old = 0, in, out;
    LONG rc;
    if (cur->pl.path[0] && (lock = Lock((STRPTR)cur->pl.path, SHARED_LOCK)) != 0)
        old = CurrentDir(lock);
    if (uimode == UI_TERM && !async) {
        /* inside a terminal: the command gets the terminal itself, as
         * ranger's do; a Shell command's output waits for a key */
        ui_suspend();
        in = Open((STRPTR)"*", MODE_OLDFILE);
        out = Open((STRPTR)"*", MODE_NEWFILE);
    } else if (console == 2) {
        editspec(spec);
        in = Open((STRPTR)spec, MODE_OLDFILE);
        out = 0;
    } else if (console) {
        conspec(spec, "Ahiru", !async);
        in = Open((STRPTR)spec, MODE_OLDFILE);
        out = 0;
    } else {
        in = Open((STRPTR)"NIL:", MODE_OLDFILE);
        out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    }
    rc = SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out,
                    SYS_Asynch, async, NP_StackSize, 32768, TAG_DONE);
    if (!async || rc == -1) {
        if (in) Close(in);
        if (out) Close(out);
    }
    if (lock) {
        CurrentDir(old);
        UnLock(lock);
    }
    if (uimode == UI_TERM && !async) {
        ui_resume(console == 1);
        drawall();
    }
    if (win) ActivateWindow(win);
    return rc;
}

void op_shell(const char *cmd)
{
    LONG rc;
    char m[60];
    if (!cmd[0]) return;
    rc = runin(cmd, 1, 0);
    reloadall(NULL);
    if (rc == -1) showfault("cannot run that");
    else if (rc) {
        fmt(m, "returned %ld", (long)rc);
        say(m, 1);
    }
}

void op_newshell(void)
{
    char cmd[200], spec[160];
    if (uimode == UI_TERM) {
        /* a Shell right here, in the terminal; EndShell comes back */
        BPTR in, out;
        ui_suspend();
        in = Open((STRPTR)"*", MODE_OLDFILE);
        out = Open((STRPTR)"*", MODE_NEWFILE);
        if (out) FPuts(out, (STRPTR)"Ahiru: a Shell here - EndShell goes back\n");
        if (out) Flush(out);
        if (in)                     /* no command: it reads them from the terminal */
            Execute((STRPTR)"", in, 0);
        if (in) Close(in);
        if (out) Close(out);
        ui_resume(0);
        drawall();
        return;
    }
    conspec(spec, "Ahiru Shell", 0);
    fmt(cmd, "NewShell \"%s\"", spec);
    if (runin(cmd, 0, 0) == -1) showfault("cannot open a shell");
}

/* the editor on path (EDITOR in the config, ENV:EDITOR, else C:Ed),
 * waited for; 1 = it ran */
int op_editpath(const char *path)
{
    char ed[128], cmd[PATHMAX + 140];
    LONG rc;
    if (cfgeditor[0]) strcpy(ed, cfgeditor);
    else if (GetVar((STRPTR)"EDITOR", (STRPTR)ed, sizeof(ed), 0) <= 0)
        strcpy(ed, "C:Ed");
    fmt(cmd, "%s \"%s\"", ed, path);
    if (scr && !termeditor(ed)) ScreenToBack(scr);  /* most editors open on Workbench */
    rc = runin(cmd, termeditor(ed) ? 2 : 0, 0);
    if (scr) ScreenToFront(scr);
    drawall();
    if (rc == -1) showfault("cannot start the editor");
    return rc != -1;
}

/* the entry under the cursor as a real file: itself on a disk, or a
 * scratch copy (T: or RAM:) of an archive member - *tmp says so */
static int realfile(char *path, int *tmp)
{
    *tmp = 0;
    if (cur->n == 0) return 0;
    if (!cur->pl.arc) {
        selpath(path);
        return 1;
    }
    if (!arcfs_temp(&cur->pl, DNAME(cur, cur->sel), path)) return 0;
    *tmp = 1;
    return 1;
}

static void viewpath(const char *path, const char *title, int hex)
{
    BPTR fh;
    long size, cap, n;
    unsigned char *buf;
    int ty;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) {
        showfault("cannot open");
        return;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    cap = (long)AvailMem(MEMF_ANY | MEMF_LARGEST) / 2;
    if (cap > 4L * 1024 * 1024) cap = 4L * 1024 * 1024;
    n = size < cap ? size : cap;
    if (!(buf = malloc(n + 1))) {
        Close(fh);
        say("not enough memory to view this", 1);
        return;
    }
    n = Read(fh, buf, n);
    Close(fh);
    if (n < 0) n = 0;
    ty = hex ? TY_OTHER : sniffmem(buf, n);
    pager(title, buf, n, ty);
    free(buf);
    drawall();
}

void op_view(int hex)
{
    char path[PATHMAX];
    int tmp;
    if (!realfile(path, &tmp)) return;
    viewpath(path, DNAME(cur, cur->sel), hex);
    if (tmp) DeleteFile((STRPTR)path);
}

static int stampof(const char *path, struct DateStamp *ds, long *size)
{
    BPTR l;
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    int ok = 0;
    if (!fib) return 0;
    if ((l = Lock((STRPTR)path, SHARED_LOCK)) != 0) {
        if (Examine(l, fib)) {
            *ds = fib->fib_Date;
            *size = fib->fib_Size;
            ok = 1;
        }
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    return ok;
}

/* inside an archive: the member is edited as a scratch copy and goes
 * back in (the core's checked commit) when it was saved */
void op_edit(void)
{
    char ed[128], cmd[PATHMAX + 140], path[PATHMAX], name[256];   /* a member name runs to 255 */
    struct DateStamp d0, d1;
    long s0 = -1, s1 = -1;
    int tmp, ok;
    if (cur->n == 0) return;
    if (cur->pl.arc && !arcw_canwrite(&cur->pl) && !arcfs_isadf(&cur->pl)) {
        arcfs_guard(&cur->pl);
        return;
    }
    strcpy(name, DNAME(cur, cur->sel));
    if (!realfile(path, &tmp)) return;
    if (tmp) stampof(path, &d0, &s0);
    if (cfgeditor[0]) strcpy(ed, cfgeditor);
    else if (GetVar((STRPTR)"EDITOR", (STRPTR)ed, sizeof(ed), 0) <= 0)
        strcpy(ed, "C:Ed");
    fmt(cmd, "%s \"%s\"", ed, path);
    if (scr && !termeditor(ed)) ScreenToBack(scr);  /* most editors open on Workbench */
    if (runin(cmd, termeditor(ed) ? 2 : 0, 0) == -1) {
        if (scr) ScreenToFront(scr);
        showfault("cannot start the editor");
        if (tmp) DeleteFile((STRPTR)path);
        return;
    }
    if (scr) ScreenToFront(scr);
    if (!tmp) {
        reloadall(NULL);
        return;
    }
    if (!stampof(path, &d1, &s1) || (s1 == s0 && CompareDates(&d0, &d1) == 0)) {
        DeleteFile((STRPTR)path);           /* not saved: nothing to put back */
        drawall();
        return;
    }
    ok = arcfs_isadf(&cur->pl) ? arcfs_adfputfile(&cur->pl, path, name) : arcw_putfile(&cur->pl, path, name);
    if (ok) {
        DeleteFile((STRPTR)path);
        reloadall(name);
    } else {
        char m[PATHMAX + 60];
        drawall();
        fmt(m, "not put back - your edit is kept in %s", path);
        say(m, 1);
    }
}

/* Enter on a file that is not an archive: run it, read it, or hand it
 * to MultiView - whatever the file is */
void op_openfile(void)
{
    unsigned char head[1100];
    char path[PATHMAX], cmd[PATHMAX + 80], m[MAXCOLS];
    BPTR fh;
    long n = 0;
    int ty, tmp, kind;
    if (!realfile(path, &tmp)) return;
    kind = kindof(DNAME(cur, cur->sel));
    if ((fh = Open((STRPTR)path, MODE_OLDFILE)) != 0) {
        n = Read(fh, head, sizeof(head));
        Close(fh);
    }
    ty = sniffmem(head, n < 0 ? 0 : n);
    if (tmp && ty != TY_TEXT && ty != TY_ANSI && ty != TY_EMPTY) {
        /* a scratch copy cannot be run or handed over: it goes away */
        viewpath(path, DNAME(cur, cur->sel), 1);
        DeleteFile((STRPTR)path);
        say("inside an archive: yank and paste it out to run or show it", 0);
        return;
    }
    if (ty == TY_EXEC) {
        fmt(m, "run %.50s? (y/n)", lastpart(path));
        if (ask(m) == 'y') {
            fmt(cmd, "\"%s\"", path);
            op_shell(cmd);
        }
        return;
    }
    if (ty == TY_TEXT || ty == TY_ANSI || ty == TY_EMPTY) {
        viewpath(path, DNAME(cur, cur->sel), 0);
        if (tmp) DeleteFile((STRPTR)path);
        return;
    }
    if (pathexists("SYS:Utilities/MultiView") &&
        (ty == TY_IFF || kind == K_IMG || kind == K_AUD || kind == K_DOC ||
         nocase(extof(path), "guide") == 0)) {
        fmt(cmd, "SYS:Utilities/MultiView \"%s\"%s%s", path,
                ownscr ? " PUBSCREEN " : "", ownscr ? PUBNAME : "");
        if (runin(cmd, 0, 1) == -1) showfault("cannot start MultiView");
        return;
    }
    viewpath(path, DNAME(cur, cur->sel), 1);
}
