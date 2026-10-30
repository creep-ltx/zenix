/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* cx - a program as a commodity (OS 2.0+): a hotkey that pops it up,
 * Exchange's show/hide/quit, one copy at a time (a second start pops the
 * first up). And the drawer Workbench has active, asked over Workbench's
 * ARexx port (3.x), so the program can open right there. */
#ifndef CF_CX_H
#define CF_CX_H

#include <exec/types.h>

#define CX_NONE 0
#define CX_POPUP 1              /* the hotkey, Exchange's Show, or a second start */
#define CX_HIDE 2
#define CX_QUIT 3

/* FALSE when there are no commodities (1.3) or when one runs already
 * (*dup set: it has been told to pop up) */
int cx_open(const char *name, const char *title, const char *descr, const char *hotkey, int *dup);
int cx_hotkeyok(void);          /* the hotkey was understood */
ULONG cx_sig(void);
int cx_poll(void);              /* what came: CX_* */
void cx_close(void);
/* CX_POPUP=NO and CX_POPKEY=... from the program's icon, when started
 * from Workbench (wbstartup is the WBStartup message, else nothing) */
void cx_tooltypes(const void *wbstartup, int *popup, char *hotkey, int size);

/* the drawer Workbench has active ("Work:Utils"); FALSE when none
 * (the backdrop, no ARexx, before 3.x) */
int wb_activedir(char *dst, int size);

#endif
