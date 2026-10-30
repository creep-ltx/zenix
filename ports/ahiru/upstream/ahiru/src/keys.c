/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* keys - every key Ahiru knows is an action bound in PROGDIR:Ahiru.keys.
 *
 * The file is text, one line per key: the keys, the action, maybe an
 * argument ("gs go SYS:"). Two keys in a row make a prefix ("yy", "dD"):
 * the first shows a panel of what can follow, made from the bindings, so
 * it always tells the truth. The built-in keys are the same text, read
 * first; the file adds to them and changes them ("none" takes a key
 * away). With no file, Ahiru writes one with every built-in key in it -
 * a full list to edit. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <string.h>
#include "ahiru.h"
#include "fs/keymap.h"
#include "fs/os.h"

#define KEYFILE "PROGDIR:Ahiru.keys"


/* ---- the actions --------------------------------------------------- */

static const struct kmact acts[] = {
    { "none", "", 0 },
    { "down", "move down", CTX_MAIN }, { "up", "move up", CTX_MAIN },
    { "halfdown", "half a page down", CTX_MAIN }, { "halfup", "half a page up", CTX_MAIN },
    { "pagedown", "a page down", CTX_MAIN }, { "pageup", "a page up", CTX_MAIN },
    { "top", "to the top", CTX_MAIN }, { "bottom", "to the bottom", CTX_MAIN },
    { "parent", "the parent directory", CTX_MAIN },
    { "enter", "enter, or open the file by type", CTX_MAIN },
    { "view", "view the file (text, ANSI, hex)", CTX_MAIN },
    { "edit", "edit it", CTX_MAIN },
    { "unpack", "unpack archives here", CTX_MAIN },
    { "mark", "mark and move on", CTX_MAIN },
    { "invertmarks", "invert the marks", CTX_MAIN },
    { "unmarkall", "unmark all", CTX_MAIN },
    { "yank", "yank (copy)", CTX_MAIN }, { "cut", "cut", CTX_MAIN },
    { "paste", "paste here", CTX_MAIN }, { "pasteover", "paste, overwrite all", CTX_MAIN },
    { "unyank", "forget the yank list", CTX_MAIN },
    { "delete", "delete (asks)", CTX_MAIN },
    { "rename", "rename (empty field)", CTX_MAIN },
    { "renameext", "rename, at the extension", CTX_MAIN },
    { "renameend", "rename, at the end", CTX_MAIN },
    { "renamestart", "rename, at the start", CTX_MAIN },
    { "mkdir", "make a directory", CTX_MAIN },
    { "search", "search", CTX_MAIN }, { "next", "next match", CTX_MAIN },
    { "prev", "previous match", CTX_MAIN },
    { "filter", "filter the listing", CTX_MAIN }, { "clearfilter", "clear the filter", CTX_MAIN },
    { "command", "command line (mkdir, touch, cd, shell)", CTX_MAIN },
    { "shell", "run a shell command here", CTX_MAIN }, { "newshell", "open a shell here", CTX_MAIN },
    { "reload", "read again", CTX_MAIN }, { "help", "these keys", CTX_MAIN },
    { "quit", "quit", CTX_MAIN },
    { "toggleinfo", "show/hide .info icons", CTX_MAIN },
    { "togglepreview", "preview column on/off", CTX_MAIN },
    { "colours", "cycle 4 / 8 / 16 colours", CTX_MAIN },
    { "sort", "sort by", CTX_MAIN },
    { "go", "go to", CTX_MAIN }, { "cd", "go to a typed path", CTX_MAIN },
    { "bookmark", "bookmark here (then a key)", CTX_MAIN },
    { "jump", "jump to a bookmark (then a key)", CTX_MAIN },
    { "tabnew", "a new tab, here", CTX_MAIN }, { "tabclose", "close this tab", CTX_MAIN },
    { "tabnext", "the next tab", CTX_MAIN }, { "tabprev", "the tab before", CTX_MAIN },
    { "tab", "tab", CTX_MAIN },
    { "visual", "visual mode: marks follow the cursor", CTX_MAIN },
    { "bulkrename", "rename the marked (or all) in the editor", CTX_MAIN },
    { "find", "find names, in every folder below", CTX_MAIN },
    { "grep", "find text in the files below", CTX_MAIN },
    { "trash", "move to the trashcan", CTX_MAIN },
    { "play", "play the module or sound (again: stop)", CTX_MAIN },
    { "copypath", "copy the path to the clipboard", CTX_MAIN },
    { "copyname", "copy the name to the clipboard", CTX_MAIN },
    { "copydir", "copy the folder to the clipboard", CTX_MAIN },
    { "openwith", "open with ... (a menu)", CTX_MAIN },
    { "habit", "jump to a folder used often", CTX_MAIN },
    { "linemode", "after the name: size, date, bits, nothing", CTX_MAIN },
    { "jobs", "the copy running in the background", CTX_MAIN },
    { "pack", "pack the marked (or this) into an archive here", CTX_MAIN },
    { "compare", "compare with the next tab's folder (marks)", CTX_MAIN },
    { "sync", "make the next tab's folder like this one", CTX_MAIN },
    { "patrename", "rename by pattern (*.JPG to *.jpg), with a preview", CTX_MAIN },
    { "checksums", "MD5 of the marked files; on a .md5 or .sfv: check them", CTX_MAIN },
    { "mount", "an .adf into a drive (DA0:) and in; again: out", CTX_MAIN },
    { "run", "run:", CTX_MAIN },
    { "launch", "start:", CTX_MAIN },
    { "close", "close the viewer", CTX_VIEW },
    { "down", "line down", CTX_VIEW }, { "up", "line up", CTX_VIEW },
    { "pagedown", "page down", CTX_VIEW }, { "pageup", "page up", CTX_VIEW },
    { "halfdown", "half a page down", CTX_VIEW }, { "halfup", "half a page up", CTX_VIEW },
    { "top", "the start", CTX_VIEW }, { "bottom", "the end", CTX_VIEW },
    { "hex", "text / hex", CTX_VIEW },
};
#define NACTS ((int)(sizeof(acts) / sizeof(acts[0])))
typedef char acts_match_the_enum[NACTS == NACTIONS ? 1 : -1];     /* ahiru.h's order */

/* ---- the built-in keys: the same text as Ahiru.keys ------------------ */

static const char defaults[] =
    "; Ahiru - file manager: the keys. Edit freely; Ahiru reads this at start.\n"
    ";\n"
    "; One key a line: KEYS ACTION [ARGUMENT]. KEYS are typed characters, or\n"
    "; names in <>: <Up> <Down> <Left> <Right>, <S-Up> (Shift) ..., <C-d> (Ctrl),\n"
    "; <Return> <Esc> <Tab> <Space> <Del> <BS> <Help> <F1> .. <F10>.\n"
    "; Two keys in a row make a prefix: the first shows what can follow.\n"
    "; ACTION none takes a key away. [main] is the file list, [view] the viewer.\n"
    ";\n"
    "; sort takes name, size, time or ext, and \"reverse\" after it, or flip;\n"
    "; go takes a path, or volumes, root or home.\n"
    ";\n"
    "; Your own commands: run shows the output and waits, launch starts it and\n"
    "; goes on. {file} {name} {dir} {marked} {other} (the next tab) are filled\n"
    "; in, quoted:\n"
    ";   <F2> run Echo {file}\n"
    ";   <F3> launch SYS:Utilities/MultiView {file}\n"
    "\n[main]\n"
    "j down\n<Down> down\nk up\n<Up> up\n"
    "J halfdown\n<C-d> halfdown\nK halfup\n<C-u> halfup\n"
    "<S-Down> pagedown\n<C-f> pagedown\n<S-Up> pageup\n<C-b> pageup\n"
    "gg top\n<S-Left> top\nG bottom\n<S-Right> bottom\n"
    "h parent\n<Left> parent\n<BS> parent\n"
    "l enter\n<Right> enter\n<Return> enter\n"
    "i view\nE edit\nx unpack\n"
    "<Space> mark\nv invertmarks\nuv unmarkall\n"
    "yy yank\ndd cut\npp paste\npo pasteover\nuy unyank\nud unyank\n"
    "dD delete\n<Del> delete\n"
    "cw rename\na renameext\nA renameend\nI renamestart\nM mkdir\n"
    "/ search\nn next\nN prev\nf filter\n<Esc> clearfilter\n"
    ": command\n! shell\nS newshell\nR reload\n<F5> reload\n"
    "? help\n<Help> help\nq quit\n"
    "zh toggleinfo\nzp togglepreview\nzc colours\n"
    "on sort name\nos sort size\not sort time\nom sort time\noe sort ext\n"
    "oN sort name reverse\noS sort size reverse\noT sort time reverse\noM sort time reverse\n"
    "oE sort ext reverse\nor sort flip\n"
    "gs go SYS:\ngc go C:\ngl go LIBS:\ngd go DEVS:\nge go ENV:\ngt go T:\ngr go RAM:\n"
    "gv go volumes\ng/ go root\ngh go home\ncd cd\n"
    "m bookmark\n' jump\n` jump\n"
    "t tabnew\n<C-w> tabclose\n] tabnext\n[ tabprev\n"
    "1 tab 1\n2 tab 2\n3 tab 3\n4 tab 4\n5 tab 5\n6 tab 6\n7 tab 7\n8 tab 8\n9 tab 9\n"
    "V visual\ncb bulkrename\ns find\n<C-s> grep\ndt trash\nP play\n"
    "cc copypath\ncf copyname\ncF copydir\nO openwith\nZ habit\nL linemode\nw jobs\npk pack\n=c compare\n=s sync\ncr patrename\n=m checksums\n=d mount\n"
    "\n[view]\n"
    "q close\nQ close\n<Esc> close\nh close\n<Left> close\n"
    "j down\n<Down> down\n<Return> down\nk up\n<Up> up\n"
    "<Space> pagedown\nf pagedown\n<C-f> pagedown\n<S-Down> pagedown\n"
    "b pageup\n<C-b> pageup\n<S-Up> pageup\n"
    "d halfdown\n<C-d> halfdown\nu halfup\n<C-u> halfup\n"
    "g top\n<S-Left> top\nG bottom\n<S-Right> bottom\n"
    "x hex\n";

/* ---- the engine is the core's (fs/keymap.c) ------------------------------ */

static const char *const ctxnames[] = { "main", "view" };
static const char *const ctxheads[] = { "In the file list:", "In the viewer:" };

void keys_load(char *bad)
{
    km_setup(acts, NACTS, ctxnames, ctxheads, 2);
    km_load(KEYFILE, defaults, bad, 120);
}

int keys_isprefix(int ctx, int k) { return km_isprefix(ctx, k); }
int keys_find(int ctx, int k1, int k2, const char **arg) { return km_find(ctx, k1, k2, arg); }
int keys_hints(int ctx, int k, const char **items, int max, char (*lab)[12], char (*desc)[48])
{
    return km_hints(ctx, k, items, max, lab, desc);
}
void keyname(int k, char *dst) { km_keyname(k, dst); }
void keys_first(int ctx, int act, char *dst) { km_first(ctx, act, dst); }
int keys_action(const char *name) { return km_action(name, CTX_MAIN); }
void keys_free(void) { km_free(); }

char *keys_help(void)
{
    return km_help("Ahiru - file manager - keys (PROGDIR:Ahiru.keys changes them)\n\n",
                   "\nIcons (.info) always travel with their files, shown or not. Archives\n"
                   "(lha, lzx, zip) and disk images (ADF, ISO) open like folders.\n");
}
