/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* more - tabs, visual mode, bulk rename, find across folders, the
 * trashcan and the sound player: what Ahiru took from Yazi (10.10.2026).
 * The keys are in keys.c's table like every other. */
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <string.h>
#include "ahiru.h"
#include "fs/os.h"
#include "fs/ops.h"
#include "fs/sync.h"
#include "fs/renames.h"
#include "fs/find.h"
#include "core/util.h"
#include "paula.h"

/* ---- tabs: each remembers a place and the entry the cursor was on ------ */

#define NTABS 9
static char *tabpath[NTABS], *tabsel[NTABS];
static int ntabs = 1, curtab;

static void tabkeep(void)
{
    free(tabpath[curtab]);
    free(tabsel[curtab]);
    tabpath[curtab] = strdup(cur->pl.path);
    tabsel[curtab] = cur->n ? strdup(DNAME(cur, cur->sel)) : NULL;
}

static void tabgo(int i)
{
    if (i < 0 || i >= ntabs || i == curtab) return;
    tabkeep();
    curtab = i;
    loaddir(tabpath[i] ? tabpath[i] : "", tabsel[i]);
    drawall();
}

void tab_new(void)
{
    int i;
    if (ntabs == NTABS) {
        say("nine tabs is all", 1);
        return;
    }
    tabkeep();
    for (i = ntabs; i > curtab + 1; i--) tabpath[i] = tabpath[i - 1], tabsel[i] = tabsel[i - 1];
    tabpath[curtab + 1] = strdup(cur->pl.path);
    tabsel[curtab + 1] = tabsel[curtab] ? strdup(tabsel[curtab]) : NULL;
    ntabs++;
    curtab++;
    drawtitle();
}

void tab_close(void)
{
    int i, to;
    if (ntabs == 1) {
        say("the last tab - q quits", 1);
        return;
    }
    free(tabpath[curtab]);
    free(tabsel[curtab]);
    for (i = curtab; i < ntabs - 1; i++) tabpath[i] = tabpath[i + 1], tabsel[i] = tabsel[i + 1];
    ntabs--;
    tabpath[ntabs] = tabsel[ntabs] = NULL;
    to = curtab >= ntabs ? ntabs - 1 : curtab;
    curtab = to;
    loaddir(tabpath[to] ? tabpath[to] : "", tabsel[to]);
    drawall();
}

void tab_next(int d)
{
    if (ntabs > 1) tabgo((curtab + d + ntabs) % ntabs);
}

void tab_go(const char *arg)
{
    if (arg) tabgo(atoi(arg) - 1);
}

/* " 1 [2] 3" for the title row, "" with one tab */
void tab_label(char *dst)
{
    int i;
    dst[0] = 0;
    if (ntabs < 2) return;
    for (i = 0; i < ntabs; i++) fmt(dst + strlen(dst), i == curtab ? "[%ld]" : " %ld ", (long)i + 1);
}

const char *tab_other(void)
{
    if (ntabs < 2) return NULL;
    tabkeep();
    return tabpath[(curtab + 1) % ntabs];
}

/* compare (sync 0) or sync (1) this folder with the next tab's */
void tab_compare(int sync)
{
    static struct place other;
    int na, nb, nt = (curtab + 1) % ntabs;
    char m[160];
    if (ntabs < 2) {
        say("open a tab (t) with the other folder first: compare and sync work with the next tab", 1);
        return;
    }
    tabkeep();
    if (!tabpath[nt] || !place_go(&other, tabpath[nt])) {
        say("cannot read the other tab's folder", 1);
        return;
    }
    place_read(&other, PS_NAME, 0);
    if (sync) {
        fs_sync(&cur->pl, &other);
    } else if (fs_compare(&cur->pl, &other, &na, &nb)) {
        drawcols();
        if (!na && !nb) fmt(m, "the same as in tab %ld", (long)nt + 1);
        else fmt(m, "marked: %ld differ here; %ld more in tab %ld", (long)na, (long)nb, (long)nt + 1);
        say(m, 0);
    }
    place_free(&other);
}

/* ---- visual mode: the marks follow the cursor from where it began ------- */

static int visual;
static long vanchor;
static unsigned char *vsnap;     /* the marks as they were when it began */

int visual_on(void)
{
    return visual;
}

void visual_end(void)
{
    visual = 0;
    free(vsnap);
    vsnap = NULL;
}

static void visual_apply(void)
{
    long i, a = vanchor < cur->sel ? vanchor : cur->sel, b = vanchor < cur->sel ? cur->sel : vanchor;
    /* vsnap is by the core's index: the rows can move under it */
    for (i = 0; i < cur->n; i++) DENT(cur, i)->mark = (i >= a && i <= b) | (vsnap && vsnap[cur->vis[i]]);
    drawcols();
    drawstatus();
}

void visual_toggle(void)
{
    long i;
    if (visual) {
        visual_end();
        drawstatus();
        return;
    }
    if (!cur->n || !cur->pl.path[0]) return;
    if ((vsnap = malloc(cur->pl.n ? cur->pl.n : 1)))
        for (i = 0; i < cur->pl.n; i++) vsnap[i] = cur->pl.ent[i].mark;
    visual = 1;
    vanchor = cur->sel;
    visual_apply();
}

/* the cursor moved (main.c's setsel) */
void visual_moved(void)
{
    if (visual) visual_apply();
}

/* ---- bulk rename: the names in the editor, one a line -------------------- */

static int scratchname(char *dst, const char *tag)
{
    const char *where = pathexists("T:") ? "T:" : "RAM:";
    fmt(dst, "%sAhiru-%s-%lx", where, tag, (long)FindTask(NULL));
    return 1;
}

void bulk_rename(void)
{
    char tmp[PATHMAX], *text = NULL;
    long i, n = 0, nm = 0, len;
    int *idx = NULL, lines;
    char **newn = NULL;
    BPTR fh;
    if (cur->pl.arc || !cur->pl.path[0]) {
        say("bulk rename works in a folder on a disk", 1);
        return;
    }
    for (i = 0; i < cur->n; i++) nm += DENT(cur, i)->mark != 0;
    /* the marked ones, else every one shown */
    if (!(idx = malloc((cur->n + 1) * sizeof(int)))) return;
    for (i = 0; i < cur->n; i++)
        if (!nm || DENT(cur, i)->mark) idx[n++] = cur->vis[i];
    if (!n) goto out;
    scratchname(tmp, "names");
    if (!(fh = Open((STRPTR)tmp, MODE_NEWFILE))) {
        say("cannot write the list of names", 1);
        goto out;
    }
    for (i = 0; i < n; i++) {
        FPuts(fh, (STRPTR)cur->pl.ent[idx[i]].name);
        FPutC(fh, '\n');
    }
    Close(fh);
    if (!op_editpath(tmp)) goto gone;
    /* read back: the same number of lines, in the same order */
    if (!(fh = Open((STRPTR)tmp, MODE_OLDFILE))) goto gone;
    Seek(fh, 0, OFFSET_END);
    len = Seek(fh, 0, OFFSET_BEGINNING);
    if (len < 0 || !(text = malloc(len + 1))) {
        Close(fh);
        goto gone;
    }
    len = Read(fh, text, len);
    Close(fh);
    text[len > 0 ? len : 0] = 0;
    if (!(newn = malloc(n * sizeof(char *)))) goto gone;
    {
        char *p = text;
        for (lines = 0; *p && lines <= n; lines++) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            if (lines < n) newn[lines] = p;
            if (!e) {
                lines++;
                break;
            }
            p = e + 1;
        }
    }
    if (lines != n) {
        say("the number of lines changed - nothing was renamed", 1);
        goto gone;
    }
    /* the core checks every new name, then renames in two steps (so two
     * names can swap), icons along */
    {
        char (*nn)[NAMEMAX + 4] = malloc(n * sizeof(*nn));
        if (nn) {
            for (i = 0; i < n; i++) {
                int l = (int)strlen(newn[i]);
                while (l && (newn[i][l - 1] == '\r' || newn[i][l - 1] == ' ')) newn[i][--l] = 0;
                fmt(nn[i], "%.107s", newn[i]);
            }
            fs_renameset(&cur->pl, idx, nn, (int)n);
            free(nn);
        }
    }
gone:
    DeleteFile((STRPTR)tmp);
out:
    free(newn);
    free(text);
    free(idx);
}

/* ---- find across folders: the hits shown as a folder of their own ------ */

#define FINDMAX 1000
static char findwhat[64];
static int findtext;            /* the hits are files holding the text */
static int results;
static long nhits;

int results_on(void)
{
    return results;
}

void results_end(void)
{
    results = 0;
}

/* a hit from the core's walk (fs/find.h): an entry of the hits folder,
 * named by its path from the root ("sub/name") */
static int addfound(void *ctx, const struct findhit *h)
{
    (void)ctx;
    if (nhits >= FINDMAX) return 0;
    if (place_add(&cur->pl, h->rel, h->e->type > 0 ? KD_DIR : KD_FILE, h->e->type > 0 ? 0 : h->e->size,
                  h->e->days * 1440 + h->e->mins)) {
        cur->pl.ent[cur->pl.n - 1].prot = h->e->prot;
        nhits++;
    }
    return 1;
}

void find_run(int text)
{
    char root[PATHMAX], m[160];
    unsigned long seen;
    int r;
    if (!cur->pl.path[0] || cur->pl.arc) {
        say("finding works in a folder on a disk", 1);
        return;
    }
    findwhat[0] = 0;
    if (!editline(text ? "search the files for: " : "find (name or #? pattern): ", findwhat,
                  sizeof(findwhat) - 1, -1) || !findwhat[0]) {
        drawstatus();
        return;
    }
    findtext = text;
    /* the folder's own entries give way to the hits */
    strcpy(root, cur->pl.path);
    visual_end();
    cur->pl.n = 0;
    nhits = 0;
    r = fs_find(root, findwhat, text ? FIND_FILES : FIND_NAMES, addfound, NULL, &seen);
    if (r == -1) {
        say("that is not a pattern AmigaDOS understands", 1);
        reloadall(NULL);
        return;
    }
    results = 1;
    cur->sel = cur->top = 0;
    dl_sort(cur);
    pv_invalidate();                /* the preview: the first hit, not the folder */
    drawall();
    if (r != 1) fmt(m, "stopped - %ld found", (long)cur->n);
    else fmt(m, "%ld found in %lu looked at - h or R leaves the list", (long)cur->n, seen);
    say(m, 0);
}

/* the title row's tag while the hits show */
void results_label(char *dst)
{
    if (results) fmt(dst, "%s: %.30s", findtext ? "text" : "found", findwhat);
    else dst[0] = 0;
}

/* ---- the trashcan: <volume>:Trashcan, as Workbench has it ---------------- */

static struct place tplace;

void trash_run(void)
{
    char tp[PATHMAX], ic[PATHMAX], *c;
    BPTR l;
    long i, tl;
    if (!cur->n || !cur->pl.path[0] || cur->pl.arc) {
        say("the trashcan takes files from a folder on a disk", 1);
        return;
    }
    /* find results ("sub/name") would land in the trash as such */
    for (i = 0; i < cur->pl.n; i++)
        if ((cur->pl.ent[i].mark || i == DIDX(cur)) && strchr(cur->pl.ent[i].name, '/')) {
            say("go to the file's own folder first (Return on a hit)", 1);
            return;
        }
    strcpy(tp, cur->pl.path);
    if (!(c = strchr(tp, ':'))) return;
    strcpy(c + 1, "Trashcan");
    /* the trashcan itself, or a folder inside it */
    tl = (long)strlen(tp);
    for (i = 0; i < tl && cur->pl.path[i]; i++) {
        int x = (unsigned char)cur->pl.path[i], y = (unsigned char)tp[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) break;
    }
    if (i == tl && (cur->pl.path[tl] == 0 || cur->pl.path[tl] == '/')) {
        say("this is the trashcan - dD deletes for good", 1);
        return;
    }
    if (isdirpath(tp) != 1) {
        /* none yet: made, with the system's trashcan icon when there is one */
        if (!(l = CreateDir((STRPTR)tp))) {
            showfault("cannot make the trashcan");
            return;
        }
        UnLock(l);
        strcpy(ic, tp);
        strcat(ic, ".info");
        if (cbuf_get()) {
            if (pathexists("ENV:Sys/def_trashcan.info")) copyfile("ENV:Sys/def_trashcan.info", ic);
            else if (pathexists("ENVARC:Sys/def_trashcan.info")) copyfile("ENVARC:Sys/def_trashcan.info", ic);
            cbuf_free();
        }
    }
    if (!place_go(&tplace, tp)) return;
    tplace.fail = 0;
    /* a move: the core's rules (a name already in the trash asks) */
    opmsg[0] = 0;
    fs_copy(&cur->pl, &tplace, DIDX(cur), 1, 0);
    place_free(&tplace);
    if (!msgup) {
        char m[PATHMAX + 30];
        fmt(m, "moved to %s", tp);
        say(m, 0);
    }
}

/* ---- sound: a module or 8SVX plays in the background -------------------- */

static int playing;
static char playname[60];

void play_toggle(void)
{
    char path[PATHMAX], why[100];
    if (playing) {
        snd_free();
        playing = 0;
        if (cur->n && strcmp(playname, DNAME(cur, cur->sel)) == 0) {
            say("stopped", 0);
            return;
        }
    }
    if (!cur->n || (dflags(cur, cur->sel) & F_DIR)) return;
    if (cur->pl.arc) {
        say("copy it out of the archive to play it", 1);
        return;
    }
    selpath(path);
    if (!snd_load(path, SND_NONE, why, sizeof(why)) || !snd_start(why, sizeof(why))) {
        snd_free();
        say(why, 1);
        return;
    }
    playing = 1;
    fmt(playname, "%.58s", DNAME(cur, cur->sel));
    drawstatus();
}

/* the status row's tag while it plays ("" when not) */
void play_label(char *dst)
{
    dst[0] = 0;
    if (playing) {
        struct sndstat st;
        snd_status(&st);
        if (st.ended && st.kind == SND_SVX) {
            snd_free();
            playing = 0;
            return;
        }
        fmt(dst, "playing %.30s  ", playname);
    }
}

void more_exit(void)
{
    int i;
    if (playing) snd_free();
    playing = 0;
    visual_end();
    for (i = 0; i < NTABS; i++) free(tabpath[i]), free(tabsel[i]), tabpath[i] = tabsel[i] = NULL;
}
