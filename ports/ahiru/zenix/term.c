/* SPDX-License-Identifier: GPL-2.0-only */
/* Ahiru inside the terminal on Zenix (ports/ahiru/README): upstream's ahiru/src/term.c compiled unchanged, but for
 * one answer. Upstream shows Ahiru on a screen of its own, in a Workbench window, or inside the terminal it was
 * started from, and by itself (MODE AUTO) inside the terminal only when that is CTerm's (its console says 'CTRM' to
 * ACTION_DISK_INFO). Zenix's console has no screens or windows to give (ports/common/gui.c), and the text-UI library
 * takes it for CTerm's already (ports/tui: the ANSI colours as they are), so term_possible says "CTerm" for every
 * terminal: AUTO then opens inside it. SCREEN or WINDOW asked for (Ahiru.config's MODE, the command line) fail, with
 * Ahiru's own message. Without a terminal (input or output not a console) there is none to open. Upstream's function
 * is renamed by the define (term_open calls it so) and Zenix's wraps it. */
#define term_possible up_term_possible
static int up_term_possible(int *isctrm);
#include "../upstream/ahiru/src/term.c"
#undef term_possible

int term_possible(int *isctrm)
{
    int can = up_term_possible(isctrm);
    *isctrm = can;
    return can;
}
