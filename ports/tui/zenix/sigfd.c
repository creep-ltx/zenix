/* SPDX-License-Identifier: GPL-2.0-only */
/* TuiWait's extra signals on Zenix (ports/tui/zenix/core.c): the descriptor behind a signal bit. A program with the
 * owner's AmigaOS layer (ports/common/dos.c) has its own tui_zenix_sigfd, its reply ports; this one, a member of
 * the library of its own, is linked only when the program has none: no descriptors, the extra signals never come. */
#include <amiga.h>

int tui_zenix_sigfd(int bit)
{
    (void)bit;
    return -1;
}
