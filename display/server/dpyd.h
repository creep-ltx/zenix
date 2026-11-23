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

extern struct bm scr;           /* the screen (on a card, D43: the pens' bitmap in fast RAM, shown by fb_show) */
#define CHUNKY 8                /* a bitmap's depth that means a byte a pixel, the pen (D43: a card's screen, its kept
                                 * copies); planar bitmaps have 1-4 planes */
#ifdef NO_RTG                   /* the demo floppy's displayd (`make demo`): its bootblock writes no card record (D43
                                 * 3), so fb.c and every card branch are left out: rtg is the constant 0 */
#define rtg 0
#define CHUNKY_BM(b) 0
static inline void fb_show(void) {}
static inline void fb_all(void) {}
static inline void fb_pointer(int x, int y) { (void)x; (void)y; }
static inline void fb_pointer_image(const u16 *a, const u16 *b) { (void)a; (void)b; }
static inline void fb_ptr_show(void) {}
#else
extern int rtg;                 /* the screen is a graphics card's (DESIGN D43, docs/d43-rtg.md) */
#define CHUNKY_BM(b) ((b)->depth == CHUNKY)     /* a byte a pixel: only on a card (its screen, its kept copies) */
void fb_show(void);             /* what was drawn on the screen since the last show onto the card, and the pointer */
void fb_all(void);              /* the whole screen to be shown again (the pens changed) */
void fb_pointer(int x, int y);  /* the pointer's place on the card (its picture's top left) */
void fb_pointer_image(const u16 *a, const u16 *b);     /* its picture: 16 rows of two planes (kept, not copied) */
void fb_ptr_show(void);         /* D61: only the pointer's old and new place onto the card, now */
extern void (*fb_band_hook)(void);      /* D61: called between fb_show's bands of 32 lines */
#endif
struct fbcard;
extern struct fbcard card;      /* that card (fb.h) */
extern int use_blit;            /* the blitter is ours (the console is shown): else the CPU draws */
extern int aga;
extern int lace;               /* the screen is interlaced (D38 "Screen modes"): line y on frame line y / 2 */
extern int frame_l;            /* a frame's lines: 312 PAL, 262 NTSC */
extern u32 chip_used, chip_budget;      /* the kept copies' chip RAM (D38 6.2: 256 KB, then simple refresh) */
extern u32 nblits, npix, nglyphs, ncopies;  /* DPY_STAT's counters (draw.c) */
extern u32 ndraws, npaints;             /* DPY_DRAW batches (displayd.c), damage painted (win.c) */
extern u32 nmoves, tmoves;              /* DPY_STAT's drag counters: a whole window's moves, their E-clock ticks (win.c) */
extern u32 nshows, tshows, twaits, nlate;   /* shows onto the screen on view, their ticks, the ticks waited for the
                                         * beam, shows that missed the beam's rule (draw.c) */

/* draw.c */
void blit_idle(void);           /* the blitter's last blit ended (before letting go of it) */
void bm_fill(struct bm *b, const struct rect *r, int pen);
void bm_xor(struct bm *b, const struct rect *r, int pen);            /* invert the pen's planes */
void bm_copy(struct bm *s, int sx, int sy, struct bm *d, const struct rect *dr);   /* dr's size from (sx, sy) */
void bm_text(struct bm *b, int x, int y, const u8 *glyph, int fg, int bg, int attr, const struct rect *clip);
void bm_mask(struct bm *b, int bx0, int y0, int rows, const u8 *mask, int ms, int x0, int x1, int fg, int bg);
void bm_image(const struct bm *img, const u8 *mask, int sx, int sy, struct bm *d, const struct rect *dr);
int bm_alloc(struct bm *b, int w, int h, int depth);
extern int shadowing;           /* a whole window's drag: scr is a drawn screen, shown once a pass (D38 6.3) */
int shadow_begin(void);         /* 1: drawing goes to the drawn screen from now on; 0: no chip RAM for it */
void shadow_show(void);         /* what changed onto the screen on view, timed to the beam */
void shadow_end(void);
int dpy_beam(void);             /* the display line the beam is on (0 the screen's first) */          /* shown, freed: drawing on the screen on view again */   /* chip RAM, 0 or -ENOMEM; a kept copy (b->kept) counted */
void bm_free(struct bm *b);

/* rectangles and regions */
int r_and(struct rect *o, const struct rect *a, const struct rect *b);  /* 1: not empty */
void rg_set(struct region *g, const struct rect *r);
void rg_sub(struct region *g, const struct rect *r);
void rg_and(struct region *g, const struct rect *r);
void rg_add(struct region *g, const struct rect *r);

/* font.c: font 0 the 8 x 8 (the terminals' cells), font 1 (DPYF_UI) the desktop's, an AmigaOS bitmap font when one
 * is set (/etc/zdesk/screen's "font NAME SIZE"), else the 8 x 8 too */
void font_init(const u8 *topaz);        /* the kernel's Topaz (CON_GRAB), then /usr/share/consolefonts/zenix-8x8.zcf */
const u8 *font_glyph(u32 cp);           /* 8 bytes */
int font_ui_load(const char *name, int size);   /* 0, or -errno (said on standard error: the font in use stays) */
void font_ui_drop(void);                /* the desktop's font the 8 x 8 again */
int font_id(int f);                     /* the font O_TEXT's word asks for: 1 the desktop's if it is loaded, else 0 */
int font_h(int f);                      /* its lines */
int font_w(int f, const u8 *s, u32 n);  /* n bytes of UTF-8: their width in pixels */
u32 font_fit(int f, const u8 *s, u32 n, int maxw, int *w);  /* the bytes of whole characters within maxw, *w theirs */
void font_run(struct bm *b, int x, int y, int f, const u8 *s, u32 n, int fg, int bg, int attr, const struct rect *clip);
void font_info(int f, struct dpyfont *o);       /* DPY_FONT_INFO's answer */

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
    char *menu;                 /* DPY_MENU's text (malloc'd), 0 none */
    u16 nmenu;
    u8 lifted;                  /* in playfield 2 for the chips' drag: not shown on the screen's planes */
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
void win_screen(void);         /* a new screen (DPY_MODE): windows kept on it, all drawn again */
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
extern int drag_moved;
/* displayd.c: the chips' drag (D38 "Dragging with the chips"): a window's bitmap b in playfield 2 (AGA) */
int hw_ok(void);                /* AGA, 4 planes, the blitter ours */
void hw_show(const struct bm *b, const u8 *zero, int x, int y, int pen);  /* at (x, y), from the next frame; pen: pen 0's
                                 * stand-in in b (-1 none) */
void hw_off(void);              /* playfield 2 off, in effect on return */
void hw_wait(void);             /* the last change in effect */          /* the pointer moved during a drag: one move a pass of the main loop */

/* displayd.c, for D55's screens (screen.c, docs/d55-screens.md) */
extern int gfd;                 /* the grab's descriptor (CON_HOLD) */
extern int base_w, base_h;      /* the console's size: a field's lines */
extern u16 *copl[2];            /* the fixed parts (a screen's list names them in COP1LC) */
extern u16 *scr_tramp;          /* 4 screens x 2 buffers x 2 fields x 6 words, in the jump word's 64 KB */
extern u8 *scr_zero;            /* 96 zero bytes, 8-aligned, chip RAM */
void cop_show(const u16 *t0, const u16 *t1);    /* the jump words at these trampolines (0: the desktop's lists) */
void cop_settle(void);          /* until the Copper runs only what the last flip named */
int dpy_peer(struct umsg *m, int op, u32 addr, u32 len);
void input_reset(void);
/* screen.c (not in the demo floppies' displayd, -DNO_SCREEN: no program there opens a screen of its own) */
#ifdef NO_SCREEN
static inline int scr_req(struct umsg *m, int cl) { (void)m; (void)cl; return -ENOSYS; }
static inline void scr_drop(int cl) { (void)cl; }
static inline void scr_gone(u32 base) { (void)base; }
static inline int scr_shown(void) { return 0; }
static inline void scr_focus(int in) { (void)in; }
static inline int scr_key(const struct ugevent *e) { (void)e; return 0; }
static inline int scr_pointer(const struct ugevent *e, u16 old, u16 now, int moved)
{
    (void)e; (void)old; (void)now; (void)moved;
    return 0;
}
#else
int scr_req(struct umsg *m, int cl);    /* DPY_SCREEN, DPY_SCR_SET, DPY_SCR_CLOSE */
void scr_drop(int cl);
void scr_gone(u32 base);        /* GEV_GONE */
int scr_shown(void);            /* a screen is in front */
void scr_focus(int in);         /* the console shown again (1), switched away (0) */
int scr_key(const struct ugevent *e);   /* 1: taken (Left Amiga+N, a screen in front) */
int scr_pointer(const struct ugevent *e, u16 old, u16 now, int moved);
#endif
