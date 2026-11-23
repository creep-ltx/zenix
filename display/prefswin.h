/* SPDX-License-Identifier: GPL-2.0-only */
/* The preference programs' window (docs/d38-desktop.md "Preferences", docs/d61-mouse.md 7): rows of choices, each a
 * label and a gadget that a click (Shift-click backwards) or Left/Right changes, Up/Down between rows, the four
 * buttons of AmigaOS's preference editors (Test, Use, Save, Cancel), a line for messages. One layout and one key
 * handling for deskprefs and mouseprefs (display/lib/prefswin.c). */
#pragma once
#include "display.h"

#define PW_TEST 0
#define PW_USE 1
#define PW_SAVE 2
#define PW_CANCEL 3
struct prefswin {
    int win, row, nrows, ww, wh, lh, gx;    /* gx: the gadgets' left */
    int extra;                  /* lines of the program's own between the rows and the buttons (at pw_ey) */
    const char *const *label;   /* nrows labels */
    void (*value)(int row, char *buf, int n);   /* a row's value as text */
    struct dpyfont uf;          /* the desktop's font */
    char msg[64];               /* the last line (a refusal, a result) */
};
void pw_layout(struct prefswin *p, int ww);     /* the font asked, lh and wh from it */
int pw_gy(const struct prefswin *p, int row);   /* a row's top */
int pw_ey(const struct prefswin *p);            /* the program's own lines' top */
int pw_by(const struct prefswin *p);            /* the buttons' top */
int pw_bx(const struct prefswin *p, int b);     /* button b's left; pw_bw its width */
int pw_bw(const struct prefswin *p);
void pw_text(const struct prefswin *p, int x, int y, int fg, int bg, const char *s);
void pw_box(const struct prefswin *p, int x, int y, int w, int h, int light, int dark);
void pw_draw(const struct prefswin *p);         /* the rows, the buttons and the message (dpy_flush is the caller's) */
void pw_say(struct prefswin *p, const char *s);
int pw_hit(const struct prefswin *p, int x, int y);   /* a row (0..), 100 + a button, or -1 */
/* a key (EV_KEY): -> 1 + the row to cycle forwards, -(1 + row) backwards, 100 + a button, 0 nothing (Up and Down
 * have moved p->row: draw again) */
int pw_key(struct prefswin *p, const struct dpyev *e);
void pw_where(const struct prefswin *p, const char *prog);    /* -v: the gadgets' places on the screen, one line */
