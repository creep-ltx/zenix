/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* more2 - the clipboard, open-with, jumping by habit and the live
 * filter (from Yazi, 10.10.2026). Keys in keys.c's table. */
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/clipboard.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <stdlib.h>
#include <string.h>
#include "ahiru.h"
#include "fs/os.h"
#include "core/util.h"

struct Library *IconBase;

/* ---- the clipboard: text, as FTXT CHRS on unit 0 ------------------------ */

static int clipput(const char *s)
{
    struct MsgPort *mp;
    struct IOClipReq *io;
    ULONG l = (ULONG)strlen(s), pad = l & 1, form = 12 + l + pad;
    unsigned char *b;
    int ok = 0;
    if (!(b = malloc(20 + l + 1))) return 0;
    memcpy(b, "FORM", 4);
    b[4] = (unsigned char)(form >> 24), b[5] = (unsigned char)(form >> 16), b[6] = (unsigned char)(form >> 8), b[7] = (unsigned char)form;
    memcpy(b + 8, "FTXTCHRS", 8);
    b[16] = (unsigned char)(l >> 24), b[17] = (unsigned char)(l >> 16), b[18] = (unsigned char)(l >> 8), b[19] = (unsigned char)l;
    memcpy(b + 20, s, l);
    b[20 + l] = 0;
    if ((mp = CreateMsgPort())) {
        if ((io = (struct IOClipReq *)CreateIORequest(mp, sizeof(struct IOClipReq)))) {
            if (OpenDevice((STRPTR)"clipboard.device", 0, (struct IORequest *)io, 0) == 0) {
                io->io_Command = CMD_WRITE;
                io->io_Data = (STRPTR)b;
                io->io_Length = 20 + l + pad;
                io->io_Offset = 0;
                io->io_ClipID = 0;
                if (DoIO((struct IORequest *)io) == 0) {
                    io->io_Command = CMD_UPDATE;
                    ok = DoIO((struct IORequest *)io) == 0;
                }
                CloseDevice((struct IORequest *)io);
            }
            DeleteIORequest((struct IORequest *)io);
        }
        DeleteMsgPort(mp);
    }
    free(b);
    return ok;
}

/* what: 0 the path, 1 the name, 2 the folder */
void clip_copy(int what)
{
    char p[PATHMAX], m[PATHMAX + 30];
    const char *s = p;
    if (!cur->n && what != 2) return;
    selpath(p);
    if (what == 1) s = cur->n ? DNAME(cur, cur->sel) : "";
    else if (what == 2) s = cur->pl.path;
    if (!clipput(s)) {
        say("cannot write to the clipboard", 1);
        return;
    }
    fmt(m, "on the clipboard: %.100s", s);
    say(m, 0);
}

/* ---- open with: a menu of the programs that may open the file ----------- */

#define NWITH 9
static char *wcmd[NWITH], wlab[NWITH][48];

/* OPENWITH lines from the config: "ext program" (ext * = any file) */
#define MAXOW 32
static char *owext[MAXOW], *owprog[MAXOW];
static int now;

void openwith_add(const char *line)
{
    const char *p = line;
    int l;
    if (now == MAXOW) return;
    while (*p && *p != ' ' && *p != '\t') p++;
    l = (int)(p - line);
    while (*p == ' ' || *p == '\t') p++;
    if (!l || !*p) return;
    owext[now] = malloc(l + 1);
    owprog[now] = strdup(p);
    if (!owext[now] || !owprog[now]) return;
    memcpy(owext[now], line, l);
    owext[now][l] = 0;
    now++;
}

void openwith_save(BPTR fh)
{
    int i;
    char line[300];
    for (i = 0; i < now; i++) {
        sfmt(line, sizeof(line), "OPENWITH %s %s\n", owext[i], owprog[i]);
        FPuts(fh, (STRPTR)line);
    }
}

static int nwith;

static void with(const char *label, const char *cmd)
{
    if (nwith == NWITH) return;
    fmt(wlab[nwith], "%.46s", label);
    wcmd[nwith] = strdup(cmd);
    if (wcmd[nwith]) nwith++;
}

void open_with(void)
{
    char path[PATHMAX], cmd[PATHMAX * 2], ed[128];
    static const char *items[2 * NWITH];
    static char keys[NWITH][2];
    const char *e;
    int i, k;
    struct DiskObject *dob;
    if (!cur->n || (dflags(cur, cur->sel) & F_DIR)) return;
    if (cur->pl.arc) {
        say("copy it out of the archive to open it with a program", 1);
        return;
    }
    selpath(path);
    e = extof(path);
    nwith = 0;
    /* the program its own icon names */
    if (!IconBase) IconBase = OpenLibrary((STRPTR)"icon.library", 36);
    if (IconBase && (dob = GetDiskObject((STRPTR)path))) {
        if (dob->do_DefaultTool && dob->do_DefaultTool[0]) {
            sfmt(cmd, sizeof(cmd), "\"%s\" \"%s\"", dob->do_DefaultTool, path);
            fmt(ed, "its icon: %.36s", dob->do_DefaultTool);
            with(ed, cmd);
        }
        FreeDiskObject(dob);
    }
    /* the config's own: OPENWITH ext program */
    for (i = 0; i < now; i++)
        if (strcmp(owext[i], "*") == 0 || nocase(owext[i], e) == 0) {
            fmt(cmd, "%s \"%s\"", owprog[i], path);
            with(owprog[i], cmd);
        }
    if (pathexists("SYS:Utilities/MultiView")) {
        fmt(cmd, "SYS:Utilities/MultiView \"%s\"%s%s", path, ownscr ? " PUBSCREEN " : "", ownscr ? PUBNAME : "");
        with("MultiView", cmd);
    }
    if (cfgeditor[0]) strcpy(ed, cfgeditor);
    else if (GetVar((STRPTR)"EDITOR", (STRPTR)ed, sizeof(ed), 0) <= 0) strcpy(ed, "C:Ed");
    fmt(cmd, "%s \"%s\"", ed, path);
    with("the editor", cmd);
    fmt(cmd, "\"%s\"", path);
    with("run it as a program", cmd);
    for (i = 0; i < nwith; i++) {
        keys[i][0] = (char)('1' + i);
        keys[i][1] = 0;
        items[2 * i] = keys[i];
        items[2 * i + 1] = wlab[i];
    }
    drawhints(items, nwith, 44);
    k = waitkey(NULL);
    drawcols();
    pv_draw();
    drawstatus();
    if (k == 13) k = '1';
    if (k >= '1' && k < '1' + nwith) {
        if (!os_launch(cur->pl.path, wcmd[k - '1'])) showfault("cannot start it");
    }
    for (i = 0; i < nwith; i++) free(wcmd[i]);
    nwith = 0;
}

/* ---- jumping by habit: the folders used most, and lately ---------------- */

#define PLACESFILE "PROGDIR:Ahiru.places"
#define MAXHAB 200
static struct hab {
    char *path;
    long count, days;
} hab[MAXHAB];
static int nhab, habdirty;

static long today(void)
{
    struct DateStamp d;
    DateStamp(&d);
    return d.ds_Days;
}

void habit_load(void)
{
    BPTR fh;
    char line[PATHMAX + 40];
    if (!(fh = Open((STRPTR)PLACESFILE, MODE_OLDFILE))) return;
    while (nhab < MAXHAB && FGets(fh, (STRPTR)line, sizeof(line))) {
        char *p = line, *q;
        long c, d;
        int l = (int)strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        c = strtol(p, &q, 10);
        if (q == p) continue;
        d = strtol(q, &p, 10);
        while (*p == ' ') p++;
        if (!*p || c <= 0) continue;
        if (!(hab[nhab].path = strdup(p))) break;
        hab[nhab].count = c;
        hab[nhab].days = d;
        nhab++;
    }
    Close(fh);
}

void habit_save(void)
{
    BPTR fh;
    char line[PATHMAX + 40];
    int i;
    if (!habdirty || !(fh = Open((STRPTR)PLACESFILE, MODE_NEWFILE))) return;
    for (i = 0; i < nhab; i++) {
        fmt(line, "%ld %ld %s\n", hab[i].count, hab[i].days, hab[i].path);
        FPuts(fh, (STRPTR)line);
    }
    Close(fh);
}

static long score(const struct hab *h)
{
    long age = today() - h->days;
    return age < 1 ? h->count * 4 : age < 7 ? h->count * 2 : age < 30 ? h->count : h->count / 2 + 1;
}

/* a folder was entered (loaddir) */
void habit_visit(const char *path)
{
    int i, low = 0;
    if (!path[0]) return;
    for (i = 0; i < nhab; i++)
        if (nocase(hab[i].path, path) == 0) {
            hab[i].count++;
            hab[i].days = today();
            habdirty = 1;
            return;
        }
    if (nhab == MAXHAB) {
        /* full: the least used goes */
        for (i = 1; i < nhab; i++)
            if (score(&hab[i]) < score(&hab[low])) low = i;
        free(hab[low].path);
        hab[low] = hab[--nhab];
    }
    if (!(hab[nhab].path = strdup(path))) return;
    hab[nhab].count = 1;
    hab[nhab].days = today();
    nhab++;
    habdirty = 1;
}

void habit_jump(void)
{
    char what[64];
    static const char *items[18];
    static char keys[9][2], labs[9][60];
    int best[9], nb = 0, i, j, k;
    what[0] = 0;
    if (!editline("jump to (part of a folder's name): ", what, sizeof(what) - 1, -1) || !what[0]) {
        drawstatus();
        return;
    }
    /* the nine best that hold it, the best first */
    for (i = 0; i < nhab; i++) {
        long sc;
        if (!nchas(hab[i].path, what)) continue;
        sc = score(&hab[i]);
        if (nb == 9 && sc <= score(&hab[best[8]])) continue;
        j = nb < 9 ? nb++ : 8;
        while (j > 0 && score(&hab[best[j - 1]]) < sc) best[j] = best[j - 1], j--;
        best[j] = i;
    }
    if (!nb) {
        say("no folder used before holds that", 1);
        return;
    }
    if (nb == 1) {
        godir(hab[best[0]].path, NULL);
        return;
    }
    for (i = 0; i < nb; i++) {
        keys[i][0] = (char)('1' + i);
        keys[i][1] = 0;
        fmt(labs[i], "%.58s", hab[best[i]].path);
        items[2 * i] = keys[i];
        items[2 * i + 1] = labs[i];
    }
    drawhints(items, nb, 64);
    k = waitkey(NULL);
    drawcols();
    pv_draw();
    drawstatus();
    if (k == 13) k = '1';
    if (k >= '1' && k < '1' + nb) godir(hab[best[k - '1']].path, NULL);
}

/* ---- the live filter: the list narrows as the letters come -------------- */

static void narrowed(const char *buf)
{
    char *seen;
    long i;
    if (buf != filter) strcpy(filter, buf);
    visual_end();                   /* the rows change under it (the marks stay) */
    dl_show(cur);                   /* (sorted once, when the filter began) */
    /* what the filter hides is not marked: delete and the rest act on
     * every mark, so a hidden one would go unseen */
    if ((seen = calloc(cur->pl.n ? cur->pl.n : 1, 1))) {
        for (i = 0; i < cur->n; i++) seen[cur->vis[i]] = 1;
        for (i = 0; i < cur->pl.n; i++)
            if (!seen[i]) cur->pl.ent[i].mark = 0;
        free(seen);
    }
    dl_clamp(cur, listh);
    drawcols();
    drawtitle();
    pvdirty = 1;
}

void live_filter(void)
{
    char was[64], f[64];
    strcpy(was, filter);
    strcpy(f, filter);
    dl_sort(cur);                   /* once: each letter after only filters */
    if (!editlive("filter: ", f, sizeof(f) - 1, narrowed)) narrowed(was);
    pv_invalidate();
    drawall();
}

void more2_exit(void)
{
    int i;
    habit_save();
    for (i = 0; i < nhab; i++) free(hab[i].path);
    for (i = 0; i < now; i++) free(owext[i]), free(owprog[i]);
    nhab = now = 0;
    if (IconBase) CloseLibrary(IconBase);
    IconBase = NULL;
}
