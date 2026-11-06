/* SPDX-License-Identifier: GPL-2.0-only */
/* libdisplay (DESIGN D38 5, docs/man/libdisplay.txt): a program's side of the display protocol (display/proto.h).
 * One connection a program. Drawing orders are queued in the program's own memory and sent as one DPY_DRAW per
 * window when dpy_flush is called, the buffer is full, or another window is drawn in; events come from the ring
 * displayd writes into (dpy_next, or the program's own poll on dpy_port() with POLLPORT | POLLRDBAND). The library
 * keeps nothing of what was drawn: a program redraws on EV_EXPOSE from its own model. */
#pragma once
#include "proto.h"

int dpy_open(const char *name);         /* 0, or -errno (displayd started on demand: the D11 service "display") */
void dpy_close(void);                   /* DPY_BYE: the windows go */
extern unsigned short dpy_w, dpy_h, dpy_depth;  /* the screen */

/* windows */
int dpy_window(int kind, const char *title, int x, int y, int w, int h, int flags);    /* -> its id, or -errno */
int dpy_newwin(struct dpywin *d);       /* the whole request; filled back */
int dpy_title(int win, const char *title);
int dpy_resize(int win, int w, int h);
int dpy_destroy(int win);
int dpy_info(int win, struct dpyinfo *i);
int dpy_menu(int win, const char *menus, int n);   /* "Title\tItem\tItem\nTitle..." (n bytes): picks come as EV_MENU */

/* drawing (queued) */
void dpy_clip(int win, int x, int y, int w, int h);
void dpy_fill(int win, int x, int y, int w, int h, int pen);
void dpy_xor(int win, int x, int y, int w, int h);
void dpy_text(int win, int x, int y, int fg, int bg, int attr, const char *s, int n);  /* n bytes of UTF-8 */
void dpy_copy(int win, int sx, int sy, int w, int h, int dx, int dy);
void dpy_image(int win, int id, int sx, int sy, int w, int h, int dx, int dy);
int dpy_flush(void);                    /* 0, or -errno (-EINVAL: an order was refused) */

/* images and colours */
int dpy_image_new(int w, int h, int depth, int flags, const void *planes);    /* -> its id */
int dpy_image_free(int id);
int dpy_palette(unsigned short *pal, int n);    /* -> the screen's depth */
int dpy_mode(int mode);                  /* DPYM_HIRES, DPYM_LACE (DPYM_ASK: only asked) -> the mode now, -errno;
                                         * every client gets EV_SCREEN and dpy_w, dpy_h follow (D38 "Screen modes") */

/* events */
int dpy_port(void);                     /* poll it with POLLPORT | POLLRDBAND */
int dpy_next(struct dpyev *e, int ticks);       /* 1 an event, 0 none in time (ticks -1: wait), -errno */
int dpy_lost(void);                     /* the ring was full and events were dropped since the last call */

/* the window manager's (display/proto.h DPY_WM_*) */
int dpy_call(int type, unsigned a0, unsigned a1, unsigned a2, unsigned a3);
