/* SPDX-License-Identifier: GPL-2.0-only */
/* The screen file (DESIGN D62, docs/d62-screen-modes.md 6): the system's /etc/screen (the video standard, the console,
 * the desktop's default, the card's size) and a user's ~/.config/screen/screen (the desktop's lines); before D62 the
 * desktop's /etc/zdesk/screen and ~/.config/zdesk/screen (docs/d38-desktop.md "Screen modes", "Fonts",
 * "Preferences"), read when the new file has no desktop line. Read by displayd (the system's, at its start) and
 * screenprefs (and written by it); zdesk reads the user's with its own words (display/zdesk/conf.c). One reader
 * (display/lib/screenfile.c). */
#pragma once

#define SCREEN_SYS "/etc/screen"
#define SCREEN_OLD_SYS "/etc/zdesk/screen"
#define SCREEN_USER ".config/screen/screen"     /* under $HOME */
#define SCREEN_OLD_USER ".config/zdesk/screen"

#define SF_DESKTOP 1                /* found: a desktop line (desktop, desktop-depth, desktop-font; mode, font) */
#define SF_CONSOLE 2                /* a console line (console, console-colours, console-font) */
#define SF_VIDEO 4                  /* video */
#define SF_RTG 8                    /* rtg */
#define SF_PI 16                    /* pi (D63) */
struct dpyscreen {
    int mode;                   /* DPYM_HIRES, DPYM_LACE, DPYM_SHRES, DPYM_SHRES_LACE ("auto" and no line: hires) */
    char font[32];              /* "font NAME SIZE": NAME; "" the 8 x 8 ("font builtin", "font topaz 8", no line) */
    int size;                   /* SIZE (0 with the 8 x 8) */
    int depth;                  /* desktop-depth: 3 or 4 (0: auto, the machine's) */
    int found;                  /* SF_* */
    /* the system's lines (D62), read but used only by screenprefs and init (which reads them itself) */
    int video;                  /* 0 auto, 1 PAL, 2 NTSC */
    int cmode;                  /* console: 0 hires, 1 hires-lace (auto, no line: hires) */
    int colours;                /* console-colours: 0 auto, 2, 4, 8, 16 */
    char cfont[24];             /* console-font: auto, none or NAME ("auto": no line) */
    int rw, rh, rbits;          /* rtg WxH[xBITS]: 0 0 0 "rtg front" or no line (the front screen as it is) */
    int pw, ph, pbits;          /* (D63) pi WxHxBITS: the Pi's picture (BITS 8, 16, 32); 0 0 0 "pi off" or no line */
};
/* path read into s -> 0; -ENOENT (no file: s the defaults). A line not understood is skipped; the first is said on
 * standard error ("WHO: PATH line N: not understood (...): LINE", with how many more there were): one line however
 * broken the file is, and the rest of it still counts */
int dpy_screenfile(const char *path, struct dpyscreen *s, const char *who);
