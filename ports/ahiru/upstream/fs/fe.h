/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* fe - what a program on the core must provide: the front end.
 *
 * The core never draws. When it has something to say, a question to
 * ask, or a bar to move, it calls these; CFile has them in src/fe.c,
 * UP-File in its own. Plain functions, linked in. (docs/core.md) */
#ifndef CF_FE_H
#define CF_FE_H

#include <exec/types.h>
#include "shared.h"

struct place;

void showmsg(const char *s);            /* a message line */
/* a question; returns the key that answered it (27 = Esc). The core
 * words it ("delete 3 marked entries? (y)es (n)o") */
int askkey(const char *q);
/* edit a line: 1 Return, 0 Esc. names: / and : refused */
int lineinput(const char *prompt, char *buf, int max, int names);
void askdone(void);                     /* a question ended, nothing to say */
/* a list to look at before saying yes (a preview): 1 = yes, 0 = no */
int asklist(const char *q, const char **items, int n);
int askwidth(void);                     /* how many columns a question may take */

/* the progress bar (total in any unit; the core adds as it goes) */
void progshow(ULONG total);
void progadd(ULONG n);
void progoff(void);

/* Esc: while cancelok is set, checkabort() looks for it and raises
 * abortop; escpressed() is a look without the flag (a tool running) */
int checkabort(void);
int escpressed(void);

/* what was shown changed: read it again.
 *   how 0: P (and anything showing the same folder)
 *       1: every place      2: every place, the progress box was up
 *   land: the name to put the cursor on in P (NULL: stay, "": the top) */
void placechanged(struct place *P, const char *land, int how);

#endif
