/* SPDX-License-Identifier: GPL-2.0-only */
/* The terminfo calls tetris makes (ports/bsdgames/zenix/term.c): one terminal description, the console's
 * (TERM=xterm-16color), built in; no database is read. */
#ifndef ZGAMES_TERM_H
#define ZGAMES_TERM_H
extern const char *cursor_address, *cursor_up, *cursor_home, *cursor_to_ll, *clear_screen, *clr_eol,
                  *enter_standout_mode, *exit_standout_mode, *enter_ca_mode, *exit_ca_mode, *cursor_invisible,
                  *cursor_normal, *set_a_foreground;
extern int lines, columns, move_standout_mode;
extern short ospeed;
int setupterm(const char *term, int fd, int *errret);
char *tiparm(const char *cap, ...);
int tputs(const char *s, int affcnt, int (*out)(int));
#endif
