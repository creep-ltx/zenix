/* SPDX-License-Identifier: GPL-2.0-only */
/* zdesk, the floating window manager of Zenix Desktop (DESIGN D38 steps 5, revision 2; docs/man/zdesk.txt): its
 * settings and theme (conf.c), the bar, its menus and blocks (bar.c), the desktop's icons and the About window
 * (desk.c), the windows, workspaces and keys (zdesk.c). */
#pragma once
#include "libz.h"
#include "../display.h"
typedef unsigned char u8;
typedef unsigned short u16;

/* ---- the theme (conf.c): a style for displayd, the roles of its pens, the bar's defaults ---- */
enum { R_DESK, R_BAR, R_BARLINE, R_BARFG, R_LOGO, R_BARCUR, R_BARCURBG, R_BARDIM, R_CLOCK, R_ICONFG, R_LABELBG,
       R_IK, R_IF, R_IA, R_IW, R_ID, R_WINBG, R_WINFG, R_BTN, R_BTNFG, R_MENUBG, R_MENUFG, R_SELBG, R_SELFG, R_N };
#define M_CLICK 1
#define M_HOLD  2
#define NGAD 3
struct theme {
    char name[16];
    struct dpystyle st;
    u8 role[R_N];               /* 255: none (R_LABELBG) */
    u8 map8[16];                /* a pen as 8 colours show it */
    u8 menus;                   /* M_CLICK | M_HOLD */
    int drag;                   /* -1: by the CPU */
    u8 gadw[NGAD], gadh[NGAD];  /* the gadgets' art */
    char gad[NGAD][12][16];
    char bar[3][160];           /* the bar's zones when no bar file says */
};
extern struct theme th;
int pen(int role);              /* a role's pen on this screen */

/* ---- settings (conf.c) ---- */
#define NKEY 48
enum { A_TERMINAL = 1, A_RUN, A_CLOSE, A_MIN, A_NEXT, A_MENU, A_MOVE, A_RESIZE, A_WORKSPACE, A_SENDTO, A_LOGOUT,
       A_ABOUT, A_ZOOM };
struct key { u8 code; u16 mask, qual; u8 action; short a, b; char *cmd; };
extern struct key keys[NKEY];
extern int nkeys;
#define NITEM 16
struct item { char *label, *cmd; char art[8]; };    /* a menu entry or a desktop icon */
extern struct item menu_items[NITEM], icons[NITEM];
extern int nmenu_items, nicons;
/* the bar's modules */
enum { B_ZENIX = 1, B_MENUS, B_WINDOWS, B_WORKSPACES, B_CLOCK, B_MEMORY, B_NETWORK, B_CPU, B_CMD };
#define NBLOCK 12
struct block {
    u8 zone, kind;              /* 0 left, 1 middle, 2 right; B_* */
    u16 every;                  /* seconds (B_CMD: at least 1, 10 when not given) */
    char *cmd;
    char text[48];
    u32 due;                    /* the tick it runs next */
    int pid, fd;                /* a command running: its pid and its output's pipe */
    u32 started;
    char out[48];
    int nout;
};
extern struct block blocks[NBLOCK];
extern int nblocks;
int conf_load(const char *theme);   /* 0, or -1 (said on standard error) */
extern unsigned mouse_acc;          /* the file mouse: DPY_WM_MOUSE's argument */
int conf_theme(const char *theme);  /* the theme alone (dlogin's greeter): 0, or -1 (said) */
const char *conf_file(const char *name, char *buf);  /* ~/.config/zdesk/NAME if there, else /etc/zdesk/NAME */

/* ---- windows (zdesk.c) ---- */
#define NZW 32
struct zw {
    int id, ws;                 /* 0 free; its workspace 1..9 */
    u8 min, zoomed, own;        /* minimised; maximised; one of zdesk's own */
    short ox, oy;               /* the place before zoom */
    unsigned short ow, oh;
    int pid;
    char title[DPY_TITLE];
};
extern struct zw zws[NZW];
extern int cur_ws, focus_id, bar_id, desk_id, bar_h, depth, verbose;
/* the desktop's font (DPY_FONT_INFO DPYF_UI: /etc/zdesk/screen's "font NAME SIZE", or the 8 x 8): the bar, the menus,
 * the labels; fh its height. Taller than 8 lines, the bar and the title bars grow by the difference */
extern struct dpyfont uf;
extern int fh;
int tw(const char *s, int n);   /* n bytes of UTF-8: their width in it */
void utext(int win, int x, int y, int fg, int bg, int attr, const char *s, int n);
extern volatile int quit;
void own_window(int id, const char *title, int x, int y, int w, int h);   /* one of zdesk's own: placed, focused */
struct zw *zw_get(int id);
void focus_win(int id);
void run(const char *cmd);      /* a command line, through /bin/sh -c */
void logout(void);
void about(void);

/* bar.c */
void bar_init(void);
const char *cpu_name(void);     /* "68060", "Emu68 68040" ...: the kernel's (ZINE_INFO) */
void bar_draw(void);
void bar_tick(void);            /* the blocks' times */
void bar_poll(struct pollfd *p, int *n);    /* the blocks' pipes into a poll list */
void bar_event(const struct dpyev *e);      /* the bar's own window, or a held menu */
int menu_open(void);            /* a menu is shown */
void menu_key(int code);        /* the menu's keys (the arrows, Return, Esc) */
void menu_keyboard(void);       /* Amiga+Space: the Zenix menu, for the keyboard */
void menu_close(void);
void menu_hold(int button);     /* the right button held somewhere: the menus at the bar, the Workbench way */
int menu_win(int id);           /* a window of the menus */

/* desk.c */
void desk_init(void);
void desk_draw(int x, int y, int w, int h);
void desk_event(const struct dpyev *e);
int about_id(void);
void about_event(const struct dpyev *e);
void icon_image(u8 *planes, const char *const *art, int rows, const u8 *pens, int depth);
