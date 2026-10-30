/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* fe - Ahiru's side of the core (fs/fe.h): the core's messages and
 * questions on the status row, its progress as a bar there, Esc. */
#include <string.h>
#include <stdlib.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include "ahiru.h"
#include "fs/fe.h"
#include "fs/ops.h"
#include "core/util.h"

void showmsg(const char *s)
{
    if (job_mine()) {
        if (s != opmsg) sfmt(opmsg, sizeof(opmsg), "%s", s);
        job_say(s);
        return;
    }
    /* the core reads opmsg back as "the last thing said" */
    if (s != opmsg) sfmt(opmsg, sizeof(opmsg), "%s", s);
    say(s, 0);
}

int askkey(const char *q)
{
    if (job_mine()) return job_ask(q);
    return ask(q);
}

/* the core's line questions; a rename started by cw, a, A or I puts
 * the cursor where that key says (renamemode, from ops.c) */
int lineinput(const char *prompt, char *buf, int max, int names)
{
    if (job_mine()) return job_line(prompt, buf, max);
    int cpos = -1, r;
    (void)names;
    if (renamemode == 0) {
        buf[0] = 0;
    } else if (renamemode == 1) {
        const char *e = extof(buf);
        cpos = *e ? (int)(e - buf) - 1 : (int)strlen(buf);
    } else if (renamemode == 3) {
        cpos = 0;
    }
    r = editline(prompt, buf, max, cpos);
    if (!r) drawstatus();
    return r;
}

/* a preview: the lines in the viewer, then the question */
int asklist(const char *q, const char **items, int n)
{
    long len = 1;
    int i, k;
    char *t;
    for (i = 0; i < n; i++) len += (long)strlen(items[i]) + 1;
    if ((t = malloc(len))) {
        t[0] = 0;
        for (i = 0; i < n; i++) strcat(t, items[i]), strcat(t, "\n");
        pager(q, (unsigned char *)t, len - 1, TY_TEXT);
        free(t);
        drawall();
    }
    k = ask("rename them as the list showed? (y/n)");
    return k == 'y';
}

void askdone(void)
{
    if (job_mine()) return;
    msgup = 0;
    drawstatus();
}

int askwidth(void)
{
    return ncols;
}

/* ---- the bar: the bottom row, redrawn only when it grows ------------ */

static ULONG ptotal, pdone;
static int pon, pcells, ppct = -1;

static void bar(void)
{
    char row[MAXCOLS];
    int w = ncols - 22, filled, pct, i;
    ULONG t = ptotal, d = pdone;
    if (w < 10) w = 10;
    if (w > MAXCOLS - 30) w = MAXCOLS - 30;
    while (t > 0xFFFFFF) t >>= 1, d >>= 1;      /* keep the multiply in range */
    if (t < 1) t = 1;
    if (d > t) d = t;
    pct = (int)(d * 100 / t);
    filled = (int)(d * w / t);
    if (filled == pcells && pct == ppct) return;
    pcells = filled;
    ppct = pct;
    row[0] = '[';
    for (i = 0; i < w; i++) row[1 + i] = i < filled ? '#' : '.';
    fmt(row + 1 + w, "] %3d%%  Esc stops", pct);
    clearrow(nrows - 1, 0, ncols);
    putrole(1, nrows - 1, row, (int)strlen(row), RO_ACC);
    msgup = 1;
}

void progshow(ULONG total)
{
    if (job_mine()) {
        job_progshow(total);
        return;
    }
    ptotal = total ? total : 1;
    pdone = 0;
    pon = 1;
    pcells = ppct = -1;
    bar();
}

void progadd(ULONG n)
{
    if (job_mine()) {
        job_progadd(n);
        return;
    }
    if (!pon) return;
    pdone += n;
    bar();
}

void progoff(void)
{
    if (job_mine()) return;
    pon = 0;
}

/* the preview unpacking a picture: any key waiting stops it, and stays
 * there for the main loop (nothing is taken from the window) */
int pvpeek, peekstop;

int checkabort(void)
{
    if (job_mine()) {
        if (job_cancelled()) abortop = 1;
        return abortop;
    }
    if (pvpeek) {
        if (keywaiting()) abortop = peekstop = 1;
        return abortop;
    }
    if (cancelok && ui_abort()) abortop = 1;
    return abortop;
}

int escpressed(void)
{
    if (job_mine()) return job_cancelled();
    return ui_abort();
}

void placechanged(struct place *P, const char *land, int how)
{
    if (job_mine()) {
        job_changed();                  /* the main loop reads again when it ends */
        return;
    }
    (void)how;
    /* every column is read again (the core may have changed the folder
     * shown in any of them); the cursor lands on the name in the
     * current one */
    reloadall(P == &cur->pl && land && land[0] ? land : NULL);
}
