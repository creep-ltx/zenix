/* SPDX-License-Identifier: GPL-2.0-only */
/* displayd's own declarations (DESIGN D38 section 3-6, docs/d38-desktop.md): the screen and its bitmaps (draw.c),
 * fonts (font.c), windows, their regions, kept copies and frames (win.c), clients, the protocol and the input
 * (displayd.c). */
#pragma once
#include "libz.h"
#include "../proto.h"
typedef unsigned char u8;
typedef unsigned short u16;
typedef short s16;
typedef int s32;

/* ---- bitmaps: interleaved planes (a line holds plane 0's bytes, then plane 1's ...), as the console's ---- */
struct bm {
    u8 *p;                      /* chip RAM */
    u16 w, h;                   /* pixels */
    u16 bpr;                    /* bytes a plane's line: (w + 15) / 16 * 2 */
    u16 depth;
    u32 stride;                 /* bytes a line: bpr * depth */
    u8 kept;                    /* a kept copy: counted in chip_used */
};
struct rect { s16 x0, y0, x1, y1; };    /* x1, y1 exclusive */
#define RMAX 24                 /* rectangles a region holds; more: the rest is not drawn (too little, never over
                                 * another window: the painter draws what it holds back to front) */
struct region { u16 n; u8 over; struct rect r[RMAX]; };    /* over: pieces were dropped */

extern struct bm scr;           /* the screen */
extern int use_blit;            /* the blitter is ours (the console is shown): else the CPU draws */
extern int aga;
extern u32 chip_used, chip_budget;      /* the kept copies' chip RAM (D38 6.2: 256 KB, then simple refresh) */

/* draw.c */
void blit_idle(void);           /* the blitter's last blit ended (before letting go of it) */
void bm_fill(struct bm *b, const struct rect *r, int pen);
void bm_xor(struct bm *b, const struct rect *r, int pen);            /* invert the pen's planes */
void bm_copy(struct bm *s, int sx, int sy, struct bm *d, const struct rect *dr);   /* dr's size from (sx, sy) */
void bm_text(struct bm *b, int x, int y, const u8 *glyph, int fg, int bg, int attr, const struct rect *clip);
void bm_image(const struct bm *img, const u8 *mask, int sx, int sy, struct bm *d, const struct rect *dr);
int bm_alloc(struct bm *b, int w, int h, int depth);   /* chip RAM, 0 or -ENOMEM; a kept copy (b->kept) counted */
void bm_free(struct bm *b);

/* rectangles and regions */
int r_and(struct rect *o, const struct rect *a, const struct rect *b);  /* 1: not empty */
void rg_set(struct region *g, const struct rect *r);
void rg_sub(struct region *g, const struct rect *r);
void rg_and(struct region *g, const struct rect *r);
void rg_add(struct region *g, const struct rect *r);

/* font.c */
void font_init(const u8 *topaz);        /* the kernel's Topaz (CON_GRAB), then /usr/share/consolefonts/zenix-8x8.zcf */
const u8 *font_glyph(u32 cp);           /* 8 bytes */

/* win.c */
#define NWIN 32
struct win {
    u16 id;                     /* 0: a free slot */
    u8 kind, cl;                /* WK_*, the client's slot */
    u16 state;                  /* WST_* */
    u16 flags;
    s16 fx, fy;                 /* the frame's top left */
    u16 iw, ih;                 /* the inside */
    u16 minw, minh, maxw, maxh, stepw, steph;
    char title[DPY_TITLE];
    struct bm back;             /* the kept copy of the inside (back.p 0: none) */
    u8 simple;                  /* no copy could be kept: the program draws again (EV_EXPOSE) */
    u8 placed;                  /* the manager placed it (or the fallback did) */
    struct region vis;          /* what of the whole window (frame, shadow) is on the screen */
    struct rect clip;           /* the client's O_CLIP, inside-relative (x1 0: none) */
};
extern struct win wins[NWIN];
extern u16 order[NWIN];         /* slots, back to front */
extern int norder;
extern struct dpystyle style;
extern int focus;               /* the slot with the keyboard, -1 none */
struct win *win_get(int id);
void win_frame_rect(const struct win *w, struct rect *r);   /* the frame (border included, shadow not) */
void win_inside(const struct win *w, struct rect *r);       /* the inside on the screen */
int win_new(int cl, const struct dpywin *d);                /* -> slot, or -errno */
void win_close(int slot);
void win_place(int slot, int x, int y, int w, int h);       /* move and/or resize */
void win_stack(int slot, int back);
void win_show(int slot, int show, int why);
void win_focus(int slot);
void win_title(int slot);
void win_style(const struct dpystyle *s);
void win_draw(int slot, const u16 *o, u32 nwords, u32 *bad);    /* a DRAW batch */
int win_at(int x, int y, int *part);   /* the window under a point (front first) and the part (0 inside, FP_*) */
void win_redraw_all(void);
void win_drag(int slot, int mode, int px, int py);          /* follows the pointer until the button is let go */
void win_drag_event(int x, int y, int up);                  /* the pointer while a drag runs (displayd.c) */
extern int dragging;            /* a drag is under way: the slot + 1 */
void drag_hide(void);           /* the outline off while other drawing is done */
void drag_show(void);

/* displayd.c */
void ev_to(int cl, struct dpyev *e);                        /* an event for a client (queued, posted later) */
void ev_win(int slot, int type, int code, int arg, int x, int y, int w, int h);
void ev_wm(int type, int win, int code, int arg, int x, int y, int w, int h);
extern int wm_cl;               /* the window manager's client slot, -1 none */
const u8 *dpy_image(int id, int cl, struct bm **img);         /* an image (cl's, or any: -1) and its mask (0: none) */
void kick(void);                /* drain the kernel's ring now (a drag's loop) */
