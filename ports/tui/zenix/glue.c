/* SPDX-License-Identifier: GPL-2.0-only */
/* The text-UI library's old tui_* names (upstream include/tui.h, for Sumi, Maki and Markdown) on Zenix: upstream's
 * glue (ports/tui/upstream/src/glue.c, MIT) as its TUI_STATIC build, the toolkit linked in (libtui.a), compiled
 * unchanged but for tui_possible: upstream asks AmigaOS whether Input() is a console; here the toolkit's own
 * TuiPossible answers (descriptor 0, or the handle tui_console gave, and descriptor 1 both terminals). Renamed and declared static
 * first, as ports/tui/zenix/core.c does with the terminal half. */
#ifndef TUI_STATIC
#define TUI_STATIC
#endif
#include <amiga.h>
#define tui_possible up_tui_possible
static int tui_possible(void);
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wempty-body"     /* upstream's droplib() is empty in its TUI_STATIC build */
#include "../upstream/src/glue.c"
#undef tui_possible

int tui_possible(void)
{
    return TuiPossible(conin);
}
