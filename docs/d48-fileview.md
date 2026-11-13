# D48: the file view, `files` (design note, revision 1)

Roadmap block 5, "the file view, viewers, preferences; TrueType fonts": this note is the file view only (the
preferences are built, D38 "Preferences"; viewers and TrueType are later items of their own). D38 section 7 listed
"a graphical file view (Ahiru-style)" as later work, and its icons start "Ahiru in a terminal until a graphical file
view exists". Written 2026-11-13 and built the same day on branch `files`. No number is claimed: `files` is an ordinary
client of the display protocol (D38 section 4) and needs no new message, port or system call.

## 1. In plain words

- **`files`** is a window on Zenix Desktop that lists a folder: names, sizes and dates, folders first. It is Ahiru's
  model in a window of its own: the same vi keys (j k h l, gg G, Space, yy dd pp, dD, cw, M, /, q), the same kind of
  keys file, and the mouse besides (a click picks a line, a double click opens it).
- **Lean.** One program of 40 KB on the disk (libdisplay and libz linked in, as every client), loaded only when
  opened; it reads only what it shows. Ahiru (240 KB, previews, archives, tabs) stays, in a terminal, for everything
  more.
- **80 columns first.** At 640 pixels a window holds at most 78 columns of the 8 x 8 font, so the view is one column
  (the folder) by default; a window 72 columns or wider (zoomed on 640, or on a wider screen) shows the parent folder
  on its left as well, Ranger's Miller columns collapsed to two. No preview column: there is no room at 640, and the
  previews are Ahiru's.
- **Opening a file** runs the program that fits in a terminal window (`term`): a text file in `less`, `E` in the
  editor, a program after a question; a picture in the picture viewer `show` (D50, docs/d50-viewers.md: its own
  window, no terminal); anything else asks what to open it with.
- **Copy, move, rename, delete** ask first when anything would be lost, and the copying, moving and deleting are done
  by the commands themselves (`cp -r`, `mv`, `rm -r`), so there is one copy of that code and its rules.

## 2. How it fits D38

- **A client like `term` and `deskprefs`** (D38 section 5): it opens the display with `dpy_open("files")`, one
  `WK_NORMAL` window sized in 8 x 8 cells (`stepw`/`steph` 8, so a resize is in whole cells as `term`'s), draws with
  `dpy_text` and `dpy_fill` in batches (`dpy_flush` once an event is handled), and redraws from its own model on
  `EV_EXPOSE` and `EV_RESIZE` (libdisplay keeps nothing). Placement, focus, the frame and the close gadget are zdesk's;
  the close gadget is quit.
- **The 8 x 8 font** for the listing (D38 3.2: text on an 8-pixel column is a byte a plane a line, the fast path): the
  columns line up and every row is one text order. The desktop's own font (`DPYF_UI`) is for titles and the bar, as
  D38 "Fonts" decided for terminals.
- **Every pixel once** (D38 "The bar it must clear"): a row is drawn whole with its background (the text padded to
  the window's width), never cleared first; a cursor move redraws the two rows that changed; a scroll of a few lines
  is one `dpy_copy` and the new rows; the window is drawn whole only when it opens, on an expose, a resize, a new
  folder.
- **Menus in the bar** (D38 revision 2): `dpy_menu` gives the bar "Files" (Open, Parent, Reload, Show hidden, Shell
  here, Quit)
  and "Edit" (Copy, Cut, Paste, Delete, Rename, New folder, Mark all, Unmark all); a pick is the same action as its key.
- **zdesk's starters** (D38 step 5): the Zenix menu's "Files" and the desktop's Files icon start `files` instead of
  `term -e ahiru`. The menu gets no extra "Ahiru" line: every desktop test clicks the menu's items by their place
  (About is last), so a new line is the partner's choice (section 9, 2); `ahiru` stays a command in any terminal.
  `/etc/zdesk/menu` and `/etc/zdesk/icons` only change their lines, not their format; an image without `/bin/files`
  gets `term -e ahiru` back (tools/mkroot.sh).
- **Not a desktop feature in the text console** (owner 2026-10-23): `files` needs the display and says so without it
  ("files: the desktop: ..."), exit 1. On a text console Ahiru is the file manager.

## 3. The listing

- **Read lean.** The folder is read with `open` and `readdir`, whose records carry the name and its type (1 file, 2
  folder, 7 link: ext2 with the file-type feature, ZenixFS, FAT, FFS, PFS3, ISO 9660, exFAT). Only an entry whose type
  is 0 (ext2 without the feature) is `lstat`-ed while reading. The names go into one block of memory (no allocation a
  name). Sorting needs the names and types only: folders first, then by name without case (Ahiru's order).
- **`lstat` only what is shown.** Size, date and mode are read when a row is first drawn and kept: opening a folder of
  500 files costs the 500 `readdir` records (what `ls` pays) and 20-30 `lstat`s for the rows on view, not 500. Sorting
  by size or date (`os`, `ot`) reads the rest first, once.
- **A row:** a mark column (`*` for a marked entry), the name (a folder with `/`, a link with `@`; cut with `~` when
  long), the size as `ls -h` writes it (`512`, `1.2K`, `34K`, `5.6M`; nothing for a folder) and the date as `ls -l`
  writes it (`Nov 13 02:47`, or `Nov 13  2025` when older than half a year). The clock is the kernel's (UTC), as `ls`.
  Below 40 columns the date goes, below 30 the size.
- **The top row** is the folder's path (cut at the left with `~` when long); **the bottom row** is the status: the
  entry's mode, owner and full size, or a message, a question, or the line being typed (rename, new folder, search,
  open with). The title is `Files: NAME` (the folder's last name).
- **Hidden names** (a leading `.`) are not shown, as `ls` and Ranger; `zh` shows them (Ahiru's key for hiding
  `.info` icons, the same idea on Unix).
- **The cursor is remembered:** going to the parent puts it on the folder you came from (Ranger's and Ahiru's way).
- **Limits** (said, never silent): at most 4,096 entries a folder and 64 KB of names; past either the listing stops
  there and the status says "only the first N entries shown". A name is at most 63 bytes (the `readdir` record's).

## 4. Keys and the mouse

**Keys: Ahiru's, and configurable the way Ahiru.keys is** (the parser is `display/lib/keys.c` since D50, shared with
`show`). The built-in keys are text in the program, in the format
of `Ahiru.keys` (ports/ahiru/upstream/ahiru/src/keys.c): one key a line, `KEYS ACTION [ARGUMENT]`; KEYS are typed
characters or names in `<>`: `<Up> <Down> <Left> <Right>`, `<S-Up>` (Shift) ..., `<C-d>` (Ctrl), `<Return> <Esc>
<Tab> <Space> <Del> <BS> <Help> <F1>`..`<F10>`; two keys in a row make a prefix (the first shows on the status line
what can follow, made from the bindings); `ACTION none` takes a key away; `;` and `#` start comments; `[main]` starts
the section of the file list (the only one). `~/.config/files/keys` is read after the built-ins and adds to and
changes them; when it is missing `files` writes it with every built-in key, a full list to edit (Ahiru's way). A line
not understood is said on the status line at start ("keys: line 12: unknown action frob") and the others still work.
The action names are Ahiru's where the action is the same, so a user who knows one file knows the other.

| Keys (built in) | Action |
|---|---|
| `j` `<Down>`, `k` `<Up>` | down, up |
| `J` `<C-d>`, `K` `<C-u>`; `<S-Down>` `<C-f>`, `<S-Up>` `<C-b>` | half a page, a page |
| `gg` `<S-Left>`, `G` `<S-Right>` | top, bottom |
| `h` `<Left>` `<BS>` | the parent folder |
| `l` `<Right>` `<Return>` | enter a folder, or open the file (section 5) |
| `i` | view it (`less`) |
| `E` | edit it |
| `<Space>`, `v`, `uv` | mark and move on, invert the marks, unmark all |
| `yy`, `dd`, `pp`, `po`, `uy` | yank (copy), cut, paste here, paste replacing, forget the yank |
| `dD` `<Del>` | delete (asks) |
| `cw`, `a`, `A`, `I` | rename: an empty line, the cursor before the extension, at the end, at the start |
| `M` | a new folder |
| `/`, `n`, `N` | search names (a part, without case), next, previous |
| `on` `os` `ot` `oe`, `oN` `oS` `oT` `oE`, `or` | sort by name, size, time, extension; reversed; flip |
| `zh` | show or hide hidden names |
| `g/`, `gh`, `gt` | go to `/`, home, `/tmp` |
| `S` | a shell here (a terminal in this folder) |
| `R` `<F5>` | read the folder again |
| `?` `<Help>` | the keys (a list in the window) |
| `q` | quit |

**The mouse** (drag-free in this step): a left click on a row moves the cursor there; a second click on the same row
within half a second opens it (as `l`); Shift with a click marks or unmarks the row; a click on a row of the parent
column goes to that folder. The menus are in the bar (section 2). Dragging files (drag and drop) needs D38's later
"drag and drop" in the protocol: not now.

## 5. Opening a file

`l`, Return or a double click on a file. The kind is found from the file's first 512 bytes, read only then:

| What | Done |
|---|---|
| a folder, or a link to one | entered |
| a Zenix program (`ZEXE`/`ZEX2` at byte 4), an AmigaOS program (the hunk header `000003F3`: ZINE runs it) or a script (`#!`), with any `x` bit (the kernel's loader says no to one this user may not run) | asks "Run NAME? (y/n)", then `term -H -e PATH` |
| text (no NUL byte, few control bytes) | `term -H -e less PATH` |
| a picture `show` reads (IFF ILBM: `FORM` .... `ILBM`; GIF: `GIF87a`, `GIF89a`), when `/bin/show` is there (added by D50, docs/d50-viewers.md) | `show PATH`, started bare in the folder (no terminal: it draws its own window) |
| anything else (a picture, a sound, an archive, data), and anything not a plain file (a device, a FIFO: never read, it could wait for ever) | asks "Open NAME with:", the line typed is the program and its words (at most 12), the path added last, run as `term -H -e WORDS... PATH`; Esc or an empty line does nothing |

`i` always views with `less`; `E` edits with `$EDITOR`, else `sumi` when `/bin/sumi` is there, else `edit` (Ahiru's
order on Zenix, ports/ahiru/README: E). `S` runs `term` in the folder. Every program is started with `sys_spawn` and
its words as they are (no shell, so no quoting of a name with spaces); `files` reaps them and goes on at once.
**term's new `-H` (hold, as xterm's `-hold`):** Zenix's `less` just prints a text that fits the screen and ends (as
`more`), so a terminal that closed with its program would flash and vanish; with `-H` the window stays, its title
"less (ended: a key closes)", until a typing key, Return, Esc or the close gadget. A long text paged in `less` then
needs one more key after `q`. The editor and the shell (`E`, `S`) run without `-H`.

*A question for the partner (section 9, 1):* a program is run in a terminal, because `files` cannot tell a program
that draws its own window (`deskprefs`) from one that needs a terminal (`ls`); a desktop program opens one extra,
empty terminal that closes when it ends. The default is the safe one: every program gets its terminal.

## 6. Copy, move, rename, delete

- **The set** is the marked entries, else the one under the cursor (Ahiru's rule). `yy` remembers it as a copy, `dd`
  as a move (paths, absolute); nothing happens until `pp` in the target folder.
- **Paste** runs `/bin/cp -r SOURCE... FOLDER` (a yank) or `/bin/mv SOURCE... FOLDER` (a cut), the commands' own code
  (cp's recursion, mv's rename and its copy-and-remove across filesystems), in runs of at most 200 paths and 7 KB of
  words (the kernel's argument limit is 256 / 8 KB, docs/limits.md). Before it:
  - a source pasted into its own folder is refused ("already here": `cp a a` would empty the file), and a folder into
    itself or below it ("cannot copy a folder into itself");
  - the paths are compared as the kernel resolves them (`zrealpath`): a folder reached through a link is the folder
    itself (`files` shows every folder by its resolved path), so the same file can never be both source and target;
  - names that already exist in the target are counted and asked about ("2 names exist here: replace them? (y/n)",
    "(a folder: merged into)" when one is a folder, as `cp -r` merges);
    `po` replaces without asking (Ahiru's "paste, overwriting everything").
  - After a move the yank is forgotten; after a copy it stays (paste it again elsewhere).
- **Delete** (`dD`, Del) asks "Delete NAME? (y/n)" or "Delete N entries? (y/n)", folders with what is in them, then
  runs `/bin/rm -r PATH...`. Nothing goes to a trashcan (Zenix has none).
- **Rename** (`cw`, `a`, `A`, `I`) edits the name on the status line (Left, Right, Home and End move, BS and Del
  delete, Return renames, Esc leaves it), then `rename()` in the same folder (what `mv` does for one name); a name that
  exists, an empty name or one with `/` is refused with a message. **New folder** (`M`) is the same line, then `mkdir`.
- **While a command runs** the status line says "copying..." (moving, deleting), the window still answers expose
  events, Esc stops the command (SIGTERM; a second Esc SIGKILL), the close gadget quits when it has ended, and other
  keys and clicks are not taken. Its standard error goes to a pipe: the first line of it is shown when
  it ends with a non-zero status ("cp: /mnt/dh0/x: read-only file system"), and the folder is read again.
- **Checked on the disk** by the test (section 8), as `ahiru-test` checks Ahiru's.

## 7. Memory, speed, size

- **Disk:** 40,564 bytes (code 22 KB of its own, libdisplay 2.5 KB and libz linked statically, as every client;
  `deskprefs` is 21 KB, `term` 38 KB). `term` grew 640 bytes for `-H`. Built at `-Os`, the whole program: what the user feels is the readdir and the drawing, which are the kernel's, the
  filesystem's and displayd's.
- **Memory:** the program's data, the names (a 500-name folder about 10 KB of names and 12 bytes an entry: 16 KB), the
  parent's listing when the parent column is shown, the keys table (about 100 bindings, 2 KB). Nothing in chip RAM of
  its own: the window's kept copy is displayd's (D38 6.2).
- **Speed:** opening a folder is the `readdir` records, a sort of the names (qsort, a few thousand comparisons at 500)
  and one screenful of rows (one text order each). The test measures it in FS-UAE on the stock A1200 with 4 MB of
  fast RAM (emulator figures, never a speed claim): a folder of 500 files, from the key to the drawn window, against
  `ls` of the same folder to `/dev/null`. The real A1200's figure is for its first run.
- **The demo floppies (D25):** `files` goes on both: it is what the desktop's Files icon starts. Its room is measured
  in the build (section 10 of the commit's notes in DESIGN D48).

## 8. Tests

`tools/files-test.sh` (FS-UAE, the stock A1200 with 4 MB of fast RAM, through the cage rig as `prefs-test`, so in
check.sh's STOCK: screenshots at times on the PC's clock):
- a scratch tree in `/tmp/fv`: files of known sizes, a folder, a text file, a hidden file, a program; `files -v
  /tmp/fv` from zdesk: `-v` writes every row it draws to its standard output (`files: row N: TEXT`) and where its
  window is, so the window's text is compared line by line, and screenshots are checked pixel by pixel
  (`tools/dpy-test.py px`: the cursor's row in the selection colour where the window says it is);
- the keys typed through CON_INPUT (`gcontest keys`): j, k, G, gg, l into the folder, h back with the cursor on it,
  zh (the hidden file shown), J (half a page: the rows scrolled in drawn), the keys file written at start, then a key
  added and a bad line in it: the line said, the key works;
- the mouse (`gcontest mouse`): a click moves the cursor, a double click enters the folder;
- yank and paste into the folder (the copy byte for byte with `cmp`), a paste where the name exists (asked; n leaves
  the file there with its own content; `po` replaces it), a paste into the entry's own folder and into the same folder
  through a link (both refused), rename (the old name gone, the new there), cut and paste (moved), a new folder, delete of a marked pair (asked,
  then gone), each checked on the disk with `ls` and `cmp` afterwards;
- opening the text file: `term -e less` started (the process list); the program: asked;
- a folder of 500 files: listed, its time printed, the count right, `G` on the last.

## 9. Questions for the partner (each with the default built)

1. **Programs from the file view:** in a terminal (the default: right for text programs, one empty terminal for a
   desktop program), or started bare (right for desktop programs, a text program then has no screen)? A later answer
   could be a word in the program's file or a list of desktop programs in `/etc/zdesk/`.
2. **The Files icon and menu line:** `files` (the default now) with Ahiru as its own line; or the other way round?
3. **Hidden names:** hidden by default (as `ls`, Ranger and Ahiru's `.info` rule), `zh` shows them. Default: hidden.

## 10. Not in this step (later, each its own approval)

Previews (Ahiru's third column), drag and drop between windows and onto the desktop (the picture viewer is built:
D50, opened by section 5's rule), tabs, bookmarks, filters, archives as folders, a trashcan,
copying in the background with a progress bar, file icons (`.info`), the settings remembered between runs (sort,
hidden), the desktop's font for the listing.

---
Revision 1, 2026-11-13: written and built on branch files; one independent review (section "As built" in DESIGN D48
says what it found).
