/* SPDX-License-Identifier: GPL-2.0-only */
/* The desktop's mouse settings (DESIGN D61, docs/d61-mouse.md 6): /etc/zdesk/mouse and a user's own
 * ~/.config/zdesk/mouse ("speed", "acceleration", "threshold", "multiplier", "buttons", "doubleclick", "pointer"), and
 * the pointer files NAME.pointer in ~/.config/zdesk/pointers/ and /etc/zdesk/pointers/ (a 16 x 16 picture in text).
 * One reader (display/lib/mousefile.c) for zdesk (applied when a session starts) and mouseprefs (which writes them). */
#pragma once
#include "proto.h"

#define MF_NAME 24                      /* a pointer's name, with its 0 */
/* acceleration's words: none, low, medium, high (index 0-3) as threshold and multiplier (percent) */
extern const unsigned char mf_acc_thresh[4];
extern const unsigned short mf_acc_mult[4];
extern const char *const mf_acc_name[4];
/* m's settings as the file says, from the defaults (speed 100, low, normal buttons, 0.5 s; the picture left as it is),
 * the pointer's name into pointer (MF_NAME bytes; "" none) -> 0; -ENOENT (no file: the defaults). A line not understood
 * is skipped and the first said on standard error ("WHO: PATH line N: not understood ..."), as the screen file's */
int dpy_mousefile(const char *path, struct dpymouse *m, char *pointer, const char *who);
/* the pointer called name (the user's pointers first, then the system's) into m's picture, colours and hot spot ->
 * 0, -ENOENT, -EINVAL (a broken file: m unchanged, said on standard error) */
int dpy_pointerfile(const char *name, struct dpymouse *m, const char *who);
/* the pointer's file text for m (at most 400 bytes) into buf -> its length */
int dpy_pointertext(const struct dpymouse *m, char *buf);
/* the path of a user's (HOME set, not root) or the system's file NAME under zdesk's settings ("pointers/x.pointer"),
 * its folders made; 0 when there is no HOME */
const char *dpy_mousepath(const char *name, char *buf, int user);
