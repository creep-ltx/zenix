/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* Ahiru - a Ranger/Yazi-style file manager for AmigaOS 2.04+.
 * Shared state and the functions the files call across.
 *
 * The core in the CFile repo does the work (core/, fs/): a list here is
 * a place there - a folder, the volume list, or a folder inside an
 * archive or disk image - plus what this screen shows of it. */
#ifndef AHIRU_H
#define AHIRU_H

#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <graphics/text.h>
#include "core/core.h"
#include "fs/place.h"

#define PATHMAX   CPATH        /* the core's longest path */
#define PUBNAME   "AHIRU"
#define MAXCOLS   256

/* entry flags (dflags() works them out from the core's entry) */
#define F_DIR     1
#define F_MARK    2
#define F_VOL     4
#define F_ASN     8
#define F_EXEC    16     /* plain name, executable or script bit */
#define F_LINK    32     /* a hard or soft link */

/* colour roles: every drawn thing asks for a role, the active
 * palette (4, 8 or 16 colours) says which pen plays it */
enum { RO_TEXT, RO_HI, RO_DIR, RO_EXEC, RO_ARCH, RO_IMG, RO_AUD,
       RO_DIM, RO_ACC, RO_MARK, RO_ERR, RO_INFO, RO_SRC, RO_ASN,
       RO_LINK, RO_SCRIPT, RO_HIDDEN, RO_COUNT };

/* sort modes: the core's (PS_*), always in natural order */
enum { SO_NAME = PS_NAME, SO_SIZE = PS_SIZE, SO_DATE = PS_DATE, SO_EXT = PS_EXT };

/* read flags */
#define RD_HIDE   1      /* skip .info when hideinfo is on */
#define RD_FILTER 2      /* apply the live filter */
#define RD_SORT   4

typedef struct {
    struct place pl;     /* what the core reads and works on */
    int *vis;            /* the entries shown (.info hidden, the filter) */
    long n, viscap;      /* how many are shown */
    long sel, top;       /* in the shown ones */
    int ok;              /* FALSE: could not be read */
    int how;             /* the RD_* it was read with */
} DList;

#define DENT(l, i) (&(l)->pl.ent[(l)->vis[i]])
#define DNAME(l, i) (DENT(l, i)->name)
#define DSIZE(l, i) ((long)DENT(l, i)->size)
#define DDATE(l, i) ((long)DENT(l, i)->date)
#define DIDX(l) ((l)->n ? (l)->vis[(l)->sel] : -1)     /* the core's entry under the cursor */


/* ---- list.c */
DList *dl_new(void);
void dl_free(DList *l);
long dflags(DList *l, long i);   /* F_* | prot << 16 */
void dl_sort(DList *l);
void dl_show(DList *l);          /* filter again, no sort */
int dl_read(DList *l, const char *path, int how);
long dl_find(DList *l, const char *name);
void dl_clamp(DList *l, int rows);
void joinpath(char *dst, const char *dir, const char *name);
void parentpath(char *dst, const char *path);
const char *lastpart(const char *path);
int canonical(const char *path, char *dst);
int pathexists(const char *path);
int isdirpath(const char *path);
APTR quietreq(void);
void loudreq(APTR old);

/* ---- ui.c */
extern struct Screen *scr;
extern struct Window *win;
extern struct RastPort *rp;
extern int ownscr, colors, cw, ch, ncols, nrows, listh;
extern int c0x, c0w, c1x, c1w, c2x, c2w;
extern const int *rolepen, *ansipen;
extern unsigned char rowbuf[MAXCOLS], rowattr[MAXCOLS];
extern int msgup, breakseen;
extern char cfgfont[64];
extern int cfgedcon;               /* ops.c: EDITORCONSOLE */

/* how Ahiru shows itself: its own screen, a Workbench window, or inside
 * the terminal it was started from (term.c). AUTO: inside the terminal
 * when started from a Shell in CTerm, else the screen */
enum { UI_AUTO, UI_SCREEN, UI_WINDOW, UI_TERM };
extern int cfgmode, uimode, fromwb;
extern int winbox[4];                   /* the window's place and size, kept */
int ui_choose(void);                    /* AUTO made definite: UI_SCREEN/WINDOW/TERM */
void ui_keepbox(void);
extern int uiresized;                   /* a new size came while a question was up */

/* what the person did: a key, a click on a cell, the wheel, a new size */
enum { EV_KEY = 1, EV_CLICK, EV_WHEEL, EV_RESIZE, EV_CLOSE };
typedef struct {
    int type, key;                      /* EV_KEY: the key; EV_WHEEL: -1 up, 1 down */
    int col, row;                       /* EV_CLICK: the cell */
    ULONG secs, mics;                   /* when (double clicks) */
    UWORD qual;
} UiEv;
ULONG ui_sig(void);                     /* the signal to Wait() for */
int ui_event(UiEv *e);                  /* one, if there is (does not wait) */
int ui_keywaiting(void);
void ui_flush(void);                    /* drawn before waiting */
void ui_clearall(void);
void ui_suspend(void);                  /* the terminal to a command for a while */
void ui_resume(int wait);

/* ---- term.c: inside a terminal */
extern int termcterm;
int term_possible(int *isctrm);
int term_open(void);
void term_close(void);
void term_put(int col, int row, const char *s, int len, int fg, int bg);
void term_clear(int row, int col, int w);
int term_pututf8(int col, int row, const char *s, int len, int fg, int bg);   /* cells it took */
int term_cw(unsigned long c);                     /* columns: 2 wide (tui 0.5), else 1 */
void term_clearall(void);
void term_flush(void);
ULONG term_sig(void);
int term_event(UiEv *e);
int term_keywaiting(void);
void term_suspend(void);
void term_resume(int wait);
void term_sixel(const unsigned char *rgb, int tw, int th, int col, int row);

int ui_open(void);
void ui_close(void);
int ui_otherwindows(void);
void ui_layout(void);
void put(int col, int row, const char *s, int len, int fg, int bg);
void putrole(int col, int row, const char *s, int len, int role);
int u8_is(const unsigned char *buf, long len);    /* UTF-8 text with letters beyond ASCII */
long u8_row(const unsigned char *buf, long len, long off, int w, int wrap,
            char *u8, int *u8len, unsigned char *lat);
void put8(int col, int row, const char *u8, int u8len, const unsigned char *lat, int w, int fg, int bg);
void clearrow(int row, int col, int w);
void ui_attrrow(int col, int row, const unsigned char *cells,
                const unsigned char *attrs, int w);
void say(const char *s, int err);
void showfault(const char *what);
int ask(const char *prompt);
int waitkey(int *raw);
int injected(void);
extern const char *inject;
int editline(const char *prompt, char *buf, int max, int cpos);
void drawhints(const char *const *items, int n, int colw);
void pager(const char *title, unsigned char *buf, long len, int ty);
int ui_abort(void);
void ui_gridon(void);
void ui_dumpgrid(const char *path);

/* synthetic keys for the raw ones */
enum { KEY_UP = 300, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PGUP, KEY_PGDN,
       KEY_TOP, KEY_END, KEY_HELP, KEY_BREAK, KEY_F1 };      /* KEY_F1 .. KEY_F1 + 9 */
#define KEY_F5 (KEY_F1 + 4)
/* (KEY_PGUP .. KEY_END are Shift with the arrows) */

/* ---- keys.c: every key is an action bound in PROGDIR:Ahiru.keys.
 * The order is keys.c's acts[] table */
#define CTX_MAIN 1
#define CTX_VIEW 2
enum { ACT_NONE, A_DOWN, A_UP, A_HALFDOWN, A_HALFUP, A_PAGEDOWN, A_PAGEUP, A_TOP, A_BOTTOM,
       A_PARENT, A_ENTER, A_VIEW, A_EDIT, A_UNPACK, A_MARK, A_INVERT, A_UNMARKALL, A_YANK, A_CUT,
       A_PASTE, A_PASTEOVER, A_UNYANK, A_DELETE, A_RENAME, A_RENAMEEXT, A_RENAMEEND, A_RENAMESTART,
       A_MKDIR, A_SEARCH, A_NEXT, A_PREV, A_FILTER, A_CLEARFILTER, A_COMMAND, A_SHELL, A_NEWSHELL,
       A_RELOAD, A_HELP, A_QUIT, A_TOGGLEINFO, A_TOGGLEPREVIEW, A_COLOURS, A_SORT, A_GO, A_CD,
       A_BOOKMARK, A_JUMP, A_TABNEW, A_TABCLOSE, A_TABNEXT, A_TABPREV, A_TAB, A_VISUAL,
       A_BULKRENAME, A_FIND, A_GREP, A_TRASH, A_PLAY, A_COPYPATH, A_COPYNAME, A_COPYDIR,
       A_OPENWITH, A_HABIT, A_LINEMODE, A_JOBS, A_PACK, A_COMPARE, A_SYNC, A_PATRENAME, A_CHECKSUMS, A_MOUNT, A_RUN, A_LAUNCH,
       V_CLOSE, V_DOWN, V_UP, V_PAGEDOWN, V_PAGEUP, V_HALFDOWN, V_HALFUP, V_TOP, V_BOTTOM, V_HEX,
       NACTIONS };
void keys_load(char *bad);              /* bad: a line the file has wrong ("" none) */
void keys_free(void);
int keys_isprefix(int ctx, int k);
int keys_find(int ctx, int k1, int k2, const char **arg);
int keys_hints(int ctx, int k, const char **items, int max, char (*lab)[12], char (*desc)[48]);
char *keys_help(void);                  /* xalloc'ed (xfree it), made from the bindings */
void keyname(int k, char *dst);         /* "j", "<Up>", "<C-d>" */
void keys_first(int ctx, int act, char *dst);   /* its first key, as a name */

/* ---- preview.c */
extern int pvdirty;
extern DList *prv;
void pv_invalidate(void);
void pv_adopt(const char *path);
void pv_compute(void);
void pv_draw(void);
void pv_free(void);

/* ---- thumb.c: a picture made small (NULL: none; *stop: a key came) */
unsigned char *thumb_make(const char *path, int maxw, int maxh, int *tw, int *th, int *sw, int *sh, int *stop);
void thumb_show(const unsigned char *rgb, int tw, int th, int x, int y);
void thumb_drop(void);                  /* the picture is gone: forget its dither */
void thumb_exit(void);
int keywaiting(void);                  /* a key (or a played one) is waiting */
extern int pvpeek, peekstop;           /* fe.c: the preview unpacks; a key stopped it */
void cellxy(int col, int row, int *x, int *y);   /* ui.c: a cell's top left in pixels */
int pv_init(void);

/* ---- ops.c */
extern int yankn, yankcut;
void op_yank(int cut);
void op_unyank(void);
void op_paste(int overwrite);
void op_delete(void);
void op_rename(int mode);
void op_mkdir(const char *name);
extern int renamemode;           /* fe.c's lineinput: where the cursor starts */
void op_touch(const char *name);
void op_shell(const char *cmd);
void op_newshell(void);
/* these work on the entry under the cursor (inside an archive too) */
void op_edit(void);
void op_openfile(void);
void op_extract(void);
void op_view(int hex);
int op_editpath(const char *path);      /* the editor on a file, waited for */

/* ---- more.c: tabs, visual mode, bulk rename, find, trashcan, sound */
void tab_new(void);
void tab_close(void);
void tab_next(int d);
void tab_go(const char *arg);
void tab_label(char *dst);
void tab_compare(int sync);
const char *tab_other(void);           /* the next tab's folder, NULL with one tab */
void visual_toggle(void);
void visual_moved(void);
void visual_end(void);
int visual_on(void);
void bulk_rename(void);
void find_run(int text);
int results_on(void);
void results_end(void);
void results_label(char *dst);
void trash_run(void);
void play_toggle(void);
void play_label(char *dst);
void more_exit(void);
/* more2.c: clipboard, open-with, jumping by habit, the live filter */
void clip_copy(int what);
void open_with(void);
void openwith_add(const char *line);
void openwith_save(BPTR fh);
void habit_load(void);
void habit_visit(const char *path);
void habit_jump(void);
void live_filter(void);
void more2_exit(void);
void theme_load(void);                  /* ui.c: PROGDIR:Ahiru.theme */
/* filecol.c: ENVARC:FileColours, the family's file kinds and colours (as csh's ls) */
void filecol_load(void);
const int *filecol_apply(const int *theme, const int *ansi, int plain, int ncolours);
int filecol_extrole(const char *name);
int cellat(int px, int py, int *col, int *row);   /* ui.c: the cell under the mouse */
/* rexx.c: the ARexx port */
int rexx_open(void);
ULONG rexx_sig(void);
int rexx_handle(void);                  /* 1 = QUIT */
int rexx_busy(int hidden);              /* hidden or in a prompt: GET only; 1 = QUIT */
void rexx_close(void);
const char *rexx_name(void);
int keys_action(const char *name);
int do_action(int act, const char *arg);   /* main.c: 1 quit */
/* jobs.c: a paste in the background */
int job_mine(void);
int job_busy(void);
ULONG job_sig(void);
void job_say(const char *s);
int job_ask(const char *q);
int job_line(const char *prompt, char *buf, int max);
void job_progshow(ULONG total);
void job_progadd(ULONG n);
int job_cancelled(void);
void job_changed(void);
int job_start(struct place *src, struct place *dst, int ismove, int force, int n);
int job_poll(void);                     /* 1: a question or the end - the screen changed */
void job_label(char *dst);
void job_show(void);
void job_end(void);
extern DList *prv;
int editlive(const char *prompt, char *buf, int max, void (*fn)(const char *buf));
void godir(const char *path, const char *selname);
extern int linemode;
/* main.c, for more.c */
void loaddir(const char *path, const char *selname);

/* ---- main.c */
extern DList *cur, *par;
extern int parnone;
extern int hideinfo, showpv, sortmode, sortrev;
extern char filter[64];
extern char cfgeditor[128];
void selpath(char *dst);
void dbg(const char *what);
void reloadall(const char *selname);
void drawall(void);
void drawcols(void);
void drawstatus(void);
void drawtitle(void);
void entrytext(DList *l, long i, int w, int sizes, int *role);
int roleof(long fl, const char *name);

#endif
