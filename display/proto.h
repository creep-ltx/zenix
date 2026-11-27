/* SPDX-License-Identifier: GPL-2.0-only */
/* The display protocol, version 1 (DESIGN D38 section 4, docs/d38-desktop.md): between displayd (the display server)
 * and its clients (libdisplay, display/display.h) and the window manager (zdesk). Frozen once released, as
 * kernel/fsproto.h: message types, structs and drawing orders only grow at the end, never change meaning
 * (docs/frozen-interfaces.md). Numbers claimed in docs/claims.md.
 *
 * One port, "display", created by displayd. A client calls it (SYS_call: synchronous, answered at once; the server
 * never waits for a client) with a struct umsg whose type is a DPY_* and whose a[0..3] are the arguments below; the
 * answer is the message's result (negative: -errno). Pointers in a request are the client's own memory: displayd
 * checks each one with SYS_peer before it reads or writes there.
 *
 * Events go the other way into a ring in the client's own memory, registered at DPY_HELLO together with a port of
 * the client's: displayd writes records into it with SYS_post (the kernel checks the ring is still the client's at
 * every write) and that wakes the client's poll (POLLPORT | POLLRDBAND on its port). A slow client never blocks the
 * server: a full ring sets DPYR_LOST, and the client asks for its windows' state again (DPY_WIN_INFO). */
#pragma once

#define DPY_PORTNAME    "display"
#define DPY_VERSION     1

/* ---- requests (umsg.type) ---- */
#define DPY_HELLO       1   /* a[0] struct dpyhello * -> the server's version, or -errno (a second hello -EBUSY) */
#define DPY_WIN_NEW     2   /* a[0] struct dpywin * -> the new window's id (1..), its place and size filled back */
#define DPY_WIN_SET     3   /* a[0] id, a[1] what (WS_*), a[2], a[3] */
#define DPY_WIN_CLOSE   4   /* a[0] id: the window goes */
#define DPY_DRAW        5   /* a[0] id, a[1] the orders (the client's memory), a[2] their bytes -> 0, or -EINVAL with
                             * the bad order's byte offset in a[3] */
#define DPY_FONT        6   /* a[0] the name ("topaz"), a[1] the size in pixels -> id << 16 | width << 8 | height: the
                             * 8 x 8 (id 0) for any name; a[0] 0: the desktop's font (DPYF_UI, its nominal width) */
#define DPY_IMAGE       7   /* a[0] struct dpyimage * -> the image's id (1..); the server keeps a copy */
#define DPY_PALETTE     8   /* a[0] u16 * (12 bits 0x0RGB), a[1] the room in entries -> the screen's depth */
#define DPY_BYE         9   /* the client leaves: its windows go */
#define DPY_WIN_INFO    10  /* a[0] id, a[1] struct dpyinfo * -> 0: the window's place, size, state, title */
#define DPY_IMAGE_FREE  11  /* a[0] id */
#define DPY_MENU        12  /* a[0] id, a[1] the window's menus (the client's memory), a[2] their bytes (at most
                             * DPY_MENUMAX; 0: none): "Title\tItem\tItem\nTitle\tItem..." - the bar draws them (D38
                             * revision 2), a pick comes back as EV_MENU */
#define DPY_SESSION     13  /* root only: a[0] the uid whose programs may use the desktop from now on (dlogin's, D38
                             * revision 3: the greeter's root, then the user's session, then root again) */
#define DPY_STAT        14  /* a[0] u32[6] (the client's memory): blits started, pixels drawn (a rectangle once for all
                             * its planes), glyphs drawn, DPY_DRAW batches, damage painted (a change of the windows),
                             * copies onto the screen; a[1] 1: then counted from 0 again. The tests' measure of what
                             * an action costs. a[1] 2: a[0] is u32[12], six more: a whole window's drag moves, their
                             * E-clock ticks, shows of the drawn screen onto the one on view (a whole drag draws off
                             * the screen), their ticks, the ticks waited for the beam, shows that missed its rule;
                             * a[1] 4 (D61): u32[15], three more: a card's pointer shows, their 100 Hz ticks from the
                             * kernel's record to the card, the most */
#define DPY_MODE        15  /* a[0] the screen's mode (DPYM_*; DPYM_ASK: none changed) -> the mode now, or -EBUSY (a
                             * drag under way, the console not shown), -ENOMEM (no chip RAM for it: unchanged). Every
                             * client gets EV_SCREEN; windows are kept on the screen and drawn again (D38 "Screen
                             * modes": a preferences program's Test, Use and Cancel) */
#define DPYM_HIRES      0   /* 640 x 256 (PAL), 640 x 200 (NTSC) */
#define DPYM_LACE       1   /* hires interlaced: 640 x 512, 640 x 400 */
#define DPYM_RTG        2   /* (D43) a graphics card's screen, the size AmigaOS gave it (zenixboot RTG): only answered;
                             * asked for hires or interlace there, the answer is still DPYM_RTG and nothing changes */
#define DPYM_ASK        0xFF
/* D62 (docs/d62-screen-modes.md 5): super-hires, AGA only (-EINVAL elsewhere), 3 or 4 planes at the 64-bit fetch;
 * DPY_MODE's a[1]: the depth (0 as it is; 3, or 4 on AGA; -EINVAL another), and with a[1] not 0 the answer is the
 * mode | the depth << 8; a[1] DPYMD_ASK only asks for both */
#define DPYM_SHRES      3   /* 1280 x 256 (PAL), 1280 x 200 (NTSC) */
#define DPYM_SHRES_LACE 4   /* 1280 x 512, 1280 x 400 */
#define DPYMD_ASK       0xFF
#define DPY_FONT_INFO   16  /* a[0] a font (0 the 8 x 8, DPYF_UI the desktop's), a[1] struct dpyfont * (the client's
                             * memory) -> 0, its metrics and widths written there; -EINVAL another number. The
                             * desktop's font is /etc/zdesk/screen's "font NAME SIZE" (an AmigaOS bitmap font in
                             * /usr/share/fonts/amiga/), or the 8 x 8 when none is set or it could not be read
                             * (docs/d38-desktop.md "Fonts") */
#define DPY_FONT_SET    17  /* a[0] 32 bytes (the client's memory): the font's name, ended by a 0 (0: the 8 x 8), a[1] its
                             * size -> 0: the desktop's font from now on, every client told (EV_SCREEN with DPYS_FONT)
                             * and every window drawn again; -ENOENT (no such font or size), -EINVAL (a file that cannot
                             * be read), -ENOMEM: the font in use stays; -EBUSY (a drag). Any client of the desktop's
                             * session, as DPY_MODE (docs/d38-desktop.md "Preferences": Test, Use, Cancel) */
#define DPY_FONTNAME    32
/* DESIGN D55 (docs/d55-screens.md): a screen of the program's own, its mode and palette, its planes in its own chip
 * RAM, a Copper list displayd checks and translates. At most one a client; its events carry win 0. Each answer that
 * changes the picture is the vertical-blank count from which it shows (CON_VBLANK, libdisplay's dpy_vwait) */
#define DPY_SCREEN      18  /* a[0] struct dpyscr * -> the count, or -EINVAL, -EFAULT, -EBUSY, -ENODEV, -ENOMEM, -ENOSPC */
#define DPY_SCR_SET     19  /* a[0] what (SS_*), a[1], a[2] -> the count (SS_FRONT: 0), -ENOENT no screen */
#define DPY_SCR_CLOSE   20  /* -> 0: the screen goes (the desktop in front if it was) */
#define SS_PALETTE      1   /* a[1] first | count << 16, a[2] the colours (0x00RRGGBB): in place */
#define SS_COPPER       2   /* a[1] struct dpycop[], a[2] their count (0 .. ncop): a flip; -EINVAL with the index in a[3] */
#define SS_PLANES       3   /* a[1] plane 0's offset in the planes' memory: a flip */
#define SS_FRONT        4   /* a[1] 1 in front, 0 behind (-EBUSY during a drag) */
#define SS_COLOURS      5   /* a[1] first instruction | count << 16, a[2] count colours: the next count CO_COLOURs from
                             * that instruction on (other instructions passed over) get them, in place; -EINVAL with
                             * a[3] the colours set when the list has fewer */
#define DPYSCR_COPMAX   1024
/* DESIGN D61 (docs/d61-mouse.md 5): the mouse's setting. Any client of the desktop's session, as DPY_MODE */
#define DPY_MOUSE       21  /* a[0] struct dpymouse * (the client's memory), a[1] DPYMO_SET or DPYMO_GET -> 0, -EINVAL
                             * (a value out of range: nothing changes), -EFAULT */
#define DPYMO_SET       0
#define DPYMO_GET       1   /* the setting in effect, and the pointer's place in x, y */
struct dpymouse {
    unsigned short size;     /* DPYMOUSE_FIRST or more */
    unsigned short speed;    /* percent: 100 moves one pixel of the screen a count at slow speed (10..800) */
    unsigned short mult;     /* percent (100..800): in a frame (a fiftieth of a second) the counts beyond thresh count
                              * this much (AmigaOS's acceleration; 100 is none) */
    unsigned char thresh;    /* 0..127 */
    unsigned char flags;     /* DPYMF_* */
    unsigned short dclick;   /* the double-click time, 100 Hz ticks (10..300) */
    signed char hotx, hoty;  /* the pointer's hot spot in its picture (0..15) */
    unsigned short colour[3];   /* the pointer's colours 1-3, 0x0RGB */
    unsigned short image[32];   /* its picture: 16 rows of two words, sprite 0's own layout (plane 0, plane 1) */
    short x, y;              /* DPYMO_GET: the pointer's place on the screen */
};
#define DPYMOUSE_FIRST  86
#define DPYMF_SWAP      1   /* the left and right buttons swapped */
/* the window manager's (only after DPY_WM_HELLO; one window manager at a time) */
#define DPY_WM_HELLO    32  /* become the window manager -> 0, -EBUSY (another one), -EPERM (not the session's user) */
#define DPY_WM_STYLE    33  /* a[0] struct dpystyle *: frames from now on (and the screen's palette) */
#define DPY_WM_PLACE    34  /* a[0] id, a[1] x << 16 | y (the frame's top left), a[2] w << 16 | h (the inside) */
#define DPY_WM_STACK    35  /* a[0] id, a[1] 0 to the front, 1 to the back */
#define DPY_WM_FOCUS    36  /* a[0] id (0: nobody): the keyboard's window */
#define DPY_WM_SHOW     37  /* a[0] id: shown (after DPY_WM_HIDE, or a new window's first showing) */
#define DPY_WM_HIDE     38  /* a[0] id, a[1] why (HIDE_MINIMISE, HIDE_WORKSPACE): not shown; a workspace's window
                             * gives up its kept copy */
#define DPY_WM_GRAB     39  /* a[0] struct dpykey *, a[1] how many (at most DPY_MAXGRAB): the keys the manager gets */
#define DPY_WM_DRAG     40  /* a[0] id, a[1] DRAG_* : the server follows the mouse until the button is released */
#define DPY_WM_CLOSE    41  /* a[0] id: ask the window's program to close it (EV_CLOSE) */
#define DPY_WM_KILL     42  /* a[0] id: the window goes now (a program that does not answer EV_CLOSE) */
#define DPY_WM_MENUGET  43  /* a[0] id, a[1] a buffer, a[2] its size -> the menus' bytes copied (DPY_MENU's text) */
#define DPY_WM_MENUPICK 44  /* a[0] id, a[1] the menu (0..), a[2] the item (0..): EV_MENU to the window's program */
#define DPY_WM_PGRAB    45  /* a[0] one of the manager's windows: every pointer event goes to it until the buttons are
                             * all up (a menu held with the right button); -EINVAL if none is held */
#define DPY_WM_BYE      46  /* the window manager leaves the role (dlogin before the user's session starts its own) */
#define DPY_WM_MOUSE    47  /* a[0] the mouse's acceleration: THRESHOLD | MULTIPLIER << 8 (the kernel's CON_MOUSE: in a
                             * frame, the counts beyond THRESHOLD count MULTIPLIER times; 0x100 none) */

/* DPY_WIN_SET's what */
#define WS_TITLE        1   /* a[2] the title (the client's memory, at most DPY_TITLE - 1 bytes) */
#define WS_SIZE         2   /* a[2] w << 16 | h: the inside's wanted size (with a manager: EV_WM_SIZE, it decides) */

/* DPY_WM_HIDE's why */
#define HIDE_MINIMISE   1
#define HIDE_WORKSPACE  2

/* DPY_WM_DRAG's modes */
#define DRAG_MOVE       1
#define DRAG_RESIZE     2
#define DRAG_OUTLINE    0x10 /* an outline (XOR) while dragging, the window moved once at the release */

#define DPY_TITLE       40
#define DPY_NAME        16
#define DPY_MAXGRAB     64
#define DPY_MENUMAX     512

struct dpyhello {
    unsigned size;           /* sizeof (the first size: DPYHELLO_FIRST) */
    unsigned version;        /* DPY_VERSION */
    char name[DPY_NAME];     /* the program's name */
    unsigned ring;           /* the event ring: struct dpyring then records, in the client's memory */
    unsigned ringsize;       /* its bytes (at least 16 + 8 records) */
    int port;                /* the client's port: displayd posts into the ring through it (SYS_post POST_BUF) */
    unsigned short width, height, depth, id;    /* back: the screen and the client's number */
};
#define DPYHELLO_FIRST  44

/* the ring's head (the client's memory): displayd writes records and then head; the client reads and moves tail */
struct dpyring {
    unsigned head;           /* displayd: the next record it writes (a count, modulo nrec) */
    unsigned tail;           /* the client: the next record it reads */
    unsigned flags;          /* displayd: DPYR_* */
    unsigned nrec;           /* displayd at hello: the records the ring holds */
};
#define DPYR_LOST       1    /* records were dropped (a full ring): ask the windows' state again; the client clears it */

/* a window's request (DPY_WIN_NEW) */
struct dpywin {
    unsigned size;           /* sizeof (DPYWIN_FIRST) */
    unsigned short kind;     /* WK_* */
    unsigned short flags;    /* WF_* */
    short x, y;              /* the wanted place of the frame's top left (-1: the manager chooses); back: the place */
    unsigned short w, h;     /* the inside's wanted size in pixels; back: the size given */
    unsigned short minw, minh, maxw, maxh;      /* 0: no limit */
    unsigned short stepw, steph;                /* resizes in steps of this (a terminal's cell), 0: 1 */
    char title[DPY_TITLE];
};
#define DPYWIN_FIRST    68
#define WK_NORMAL       0
#define WK_DIALOG       1
#define WK_BAR          2    /* the manager's bar: no frame, always in front */
#define WK_DESKTOP      3    /* the manager's desktop: no frame, always at the back, the whole screen */
#define WF_MOTION       1    /* motion events also without a button held */

struct dpyinfo {             /* DPY_WIN_INFO */
    unsigned size;
    short x, y;              /* the frame's top left */
    unsigned short w, h;     /* the inside */
    short ix, iy;            /* the inside's top left on the screen */
    unsigned short state;    /* WST_* */
    unsigned short kind;
    int pid;                 /* the program's */
    char title[DPY_TITLE];
};
#define DPYINFO_FIRST   64
#define WST_SHOWN       1
#define WST_FOCUS       2
#define WST_MINIMISED   4
#define WST_WORKSPACE   8    /* hidden for a workspace */

struct dpyimage {            /* DPY_IMAGE: planar, rows of ((w + 15) / 16) words, plane after plane */
    unsigned size;
    unsigned short w, h, depth, flags;   /* depth 1..4; IMG_MASK: one more plane, the shape (1: drawn) */
    unsigned data;           /* the client's memory */
};
#define DPYIMAGE_FIRST  16
#define IMG_MASK        1

struct dpykey {              /* DPY_WM_GRAB: a raw key code with exactly these qualifiers (in mask) */
    unsigned char code, pad;
    unsigned short mask, qual;   /* GQ_* (usys.h): which qualifiers count, and their wanted state */
    unsigned short id;           /* the manager's own number, back in EV_WM_KEY */
};

/* the frame style (DPY_WM_STYLE) */
struct dpystyle {
    unsigned size;               /* sizeof (DPYSTYLE_FIRST) */
    unsigned short palette[16];  /* the screen's colours, 12 bits; at 8 colours the first 8 */
    unsigned char left, right, top, bottom;     /* border widths around the inside; top includes the title bar */
    unsigned char title;         /* the title bar's height within top (0: none) */
    unsigned char bevel;         /* 1: a light and a dark line on the border's edges (raised) */
    unsigned char shadow;        /* a drop shadow this many pixels right and down (0: none) */
    unsigned char centre;        /* the title centred (1) or left */
    unsigned char border[2], bar[2], text[2];   /* the colours [0] inactive [1] active */
    unsigned char light, dark;   /* the bevel's colours */
    unsigned char shadowc;       /* the shadow's colour */
    unsigned char stripes;       /* the active title bar striped every second line in this colour (0: none) */
    unsigned short gad[3];       /* the gadgets' images (DPY_IMAGE ids, 0 none): close, minimise, zoom */
    unsigned char gadx[3];       /* their place: a pixel from the left of the frame, or 128 + a pixel from the right */
    unsigned char drag;          /* the default drag: DRAG_OUTLINE or 0 (the whole window) */
};
#define DPYSTYLE_FIRST  64

/* a screen of its own (DPY_SCREEN, DESIGN D55 section 3) */
struct dpyscr {
    unsigned size;               /* sizeof (DPYSCR_FIRST) */
    unsigned short w, h;         /* 320 (lores) or 640 (hires); 1 .. the display's lines (twice with DPYSF_LACE, even) */
    unsigned short depth;        /* planes 1-8 (OCS/ECS: lores 1-6, hires 1-4) */
    unsigned short flags;        /* DPYSF_* */
    unsigned planes;             /* the planes' memory: the base of a chip RAM region the client's memalloc gave */
    unsigned bytes;              /* the part every fetch stays in: [planes, planes + bytes) */
    unsigned pstride;            /* plane k + 1 is this many bytes after plane k */
    unsigned short bpr;          /* a plane's line to its next line */
    unsigned short ncop;         /* the most instructions SS_COPPER gives (0 .. DPYSCR_COPMAX) */
    unsigned short ncol;         /* colours in palette (0 .. 256; 32 on OCS/ECS) */
    unsigned short pad;
    unsigned palette;            /* ncol colours, 0x00RRGGBB */
};
#define DPYSCR_FIRST    36
#define DPYSF_LACE      1        /* interlaced */
#define DPYSF_HAM       2        /* HAM6 (6 planes) or HAM8 (8, AGA) */
#define DPYSF_EHB       4        /* Extra Half-Brite (6 planes) */
#define DPYSF_SCROLL    8        /* one more fetch unit on the left of each line (2 bytes OCS/ECS, 8 AGA) */
#define DPYSF_BACK      16       /* opened behind the desktop */
#define DPYSF_MOTION    32       /* EV_MOTION also without a button held */
struct dpycop { unsigned short op, a, b, c; };  /* SS_COPPER's instructions, in screen order */
#define CO_WAIT         1        /* a the line (interlaced: frame line a / 2), b the lores (hires) pixel across, 0 the start */
#define CO_COLOUR       2        /* a the colour register, b << 16 | c 0x00RRGGBB */
#define CO_PLANE        3        /* a the plane, b << 16 | c its offset from SS_PLANES' offset; only right after a CO_WAIT
                                  * with b 0 (or before the first): from that line on */
#define CO_MOD          4        /* a, b the odd and even planes' modulos (signed, the hardware's, not interlaced); as CO_PLANE */
#define CO_SCROLL       5        /* a BPLCON1 */

/* ---- drawing orders (DPY_DRAW): 16-bit words; each order's first word is op << 8 | its length in words (header
 * included), so an order the server does not know is skipped. Coordinates are window-relative pixels, clipped to
 * the window, the clip rectangle and what is visible. Colours are pens (0..15). ---- */
#define O_CLIP          1   /* x, y, w, h (w 0: no clip) */
#define O_FILL          2   /* x, y, w, h, pen */
#define O_TEXT          3   /* x, y, font, fg | bg << 8, attr (TA_*), nbytes, then the UTF-8 bytes, padded to a word */
#define O_COPY          4   /* sx, sy, w, h, dx, dy: inside the window (scrolling) */
#define O_IMAGE         5   /* id, sx, sy, w, h, dx, dy */
#define O_XLINE         6   /* x, y, w, h [, pen]: invert the pen's planes (all without one) - a cursor, an outline */
/* O_TEXT's font word: 0 the 8 x 8 (Topaz from the ROM and the ZCF supplement: terminals' cells, 8 pixels a code
 * point), DPYF_UI the desktop's font (window titles, the bar, menus, labels: proportional, its own height; a code
 * point it lacks drawn by the 8 x 8 at its baseline); another number: 0. The text's box is x .. x + its width
 * (DPY_FONT_INFO's widths), y .. y + the font's height: drawn in fg on bg, nothing outside it */
#define DPYF_UI         1
#define TA_BOLD         1
#define TA_UNDER        2
#define TA_INV          4

/* a font's metrics (DPY_FONT_INFO), for laying text out before it is drawn */
struct dpyfont {
    unsigned size;               /* sizeof (DPYFONT_FIRST) */
    unsigned short id;           /* O_TEXT's font word for it */
    unsigned short height;       /* lines */
    unsigned short baseline;     /* the baseline's line from the top (0 .. height - 1) */
    unsigned short xsize;        /* the nominal width (every character's in a fixed-width font) */
    unsigned short flags;        /* DPYFF_* */
    unsigned short other;        /* the width of a code point past 255 (drawn by the 8 x 8): 8 */
    char name[32];               /* "topaz 11" (the setting's name and size), "" the 8 x 8 */
    unsigned char adv[256];      /* the width of each code point 0-255 (the pen's advance) in pixels */
};
#define DPYFONT_FIRST   304
#define DPYFF_PROP      1        /* proportional */
#define DPYFF_BUILTIN   2        /* the 8 x 8 (no font set, or it could not be read) */

/* ---- events (the ring's records, 16 bytes) ---- */
struct dpyev {
    unsigned char type;      /* EV_* */
    unsigned char code;      /* EV_KEY: the raw key code (bit 7 up); EV_BUTTON: which (1 left, 2 right, 3 middle),
                              * bit 7 up; EV_WM_FRAME: the part (FP_*); EV_WM_KEY: the grab's id (low byte) */
    unsigned short qual;     /* GQ_* (usys.h): qualifiers and buttons */
    unsigned short win;      /* the window */
    unsigned short arg;      /* EV_FOCUS 1 in / 0 out; EV_MINIMISED 1 hidden / 0 shown; EV_WM_KEY the grab's id */
    short x, y;              /* the pointer, window-relative (EV_WM_*: the screen's); EV_EXPOSE, EV_RESIZE: a place */
    union {
        struct { unsigned short w, h; };    /* EV_EXPOSE's rectangle, EV_RESIZE's new size */
        unsigned char ch[4];                /* EV_KEY: the character, UTF-8 */
    };
};
#define EV_KEY          1
#define EV_BUTTON       2   /* (D61) arg 2: a double click: the same button again within the double-click time and 4
                             * pixels of the last press (a third press counts as a first) */
#define EV_MOTION       3
#define EV_RESIZE       4   /* the inside is w x h now (and EV_EXPOSE follows for what must be drawn) */
#define EV_EXPOSE       5   /* x, y, w, h: draw this again */
#define EV_FOCUS        6
#define EV_CLOSE        7   /* the close gadget was used: the program decides */
#define EV_MINIMISED    8
#define EV_MENU         9   /* a menu item was picked: code the menu, arg the item */
#define EV_SCREEN       10  /* the screen's mode changed (DPY_MODE): w x h its size now, arg the mode (DPYM_*); code
                             * DPYS_FONT: the desktop's font changed (DPY_FONT_SET; ask DPY_FONT_INFO again); x (D62)
                             * the screen's depth (0 from an older server) */
#define DPYS_FONT       1
/* to the window manager */
#define EV_WM_NEW       32  /* a window wants to appear: win, w, h its wanted inside, x, y its wanted place, arg kind */
#define EV_WM_FRAME     33  /* a click on a frame: code the part (FP_*), qual the buttons, x, y */
#define EV_WM_KEY       34  /* a grabbed key: arg the grab's id, code the raw code */
#define EV_WM_GONE      35  /* a window ended */
#define EV_WM_DRAGEND   36  /* the drag ended: x, y the place, w, h the size */
#define EV_WM_TITLE     37  /* a window's title changed */
#define EV_WM_CLICK     38  /* a click on a window's inside (focus follows the manager): win, qual, x, y */
#define EV_WM_SIZE      39  /* a program asks for a new size (WS_SIZE): win, w, h; the manager decides (DPY_WM_PLACE) */
#define EV_WM_MENU      40  /* a window's menus changed (DPY_MENU) */
#define FP_TITLE        1
#define FP_CLOSE        2
#define FP_MIN          3
#define FP_ZOOM         4
#define FP_BORDER       5
#define FP_SIZE         6   /* the bottom right corner: resize */
