/* SPDX-License-Identifier: GPL-2.0-only */
/* The PC's stand-in for <zenix.h>'s screen layer (user/zscr.c), with which
 * tools/games-host-test.sh builds the games' curses (ports/bsdgames/zenix/curses.c) on the PC */
#pragma once
#include <stdio.h>
static int zscr_rows = 24, zscr_cols = 80;
static inline int zscr_size(void) { return 0; }
static inline int zscr_move(char *b, int row, int col) { return sprintf(b, "\033[%d;%dH", row, col); }
