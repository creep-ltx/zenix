/* SPDX-License-Identifier: GPL-2.0-only */
/* The desktop's screen file (docs/d38-desktop.md "Screen modes", "Fonts", "Preferences"): /etc/zdesk/screen, and a
 * user's own ~/.config/zdesk/screen. Read by displayd (the system's, at its start), zdesk (the user's, applied at the
 * session's start) and deskprefs (and written by it); one reader (display/lib/screenfile.c) for the three. */
#pragma once

struct dpyscreen {
    int mode;                   /* DPYM_HIRES or DPYM_LACE ("mode auto" and no line: hires) */
    char font[32];              /* "font NAME SIZE": NAME; "" the 8 x 8 ("font builtin", "font topaz 8", no line) */
    int size;                   /* SIZE (0 with the 8 x 8) */
};
/* path read into s -> 0; -ENOENT (no file: s the defaults). A line not understood is skipped; the first is said on
 * standard error ("WHO: PATH line N: not understood (...): LINE", with how many more there were): one line however
 * broken the file is, and the rest of it still counts */
int dpy_screenfile(const char *path, struct dpyscreen *s, const char *who);
