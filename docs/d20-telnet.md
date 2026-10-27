# D20: telnet, the BBS terminal and pseudo-terminals (design note, revision 2)

Status: **design, revision 2 after independent reviews (section 13), nothing built.** D19 section 10 step 11: "telnet
(its own note: pseudo-terminals, SIGHUP, the BBS terminal)". Roadmap block 2: `telnet` "made for BBSes, the owner's
daily use: ANSI colours, the PC character set and Amiga fonts, the BBS window size, downloads". The short version is
DESIGN.md D20. Facts about today's code are *read* from main at f03180e (2026-10-27); facts about boards and terminals
are *read* from the knowledge base (`bbs/`, `amiga/ansi-art-*`, `amiga/file-transfers-*`, `go/zmodem-and-telnet.md`),
from CTerm's source (the owner's own AmigaOS terminal, MIT, used on real boards) and, where marked, from SyncTERM's
source (`src/conio/cterm.adoc`, `cterm.c`, `cterm_cterm.c`, `cterm_ecma48.c`, `src/syncterm/bbslist.c`, sbbs master,
checked by the review); sizes are *estimates* unless marked.

## 1. What the owner gets, in plain words

- `telnet bbs.example.org 2323` from any console: the board's screens in colour, with the right characters (the PC's
  box and block characters for PC boards, Topaz and Latin-1 for Amiga boards), the Swedish keyboard working, and the
  board told how big the window is.
- **Downloads by Zmodem start by themselves** when the board starts sending; uploads when the board asks. Files go to
  the directory telnet was started in, never over an existing file.
- `telnet host` to a Unix machine (the PC) works too, as a plain xterm-like terminal.
- One key (Ctrl-]) leaves or controls telnet; everything else, Ctrl-C included, goes to the board.
- Later, its own step: logging in to Zenix *from* another computer (a telnet server), which needs pseudo-terminals.

## 2. Today (read)

- **The console** (`term.c`, D14): 80 columns, 32 rows on PAL (25 on NTSC), ECMA-48 parser, 16 colours by SGR,
  region scrolls by blitter, the whole-screen scroll a Copper pointer bump, 200 lines of scrollback. The grid and the
  scrollback hold bytes (and colour bytes), not glyphs. Glyphs are the ROM's Topaz 8 for bytes 32-255 (Latin-1;
  `gfx.c glyphs[224][8]`, indexed `ch - 32`, a byte below 32 drawn as `?`; with more than one plane a second 7 KB copy,
  `rglyph`), bytes below 32 are never stored by `put1`, wrapping is deferred (VT style), **LF is always CR LF**
  (`term.c` ~555), `ESC[2J` does not move the cursor, SGR 1 is bold (pixel smear) and never a brighter colour (D14's
  reviewed answer 2), SGR 5 (blink) is ignored, `ESC[?7l` is ignored (only `?25` and `?1` are acted on), `ESC[6n`
  answers into the asking console's input. Only the shown console (`con_cur`) is drawn; the others update their
  grids. `term_log` (the kernel log, panics) writes into the shown console's grid. The palette is Zenix's own (`gfx.c
  gfx_palette`: yellow 0xBB2 where the PC has brown, a lighter blue), loaded by `build_cop`.
- **The depth:** 16 colours on AGA with fast RAM (the PiStorm A1200), **8 on the stock A1200** (2 MB chip RAM, no
  fast RAM; bright foregrounds are the colour plus bold, bright black is grey, bright backgrounds are not shown),
  `/etc/console` chooses 2, 4, 8 or 16 at the next boot, and root may switch at any time (`SYS_console CON_COLOURS`,
  `gfx_set_colours`, which rebuilds or frees `rglyph`; D14 section 11).
- **The keyboard** (`kbd.c`): AmigaOS's Swedish keymap `s` (Latin-1: å ä ö as 0xE5 0xE4 0xF6); in raw mode Backspace
  is 0x7F, Del `ESC[3~`, Help `ESC[2~`, arrows `ESC[A` (`ESC O A` under DECCKM), F1-F10 xterm's. Amiga+F1..F10 switch
  consoles and never reach a program.
- **The tty** (`tty.c`): one per console (`NTTY` = 11, index 0 unused; `get()` maps any other number to console 1),
  line discipline in the kernel, `TTY_RAW` and `TTY_NOSIG`, the foreground a process group (D16); a process's
  controlling tty `ctty` is always a console (`proc.c` ~642 clamps it). `tty_write` draws in 256-byte pieces and runs
  `kwork` between them, which takes input and wakes waiters but **lets no other process run**: the kernel is never
  preempted. (How long a write draws is not measured on a real 68020: `tty.c`'s comment says "seconds for 4 KB",
  architecture.md's FS-UAE/PiStorm figure of 11 ms per 79-character line gives about 0.6 s; section 6.)
- **The scheduler** (D10): the slice is 5 ticks (50 ms, `kernel.h` `SLICE`); a woken process takes the CPU at the
  next return to user mode only if its level is strictly better (`want_switch`); equal levels alternate at slice end.
  netd runs at nice 0 (frozen, D19 6.3).
- **The network** (D19 steps 0-8): BSD sockets, `poll` over sockets and ttys (D16), DNS through netd, SLIP over
  `/dev/ttyS0` for every Amiga (the serial port is the network's while it runs: no `cu`; received bytes wait for netd
  in the 1 KB receive ring, 89 ms of the line, `serial.c`), the A2065 in FS-UAE, GENET for the PiStorm at step 9.
- **`cu`** (D12): two processes, `~.` ends it, keys raw and unchanged. No file transfer.
- **Signals** (D16): SIGHUP is generated only for stopped children of an exiting process (`proc_exit`, D16 rule 12);
  SIGWINCH never; no sessions; controlling terminals are only the consoles.

## 3. The telnet client

### 3.1 The protocol (RFC 854, option negotiation RFC 855)

- **One process, one loop:** `poll` on the socket and standard input (D16), no second process as `cu` needs. Bytes
  from the socket go through the telnet decoder (IAC commands out, `IAC IAC` -> 0xFF, `CR NUL` -> CR unless *their*
  side agreed WILL BINARY), then the terminal filter (section 4.4), then to standard output. Keys go through the key
  translation (3.3), then the encoder (0xFF doubled; CR -> `CR NUL` unless *our* side agreed WILL BINARY) to the
  socket. BINARY is per direction (RFC 856), and each direction follows its own agreement.
- **Options, with RFC 1143's "Q method"** so a misbehaving server can never make the two sides loop (as CTerm, *read*
  `net.c` 425-475; the directions checked by both reviews):

| Option | Ours (WILL) | Theirs (DO) | Why |
|---|---|---|---|
| BINARY 0 (RFC 856) | asked | asked | 8-bit characters and file transfers without `CR NUL` folding; refused is fine for text (section 5 for transfers) |
| ECHO 1 (RFC 857) | refused | accepted | the board echoes. **No local echo in the BBS modes** even if the server never says WILL ECHO (boards and serial bridges such as tcpser echo by themselves; SyncTERM and CTerm never echo locally); local echo only in the Unix mode when the host will not echo |
| SGA 3 (RFC 858) | accepted | asked | character at a time, no GA |
| TERMINAL-TYPE 24 (RFC 1091) | accepted | - | `ANSI` in both BBS modes, `XTERM-16COLOR` in the Unix mode (D14's `TERM`); a second SEND gets the same name. CTerm sends `ANSI` only in its PC mode and `XTERM-256COLOR` otherwise: `ANSI` for Amiga boards is a deliberate difference (the Zenix console is not a 256-colour xterm, and boards treat `ANSI` as ANSI-BBS) |
| NAWS 31 (RFC 1073) | asked | - | the window size, at once when agreed: two 16-bit big-endian numbers, a 255 byte doubled (3.2) |
| everything else (LINEMODE 34, NEW-ENVIRON 39, TSPEED, LFLOW, STATUS, TIMING-MARK ...) | refused (WONT) | refused (DONT) | not needed for a board or a Unix login |

- **Subnegotiation** is collected into a 64-byte buffer (TTYPE SEND and NAWS are a few bytes); a longer one is dropped
  whole, never written past. IAC commands other than SB/SE and the four verbs (NOP, GA, AYT, BRK ...) are ignored.
- **When telnet starts negotiating:** on port 23 at once (as CTerm does); on any other port only after the server sent
  its first IAC (BSD telnet's rule), so a plain TCP bridge never receives `ÿû...` as typed keys. IAC is always decoded.
  `telnet -r` (raw) turns the decoder and encoder off for a plain TCP service (as `nc`).
- **Connect and disconnect:** `telnet HOST [PORT]` (port 23 by default; a name through `getaddrinfo`, D19's
  `SOCK_RESOLVE`). Until connected the tty is in its normal mode, so ^C ends a slow lookup or connect (both wait
  interruptibly, D19 4.3). Connected: "Connected to HOST. Escape key is Ctrl-]." and the tty goes `TTY_RAW |
  TTY_NOSIG`. The other side closing ends telnet with "Connection closed by HOST.", the console's font and rules
  dropped and the screen cleared (4.2). Exit status 0 after a normal close, 1 when the connection failed, 2 for wrong
  arguments.

### 3.2 The window size (NAWS)

**Choice: tell the truth, 80 x 32 on PAL and 80 x 25 on NTSC** (`SYS_ttyinfo`). Boards that read NAWS use the extra
rows for longer lists and pages (AmiExpress: lines per screen = NAWS rows - 1, at least 10: *read*,
`bbs/lines-per-screen.md`); screens drawn for 25 rows sit at the top and the cursor goes on below them, as on any
larger terminal. It keeps the fast path: a full-screen scroll is the Copper pointer bump, and scrolled lines go to
the scrollback (Amiga+Up), where message text can be read again.

**Not taken now: an 80 x 25 window inside the 32 rows** (owner question 3). It needs the console to act as a 25-row
terminal (every clamp, `ESC[6n`, `ESC[2J`, the scroll), and its scroll is then a region scroll: a blitter copy of 25
rows on every line feed (about 48 KB at 3 planes, *estimate* 10-15 ms on a stock A1200, against nothing for the
pointer bump), and region scrolls push nothing into the scrollback. A telnet-side filter that clamps cursor rows
cannot do it alone (cursor-down and line feeds would need a full screen model). If wanted, it is a later console
setting of its own (a per-console row count), measured.

### 3.3 The keyboard

The keys arrive from the console as today; telnet translates per mode:

| Key (what the console gives) | PC board | Amiga board | Unix |
|---|---|---|---|
| Backspace (0x7F) | 0x08 | 0x08 (Amiga boards expect BS: AmiExpress, the BBX web terminal's rule) | 0x7F |
| Del (`ESC[3~`) | 0x7F | 0x7F | `ESC[3~` |
| Return (CR) | CR (`CR NUL` while our WILL BINARY is not agreed) | the same | the same |
| Arrows, F1-F10 | as given (`ESC[A` ...) | as given | as given |
| Help (`ESC[2~`) | as given: boards read it as Insert | as given | as given |
| å ä ö Å Ä Ö and other Latin-1 | CP437 (0x86 0x84 0x94 0x8F 0x8E 0x99, checked by the review against a cp437 codec and `font_8x8.c`; no CP437 form: not sent, the bell) | as given (Latin-1) | UTF-8 (two bytes) |
| Ctrl-C, Ctrl-Z, every other control | as given (to the board) | as given | as given |
| Ctrl-] | the escape key (3.4) | | |

The Swedish keymap's dead keys are already spacing characters (`kbd.c`), so nothing composes. Characters only the Alt
layer gives (`[ ] { } \ | @`) arrive as ASCII and need nothing.

### 3.4 The escape key

**Ctrl-]** (BSD and Linux telnet's; `-e CHAR` changes it, `-e none` turns it off). On the Swedish keymap `]` is Alt
with raw key 0x1B (the ¨/^ key right of Å; `kbd.c` 76-97), and `key_down`'s rule (`c & 0x40` and below 0x80: `c &=
0x1F`) turns Ctrl+Alt+that key into 0x1D (*read* by both reviews, not yet run; the keyboard test checks it). It opens
a one-line local prompt on the bottom row, `telnet>`: `q` close and quit, `z` suspend (font and rules dropped, the
console restored, then SIGTSTP to itself: the shell's job control; on SIGCONT telnet sets raw mode, font and rules
again), `d` receive files now (a board that starts Zmodem without its header; later X/Ymodem), `u FILE...` send files,
`p`/`a`/`x` switch to the PC, Amiga or Unix mode, Ctrl-] sends the escape key itself, Return goes back. `cu`'s `~.`
is not taken: one way out, the one every telnet has, and a board's hotkeys never meet a line start rule. The prompt
never writes into the last column of the last row (the console ignores `ESC[?7l`, so that would scroll the screen).

### 3.5 The three modes

| | **PC board** (`-p`) | **Amiga board** (`-a`, the default) | **Unix** (`-x`) |
|---|---|---|---|
| Characters shown | CP437 through the console's PC font (4.2) | Latin-1, the console's own Topaz | Latin-1; UTF-8 from the host is decoded and shown as Latin-1, anything else as `?` (until roadmap block 3's UTF-8 console) |
| Console rules (4.3) | `PCCTRL WRAPNOW CLRHOME BRIGHT LFONLY CGAPAL` (+ `ICE` on request) | `WRAPNOW CLRHOME BRIGHT CGAPAL` (+ `ICE`) | `LFONLY` |
| TERMINAL-TYPE | `ANSI` | `ANSI` | `XTERM-16COLOR` |
| Filter (4.4) | music, FF, SUB, the iCE and font switches | the same, plus 0x9B as CSI and the Amiga console's own sequences | none (UTF-8 decoding only) |

**The default is the Amiga board** (the owner's daily use, and the console's native look). A board can switch to the
PC mode with SyncTERM's font code (4.4), but few will: boards send it only after recognising SyncTERM, and telnet does
not claim to be SyncTERM (4.4, device attributes). So the mode is chosen by the caller: `-p`, the escape menu, and a
phone book that remembers each board's mode (section 9 step 5, owner question 4). No guessing from the bytes in
revision 2: Charcoal's learned weights tell Amiga from PC art 98% of the time on whole files (*read*,
`amiga/ansi-art-real-world-traps.md`), but a live stream switching fonts mid-screen on a wrong guess redraws
everything wrongly; a later revision may add it as an option once measured on real boards.

**When the console refuses the ops** (telnet's output is not a console: a pty later, or a file; no memory for the
font): telnet says so once and falls back by itself: in the PC mode it maps CP437 to the nearest Latin-1 character
(box and block characters to `+ - | #`, a 256-byte table), and draws without the rules. Never an error that ends the
call.

## 4. The BBS terminal

### 4.1 Where the screen work happens

| | **A. The console draws; telnet filters the stream** | B. telnet is a full terminal emulator | C. A display server |
|---|---|---|---|
| CPU per byte | one parse (the console's, -O2) plus a light filter | two parses: telnet's screen model, then the console's | not built (D4, roadmap block 5) |
| Glyphs (CP437) | the console must have them anyway | the same: a user program cannot draw (no framebuffer, D4) | - |
| Scrollback, console switches, `ESC[6n` | as every console program has them | must be rebuilt in telnet | - |
| Size | a small kernel addition, a small program | a second emulator (CTerm's `vt.c` is ~3,000 lines) | - |
| Other users | `cu`, a future telnet server's sessions, a pager showing a saved `.ans` | only telnet | - |

**Choice: A** (both architecture reviews agree it fits D4, D14, the Linux-style console rule, and roadmap block 3's
"the PC set comes earlier, with telnet"). The console gets what only it can do (a second font, seven rules that
change how bytes are stored and drawn); telnet does what is a rewrite of the stream (protocol, music strings, the
Amiga console's own codes). Golden path: the one parse that matters is the kernel's fast path, and nothing is drawn
twice.

### 4.2 The PC font (the font question)

- **The glyphs:** a 256-glyph 8 x 8 CP437 font. Source: **Linux's `lib/fonts/font_8x8.c`** (`// SPDX-License-Identifier:
  GPL-2.0`, 256 glyphs, `FONTDATAMAX` 2048, CP437 order: 0x84 ä, 0x86 å, 0xB0 ░, 0x01-0x1F the smiley set, 0x00 and
  0xFF blank; its header says it was generated by `cpi2fnt` from a DOS code-page font file; *checked* by the review
  at git.kernel.org and in a local Linux tree). GPL-2.0 is D9's licence; its notice travels in
  `/usr/share/licenses/` like lwIP's (D19 owner decision 6). Not VileR's Ultimate Oldschool PC Font Pack (CC BY-SA 4.0
  is not GPL-2 compatible). 8 x 8 is the console's cell; PC art was drawn for 8 x 16 VGA, so it looks as in the PC's
  50-line mode (IBM VGA50, SAUCE's name for it): the honest price of 80 x 32 with one font height.
- **The file:** `/usr/share/consolefonts/cp437-8x8.psf`, **PSF1** (Linux's console font format: `36 04`, mode 0,
  height 8, 2,048 bytes of glyphs; 2,052 bytes), built from the C source by a host tool, on every full root image and in
  the base package (not on the RAM floppy, whose `RAMAPPS` are full: telnet is not there either).
- **Loaded on demand, per console, by the program that uses it** (golden path: nothing for machines that never call a
  PC board): `SYS_console` gets a new op, `CON_FONT` (4.5 gives the exact interface). The kernel checks the data as
  untrusted input (inside the caller's memory, magic `36 04`, mode 0 (no 512 glyphs, no Unicode table), height 8, size
  exactly 2,052), copies the 2,048 bytes into a block of its own and, with more than one plane, builds that font's "four
  bytes a line" table (8 KB, 256 glyphs, as D14's 7 KB `rglyph` for 224). Memory: 2 KB, plus 8 KB at 2-4 planes, only
  while a PC board is open.
- **How it is drawn (the review's model):** only the shown console is drawn, so `gfx.c` keeps one pointer, "the font
  of the shown console" (Topaz's 224-glyph table indexed `ch - 32`, or a loaded 256-glyph table indexed `ch`), set by
  `con_switch` and when the font is loaded or dropped; `put_glyph` takes the index from it (one more pointer and
  offset on the -O2 path, measured with `bench`'s screen line). `gfx_set_colours` rebuilds or frees a loaded font's
  four-bytes table with Topaz's. **`gfx_panic` sets the shown font back to Topaz and the rules to none** before it
  draws, so a panic is always readable; the ordinary kernel log written while a PC board is shown goes into that
  console's grid like any output and is drawn in its font (ASCII 0x20-0x7E is the same in CP437 and Topaz).
- **One owner, last setter wins:** a second `CON_FONT` on the same console replaces the font and becomes its owner;
  the font is dropped when **its current owner** ends (as `tty_release` drops a raw mode: `proc_exit`), on `CON_FONT`
  with a null pointer; `ESC c` resets the terminal state but never the font or the rules (a board's reset would
  otherwise lose them). It is never shared: another console, another user's session and panics keep Topaz.
- **The grid holds bytes, so a font change redraws everything in the new font** (an accepted limit, as Linux's
  `setfont`): while a PC board is shown, earlier scrollback lines are drawn in the PC font; after the font is dropped,
  the board's bytes would be Latin-1 junk and stored control-character glyphs `?`. So telnet clears the screen
  (`ESC[2J ESC[H`) before it drops the font (at its end, at `z`, at a mode switch), and the kernel redraws the console
  on a drop. A mid-screen font switch by the board leaves the screen as the board then draws it.
- **Why not built in:** 2-10 KB in every machine's kernel for a few users' boards, and the RAM floppy is short
  (section 6). **Why not loaded by `init` at boot:** the same cost for everyone. **Why not root only:** telnet runs as
  the user; the op is limited to a console the caller is on (4.5).
- **UTF-8 later (roadmap block 3):** a loaded `CON_FONT` font means, and is frozen to mean, **"8-bit mode: each byte is
  one glyph of this 256-glyph font; the console decodes no UTF-8 while it is loaded"** (the role of Linux's `ESC % @`).
  Block 3's Unicode fonts (PSF2 with a Unicode table) are told apart by their magic and get their own meaning then.
  When the console decodes UTF-8, telnet's Amiga mode converts Latin-1 to UTF-8 itself (a 128-entry table) or loads
  the 8-bit mode with a Latin-1 font; decided in block 3, nothing here blocks either.
- **Topaz for Amiga boards is already there** (the machine's own ROM Topaz 8, Latin-1, read at boot, never shipped).
  **The scene's other Amiga fonts cannot be shipped as they are:** TrueSchool's pack (github.com/rewtnull/amigafonts,
  which SyncTERM ships) says "GPL-FE" with no GPL version in its README, while its font files' own metadata says CC
  BY-NC-SA 3.0 ("NC" is not GPL-2 compatible), and its Topaz is the ROM's (© Amiga) (*checked* by the review).
  MicroKnight, P0T-NOoDLE and mO'sOul need their original authors' terms first.

### 4.3 Seven console rules for boards

A second new `SYS_console` op, **`CON_RULES`** (4.5). Each rule is a few instructions in `term.c`, tested only where
the console already branches (bytes below 32, the last column, LF, SGR, `J`), so plain consoles pay nothing
measurable. **The colour rules act when a cell is stored**: the colour byte written into the grid (and so into the
scrollback, which keeps only colours) already holds the bright foreground or background, so redraws, the scrollback
and console switches stay right, and dropping a rule changes no cell already drawn.

| Bit | Rule | Why (*read*: knowledge base, SyncTERM's source where marked) |
|---|---|---|
| `CONR_PCCTRL` 1 | bytes 0x01-0x1F that are not BEL, BS, HT, LF, CR, ESC (and CAN/SUB inside a sequence) and 0x7F are stored and drawn as glyphs; NUL is ignored | CP437 art draws with ☺ ♥ ► ◄ ↑ ↓ ♫ ⌂ (PC page rule, `ansi-art-fonts-and-parsing.md`) |
| `CONR_WRAPNOW` 2 | writing the last column wraps at once | ANSI.SYS and the Amiga console wrap at once; art made for them has no line end after a full row and may move the cursor right after it |
| `CONR_CLRHOME` 4 | `ESC[2J` also homes the cursor | ANSI.SYS and BBS terminals do; xterm does not; art that starts with `ESC[2J` alone lands wherever the cursor was (`screens-variants-utf8-wide.md`) |
| `CONR_BRIGHT` 8 | SGR 1 sets a per-console "bright" flag: foregrounds are stored as colour + 8 (so `ESC[1m ESC[31m` is bright red) and are not drawn bold; 22 and 0 clear it | ANSI-BBS's "bold" is the high-intensity colour (ESC[1;30m is the dark grey of shading); bold smear spoils block art. **This reverses D14's reviewed answer 2 ("bold stays bold") for consoles that ask for it only.** At 3 planes a foreground of 8 or more is still drawn as the colour plus bold (`gfx.c` ~314): the planes for more are not there |
| `CONR_ICE` 16 | SGR 5 and 6 set the background bright (stored as colour + 8), SGR 25 clears it, instead of being ignored | iCE art; off by default (SyncTERM's default is blink, which the console cannot show: ignored as today). On by `-i` or the board's `CSI ? 33 h` ("blink to bright intensity background"), off by `CSI ? 33 l` (*checked*, cterm.adoc 982-986, cterm_ecma48.c) |
| `CONR_CGAPAL` 32 | the console uses the CGA/VGA palette while it is shown: in SGR order 0x000 0xA00 0x0A0 **0xA50** (brown) 0x00A 0xA0A 0x0AA 0xAAA, bright 0x555 0xF55 0x5F5 0xFF5 0x55F 0xF5F 0x5FF 0xFFF | art is drawn and checked in that palette (PabloDraw, Moebius, SyncTERM); Zenix's own palette was chosen for Topaz text. `build_cop` takes the shown console's palette; `con_switch` and `gfx_set_colours` rewrite the Copper's colour moves (in the vertical blank, as the pointers). At 2 planes (four fixed colours) it has no effect |
| `CONR_LFONLY` 64 | LF moves down one row in the same column (no CR) | xterm's terminfo (`cud1=^J`), ANSI.SYS and SyncTERM ("same column of the next row", cterm.adoc) move down only; full-screen programs on a Unix host would draw wrongly with Zenix's LF = CR LF. The Amiga mode keeps CR LF (the Amiga console's newline) |

**What the depth means for boards** (said by telnet once at connect when it is less than 16): **16 colours** show
ANSI art as drawn. **8 colours** (the stock A1200's default): a bright foreground is shown as the colour plus bold,
bright black is grey, iCE backgrounds are the plain colour: art loses its shading. **4 and 2 colours** fold the
colours by warmth (D14 section 11): readable menus, not art. Owner question 2: `colours 16` on the stock A1200 costs
40 KB more chip RAM (160 KB of screen instead of 120).

### 4.4 The filter in telnet (stream rewrites only, no screen model)

A small state machine that tracks only whether it is inside an escape sequence or a music string; it never models
the screen:

- **ANSI music, swallowed as SyncTERM does** (*checked*, cterm.adoc 676-689 and 3260-3299, `cterm_accumulate_music`):
  the default is SyncTERM's "BANSI" (`cterm.c` 1865, `bbslist.c` 1008): `CSI |` (SyncTERM's own) and `CSI N`
  (BananaCom's) start a music string; `CSI M` without parameters is music only in the "all" mode, which the board
  chooses with `CSI = 2 M` (`CSI = 0 M` none but `CSI |`, `CSI = 1 M` BANSI), and is ECMA-48's delete line otherwise.
  A music string ends at SO (0x0E) or at the first byte outside SyncTERM's music set (`aAbBcCdDeEfFgGlLmMnNoOpPsStT`,
  digits, `. - + # < >` and space), which is then handled as ordinary output; at most 1,024 bytes (SyncTERM's
  `musicbuf`). So a stray `CSI N` eats no text. Played on Paula later (roadmap block 6), not now.
- **Form feed** (0x0C, the BBS clear screen; SyncTERM clears and homes, `cterm_ecma48.c` 1036) -> `ESC[2J ESC[H` in
  both BBS modes. **SUB** (0x1A, DOS's end of file) is dropped in both BBS modes (a SAUCE record that slips through then
  shows as a line of text, as on other terminals; boards send screens without SAUCE).
- **iCE:** `CSI ? 33 h/l` switch `CONR_ICE` (4.3).
- **SyncTERM's font switch** `CSI Ps1 ; Ps2 SP D` (defaults 0;0, so `CSI SP D` alone is CP437; *checked*, cterm.adoc
  406-570): only Ps1 = 0 (the main font) is acted on, the bright and blink slots 1-3 are swallowed. Ps2 = 0 switches
  the whole PC mode (font, rules, keys); 37-42 (P0T-NOoDLE, mO'sOul, MicroKnight Plus, Topaz Plus, MicroKnight, Topaz)
  switch the whole Amiga mode, shown in the ROM's Topaz; other numbers (36 Atari, 43 Prestel, 44 Atari ST ...) are
  swallowed.
- **Amiga mode:** the 8-bit CSI 0x9B becomes `ESC [`; the Amiga console's own sequences that would mean something else
  on the console are rewritten, answered or dropped: `CSI n u` with a number is the Amiga's line length, not "restore
  the cursor": dropped; `CSI 0 SP p` / `CSI SP p` are the Amiga's cursor off/on: rewritten to `CSI ? 25 l/h`; `CSI 0
  SP q` (the window bounds request Amiga programs use for the size) is answered by telnet to the board as `CSI 1;1;R;C
  SP r` with the rows and columns of 3.2 (CTerm does, `vt.c` ~1189); `CSI n t/x/y` draw nothing and the console
  already swallows them. *Checked*: ROM Kernel Reference Manual: Devices (console.device), CTerm `vt.c`.
- **Device attributes (`ESC[c`)** are not answered (neither does the console today), so boards that probe for
  SyncTERM (`CSI = 67;84;101;114;109;... c`) never send its extensions; boards detect ANSI with `ESC[6n`, which the
  console answers. Whether Synchronet, Mystic or ENiGMA wait long for a DA answer is checked against the simulator and
  a real board (the reviews believe they time out quietly).
- **Unix mode:** UTF-8 decoded to Latin-1 (`?` for the rest), nothing else.

### 4.5 The interface (claimed in CLAUDE.md and frozen in the build step)

`SYS_console` (89) keeps `CON_COLOURS` 1 exactly as it is. The new ops name the console by a **file descriptor**, as
Linux's `KDFONTOP` and `PIO_CMAP` do (not by the caller's `ctty`, which would change the shell's screen for `telnet
host > log` or a background `telnet &`): the op word is `op | (fd + 1) << 8`, where 0 in the upper bits (every
caller so far) means "no descriptor", which only `CON_COLOURS` accepts.

- **The target:** the descriptor must be a console (`FT_TTY` with a console number; anything else, a pty later
  included, -ENOTTY) that is the caller's controlling tty, or the caller is root; and the caller must be in that
  console's foreground process group (-EPERM otherwise: a background job never changes the screen). Without a display:
  -ENODEV.
- **`CON_FONT` 2** (a2 a pointer to PSF1 data, a3 its size; a null pointer drops the font): checks of 4.2 (-EFAULT,
  -EINVAL), -ENOMEM when the memory is not there (nothing changes); answers 0.
- **`CON_RULES` 3** (a2 the new bits, a3 the mask of bits to change; a3 = 0 only asks): answers the bits in force before
  the call; unknown bits -EINVAL, so later bits keep their meaning. Owned and dropped like the font (its last setter's
  end).
- Frozen once merged: the op numbers, the fd-in-the-op-word rule, the arguments, the `CONR_*` bits 1-64 and their
  meaning, the errors above, CON_FONT's "8-bit mode" meaning (4.2), and the font file's format (PSF1) and path.

### 4.6 Amiga boards (AmiExpress and friends)

AmiExpress asks "ANSI, RIP or No graphics" and sends ANSI in Latin-1 for Topaz, CR LF line ends over telnet, `~`
codes already expanded on the board's side (*read*, `amiga/amiexpress-caller-strings.md`); its pause prompt erases
itself with `ESC[1A ESC[K`; lines per screen from NAWS. Everything it sends is ECMA-48 plus the few Amiga console codes
of 4.4, drawn by the console's own Topaz: the Amiga mode needs no font load. Its transfer protocols are Zmodem (the
common one), Xmodem, Ymodem and Hydra (section 5).

## 5. Downloads and uploads

- **Zmodem first** (the protocol Amiga and PC boards use most), in **two small programs, `rz` and `sz`** (one file,
  two names, as `ifup` is `ifconfig`), reading and writing their standard input and output as lrzsz's do. So the same
  programs serve telnet now, `cu` later, and a telnet or ssh session *into* Zenix (`sz file` sends from Zenix). Loaded
  only while a transfer runs (golden path).
- **The engine: CTerm's `zmodem.c`** (the owner's own code, MIT, 1,010 lines with its `xfer.h` table of I/O calls,
  verified both ways against lrzsz 0.12.20 with 0 B / 3,000 B / all byte values / 1 MB, `-8`, `-e`, and a line that
  drops and garbles bytes; *read*, `amiga/file-transfers-zmodem-ymodem-xmodem.md`), so it builds for the PC test and
  for Zenix unchanged. The file has no licence header of its own (only the repository's MIT LICENSE): the copy gets
  `SPDX-License-Identifier: MIT` and the notice (CLAUDE.md's licence rule). **Name cleaning, `.part`, resume and the
  no-overwrite rule live in Zenix's side of the I/O table** (`rz`'s own file), not in the engine. lrzsz itself (GPL-2+,
  1998) stays the test peer.
- **Start:** telnet looks for `** ZDLE B 0 0` (ZRQINIT: the board sends, start `rz`) and `** ZDLE B 0 1` (ZRINIT: the
  board waits, ask "send which files?" and start `sz`) in the raw bytes before the telnet decoder, as CTerm does (the
  header has no 0xFF, so escaping cannot hide it). The matched header **and every byte after it** go to the program's
  standard input, none to the screen (else `sz` would wait for the board to repeat ZRINIT, about 10 s). telnet spawns
  the program with two pipes and relays socket <-> pipes through the decoder and encoder, shows a one-line progress on
  the bottom row from the program's standard error, and hands the bytes after the session back to the screen (the
  board's prompt that came with the `OO`).
- **BINARY:** asked at connect. **If the board refused it**, `rz -e` / `sz -e` are started (ESCCTL: every control
  character escaped, so no raw CR is ever followed by a file byte 0x00 that `CR NUL` folding would eat; the knowledge
  base's own test caught that: `go/zmodem-and-telnet.md`, "Telnet binary mode").
- **Cancel:** Ctrl-X five times from the keyboard, or the escape menu, sends ten CANs and ten BSs (Zmodem's give-up)
  and ends the program; five CANs from the board end it too.
- **Where files go:** the directory telnet was started in (`rz`'s Unix rule; `-d DIR` to choose), said in the banner.
  Names are cleaned (no `/`, no `..`, **no leading `.`**, so a board can never push `.profile` into a home directory,
  no control characters, at most 255 bytes); **a file is written as `NAME.part` and renamed when whole**; an existing
  `NAME` is never overwritten: the same size is skipped (ZSKIP, said), another size is saved as `NAME.1`, `.2` ...
  **Resume only when it is the same file:** a `NAME.part` shorter than the offered file is resumed only after ZCRC
  (the receiver sends its length, the sender the CRC-32 of that many bytes; equal: `ZRPOS` at that length), as BBX
  does, verified against `lsz -r` (`go/zmodem-and-telnet.md`); a sender that does not answer ZCRC, or a different CRC,
  gets a fresh download under a new name. (CTerm's blind `ZRPOS` at the part's length would splice two files.) A full
  disk ends the transfer with the reason, keeping the part.
- **The Zmodem traps** in the knowledge base are the engine's tests (`go/zmodem-and-telnet.md`): CR LF after a hex
  header, ZDLE escapes and ZRUB0/1, ESCCTL including CR, nothing after ZSKIP, five CANs, ZFIN with a file open, the
  OO after ZFIN.
- **Xmodem and Ymodem later** (they cannot be detected: the board waits silently; the escape menu's `d`), **Hydra
  later** (AmiExpress's bidirectional protocol; CTerm carries HydraCom's code). Owner question 5.

## 6. Size, memory and speed

| Part | Size (*estimates*, -Os) |
|---|---|
| Kernel: `CON_FONT`, `CON_RULES`, the shown-font pointer, a loaded font's table at a depth change, the per-console palette in the Copper list, the seven rules, release at exit, `gfx_panic`'s reset | 1.5-2.5 KB code (the architecture review's figure once the `gfx.c` work is counted), ~20 bytes per console |
| `telnet` (protocol, filter, keys, escape menu, the fallback table, libz and its socket calls) | 16-22 KB (today's *measured* `.zx` sizes: ifconfig 13,948, ping 14,160, host 11,888, cu 8,432, less 24,036 bytes; libz is still linked into every program) |
| `rz`/`sz` (one program) | 14-18 KB, plus the engine's buffers while a transfer runs (a few KB *read* in `zmodem.c`; CTerm's note says 15 KB of state with its paths) |
| `cp437-8x8.psf` | 2,052 bytes on disk; 2 KB + 8 KB (2-4 planes) while used |

- **The RAM floppy:** the kernel has 26,728 bytes left on `zenix-ram.adf` but **2,152 on the mount tests' floppy**
  (D19 section 14): this step's kernel growth takes 8 KB from `RAMDISK_MOUNT_K` first (23 free blocks), as D19 says.
  telnet, `rz`/`sz` and the font are on the full root images (`mkroot.sh` copies `rootfs/` whole), not on the RAM
  floppy, whose `RAMAPPS` are full. The demo floppy (block 2's end) needs about 35-45 KB for them: counted in its step.
- **Memory on the stock A1200** (2 MB chip, SLIP, 8 colours): 1,159 KB free with SLIP up (D19, *measured* in FS-UAE),
  minus telnet (~25-30 KB with its buffers) and, on a PC board, the font (10 KB): about **1,120 KB free while
  calling a board**; 40 KB less at 16 colours. The PiStorm (fast RAM, Ethernet at step 9) has room to spare.
- **Speed: drawing is the limit, not the network.** About 7 KB/s of text at one plane and 10-15% less at 3-4 planes
  (*estimates* from FS-UAE and the PiStorm, D14), under SLIP's 11.5 KB/s, so a 6 KB ANSI screen takes about a second
  on the stock A1200. **To be measured on the real stock A1200 and the 060**, never Emu68.
- **SLIP and long writes (a trap found in design, sharpened by three reviews):** the kernel draws a write without
  letting another process run, and SLIP's 1 KB receive ring lasts 89 ms. netd and telnet are both at nice 0, so a
  woken netd waits for telnet's slice to end (50 ms) plus the write piece then running: about 85-90 ms, the whole
  ring; D19's step 5 already lost 0.07% with no drawing at all. A piece of 256 bytes is not a time bound either
  (`ESC[2J` and region scrolls are blitter work). So, in this order, each measured on the stock A1200 before the next:
  1. **telnet writes the screen at most 256 bytes per call and runs itself at nice 1** (any user may lower their own
     priority, D10): netd, woken by serial bytes, is then strictly better and takes the CPU at the next return to user
     mode, at most one piece later (about 40 ms). netd's frozen nice 0 is not touched.
  2. **If losses remain:** a kernel yield point in `tty_write` between pieces when a strictly better process is ready
     (the terminal is consistent there, as `kwork` already relies on), which also helps any large console write
     (`cat`, `ls -l`) starving netd: the main session's change, measured.
  3. A larger receive ring (D19 step 5 choice 11) stays the fallback.
  The losses are counted (lost bytes, `ifconfig`'s counters) in the A1200 test.

## 7. Pseudo-terminals, SIGHUP and the telnet server

### 7.1 What needs them

**The client does not:** it uses the console's tty as any full-screen program does. A **telnet server** on Zenix
(logging in to Zenix over the network), later an **ssh server**, `script`, a terminal multiplexer and a ported
`expect` do: a program that is a terminal for another program. The remote control the roadmap plans can run
commands over pipes; an interactive shell over it needs a pty too.

**Choice: the client now; ptys, session leaders, SIGHUP, SIGWINCH and `telnetd` later, as one step of their own**
(owner question 1), after the client is in daily use, with this section as its design and two reviews of its own then
(a deep core change: the tty layer). The client step builds nothing that step must change: its console ops refuse
anything but a console (4.5), and telnet's fallback (3.5) covers running on a pty.

### 7.2 Design (for the later step)

- **Names, as Linux and POSIX:** `/dev/ptmx` opens the master of a free pty; the slave is `/dev/pts/N`, both in
  `open_device` like `/dev/ttyN` (no devpts filesystem). `posix_openpt`, `grantpt` (nothing to do), `unlockpt` (nothing),
  `ptsname` and `openpty` are libz. BSD's `ptyp0`/`ttyp0` pairs are not taken.
- **Inode numbers, never a console's:** a slave's `st_ino` is its tty index (`NCONS + 1 + N`, 11 and up), a master's
  `0x100 + N`; `ptsname` reads N from the master's. Never 1..`NCONS`, so `login`'s autologin test (a character device
  with `st_ino` 1, `user/login.c` 48) can never match a remote login. Frozen with the step.
- **Ownership:** anyone may open `/dev/ptmx`. A pty records an **owner uid**, first the master's opener; `/dev/pts/N`
  opens only for root and that uid. `login`, on success, sets it to the user (root's `SYS_ttyctl` op, the role of
  Linux's `chown` of the pty), so the user's own programs can open their terminal again (`/dev/tty`, `sudo`'s password
  prompt).
- **One tty layer:** the line discipline (`tty.c`: cooked lines, echo, ^C/^Z to the foreground group, raw, nosig) is
  shared; what differs is where a tty's output goes (a console: `term_write`; a pty: a ring the master reads) and where
  its input comes from (`kbd_work` into the shown console; the master's writes). `struct tty` gains an output ring, a
  kind, an owner uid, a size and a session leader; ttys 1-10 stay the consoles, ptys take 11 and up (`NPTY` 8,
  allocated at open: ~3.5 KB each with the two rings), and `ctty` (a byte) names either. **Every place that assumes a
  tty is a console changes in that step** (the reviews' list): the clamp in `proc.c` ~642 and its use of `fd->off` ~627,
  `tty.c` `get()` (which maps an unknown number to console 1) and `tty_of`, `tty_input` (which uses `con_cur`),
  `tty_write` -> `term_write` (`wsp[NCONS + 1]` would overflow), `con_in_use`, `vfs.c` ~495's `con != tty_of(current)`.
- **Numbers are not reused too early:** a pty's number is free again only when its master and every slave descriptor
  are closed and no process names it as `ctty` (a lingering process of a hung-up session must never read the next
  login's input or get its signals).
- **Controlling terminal and session leader:** the spawn rule stays ("the tty of fd 0 at spawn, else the parent's"),
  so `telnetd` spawning `login` with the slave as fd 0-2 gives the session its terminal. A tty records its **session
  leader**: the first process spawned onto it from another tty (`login`), and **after D18's step-aside the shell login
  hands to** (`exit`'s second argument moves the role with the child). `setsid` in libz makes the caller its own group
  (D16) and, when its fd 0 is a tty nobody leads, that tty's leader: enough for the ported servers that call it; a
  full POSIX session id is not added (D16's "process groups only").
- **Hang-up:** the master's last close hangs the slave up: SIGHUP then SIGCONT to **every process group whose
  controlling tty it is** (POSIX's carrier rule plus a cut-down orphaned-group rule, so a stopped job never waits for
  ever with SIGHUP pending); reads on the slave give 0 (end of file), writes -EIO. The shell, on SIGHUP, sends SIGHUP
  and SIGCONT to its jobs and ends (bash's rule). **The master side:** when no slave is open, a master read gives -EIO
  and `poll` reports POLLHUP (Linux), which is how `telnetd` learns the session ended. Consoles never hang up (no
  carrier); `cu` and the serial port still have no carrier detect.
- **Window size, SIGWINCH:** each tty gets a size (a console's is the screen's); a master sets its slave's size (from
  NAWS in `telnetd`), which sends SIGWINCH to the slave's foreground group; `SYS_ttyinfo` keeps its frozen meaning
  ("this process's tty's size") and answers a pty's. Setting needs one new call: `SYS_ttyctl (fd, op, arg)` with
  TIOCSWINSZ/TIOCGWINSZ, the owner op and TIOCSCTTY's subset, its number claimed then; libz's `ioctl` maps the Linux
  names.
- **`telnetd`:** the server side of section 3's protocol (WILL ECHO, WILL SGA, DO TTYPE, DO NAWS; checked by the
  review), one process per connection spawning `login` on a pty and relaying; off by default; root's `/etc/` file to
  turn it on. How a TCP listener is started at boot is not answered by D11 (its services start on a message to a port
  name, not on a connection): decided in that step (an init-started daemon as `ifup -a` is, or inetd's model).
  Unencrypted: for the home network until ssh; said in its page.
- **Cost (later, *estimate*):** 2-3 KB of kernel code, ~3.5 KB per open pty, `telnetd` ~12 KB per connection plus the
  session.

## 8. Tests

### 8.1 On the PC

- **A BBS simulator** (`tools/bbs-sim.py`, beside `slip-host.py`; no root, in the user and network namespace
  `slip-host.py` and `eth-test.sh` already use): a telnet server that negotiates (and records what the client asked and
  answered: BINARY per direction, SGA, ECHO, TTYPE's name, NAWS's size and 255 doubling, no loops, no negotiation
  before its own IAC on a non-23 port), then on command sends screens: every CP437 byte 1-255 in a grid, colours with
  and without SGR 1 (`ESC[1m ESC[31m` too), iCE blink with `CSI ? 33 h`, `ESC[2J` without `ESC[H`, a full 80-column row
  followed by a cursor move, LF without CR, cursor codes and `ESC[6n` (its answer checked), music strings (BANSI,
  `CSI = 2 M`, with and without SO, one broken by a text byte, an unterminated one), FF, SUB, SyncTERM's font switch
  (`CSI SP D`, 0;0, 0;42, 1;0), an Amiga screen (Latin-1, 0x9B CSI, `CSI n u`, `CSI 0 SP q` and the answer), 0xFF
  doubled inside text, and **a Zmodem upload by the real `lsz`** (and a download to `lrz`), with BINARY and without
  (`-e`), a resume after ZCRC and a refused splice, files compared by checksum. **The test art is made for the test**
  (generated byte grids and a small hand-drawn screen in the repository); real art packs are copyrighted and stay out
  of the public repository (the owner's own `~/Amiga/BBS` files may be used locally, never committed).
- **telnet's core on the host:** the decoder, encoder, Q-method negotiation, the filter, the key translation and the
  CP437 fallback table as plain C without system calls (as netd's core), built for Linux and run against the simulator;
  **fuzzed** (IAC and SB sequences, music strings, 0x9B) one run at a time with a memory cap (this machine's rule).
- **The Zmodem engine** with CTerm's own PC tests against lrzsz (both ways, the sizes and the noisy line), and the
  I/O table's rules (names, `.part`, no overwrite, leading dots, resume by ZCRC).
- **The console rules and the PC font** in `tools/term-test.sh` (the host model of `term.c`): each rule's stored cells
  (the bright flag, iCE, LF only), the font's glyph index 0-255 and `?` for stored controls after a drop, the palette
  at a console switch and a depth change, the release when the owner ends and not when a former owner ends, the fd
  checks and errors of 4.5 (a file, a background caller, another user's console), `gfx_panic`'s reset; the existing fuzz
  then with every rule bit set.

### 8.2 In FS-UAE

- **Over the A2065** (D19 step 8: the serial port stays free, so the test rig types keys and reads the console's copy
  on the serial line as today): `telnet 10.0.2.2 PORT` to the simulator through slirp; the negotiation record; key
  translation (Swedish letters typed on the emulated keyboard arrive as CP437 or Latin-1 or UTF-8 by mode; Backspace
  as BS; Ctrl-C reaches the board; Ctrl+Alt+0x1B opens the escape prompt); the escape menu (`q`, `z` then `fg`); the
  connection closed by the board; `telnet host > file` (the fallback, the console untouched); a download and an
  upload compared byte for byte, `.part`, the no-overwrite rule, a cancel, a resume.
- **Screenshots** (`tools/shot.py`, the cage rig) at 1, 3 and 4 planes: the CP437 grid compared with the font's own
  bitmaps, the CGA palette, iCE, bright, wrap-now; a console switch away and back while a PC board is shown (the other
  console in Topaz and Zenix's palette); root's `CON_COLOURS` switch while a PC board is shown; after telnet ends, the
  console in Topaz and cleared.
- **Over SLIP** (scripted, as `slip-test.sh`: the serial port is the network's): a 32 KB screen through telnet on the
  stock A1200 machine, the bytes lost on the ring counted with and without telnet's nice 1 (emulator figures only).
- **Memory** with `free` and `ps` while a board is shown (the matrix's 2 MB and 1 MB machines).

### 8.3 On the real machines (the owner's)

The stock A1200 over SLIP and the PiStorm over GENET (after D19 step 9): the owner's own boards (an Amiga board, a
PC board), screens and a Zmodem download each; the drawing speed, the SLIP losses while a screen draws (section 6's
three steps), the time for a 6 KB screen, at 8 and 16 colours, labelled by machine.

## 9. Build order

Each step small, merged with its tests and the full suite. Steps that touch `kernel/kernel.h`, `syscall.c`,
`term.c`, `gfx.c`, the `Makefile` (`UAPPS`, images), `user/usys.h`, `libz` or `docs/source-tree.md` are **the main
session's** (CLAUDE.md "Sessions"); a side session may take only the host-only parts marked *side*, with this note's
names and formats.

0. *side:* `tools/bbs-sim.py` with its own self-test; the PSF1 font built from Linux's `font_8x8.c` by a host tool
   (`tools/mkpsf.py`, the C source beside it as `tools/font_8x8.c` with its licence line); telnet's core (`net/telnet/`:
   decoder, encoder, options, filter, keys, fallback table; plain C, a network protocol beside netd's core) with host
   tests and the fuzz harness; the Zmodem engine (`user/xfer/`: CTerm's `zmodem.c` and `xfer.h` with an MIT SPDX line;
   it serves `cu` and the serial line too, so not under `net/`) with its host tests against lrzsz. The main session adds
   the new directories to `docs/source-tree.md` when it merges.
1. *main:* the console: `CON_FONT` and `CON_RULES` (numbers, the op-word rule and bits claimed in CLAUDE.md first;
   frozen once merged), the shown-font pointer, the rules, the palette, their release at exit, `gfx_panic`'s reset,
   `termtest` pages for the PC font and the rules; `term-test.sh` and the screenshot tests. The mount tests' RAM disk
   shrunk first.
2. *main:* `telnet` (`user/telnet.c` over `net/telnet/`) in the Unix mode: connect, options, NAWS, TTYPE, the escape
   key and menu, close, `LFONLY`; its page, `commands.md`, `features.md`; tests against the simulator over the A2065
   and over SLIP.
3. *main:* the BBS modes: the filter, the PC font load, the rules, key translation, the fallback, the 256-byte writes
   and nice 1; screenshots.
4. *main:* `rz`/`sz` (`user/rz.c` over `user/xfer/`) and telnet's autostart, relay and progress line; the transfer
   tests.
5. *main, if the owner wants it (question 4):* the phone book.
6. *owner:* the real machines (8.3); then section 6's step 2 or 3 if the losses ask for it.
7. *later, its own step and two reviews:* ptys, session leaders, SIGHUP, SIGWINCH, `SYS_ttyctl`, `telnetd` (section 7).

## 10. Frozen once built (CLAUDE.md's list gets them in the build step)

`SYS_console`'s ops `CON_FONT` 2 and `CON_RULES` 3 as section 4.5 states them (the fd in the op word, the arguments,
the target and permission rules, the errors, the `CONR_*` bits 1-64, CON_FONT's 8-bit-mode meaning), the font file's
format (PSF1) and path; `rz`/`sz`'s standard input and output contract (lrzsz's). telnet's command-line options are
listed in its page, not frozen. Later, with the pty step: `/dev/ptmx` and `/dev/pts/N`, the slave and master inode
ranges, `SYS_ttyctl`'s ops (Linux's TIOC names and values) and the SIGHUP and SIGWINCH rules. **Nothing here changes
an existing frozen meaning** (both architecture reviews checked): `SYS_ttyinfo` keeps "this process's tty's size",
the consoles' `st_ino` stays their number, `TTY_RAW`/`TTY_NOSIG`, the spawn `ctty` rule, `CON_COLOURS`, autologin and
D16's signal numbers are unchanged.

## 11. Open questions for the owner

1. **Logging in to Zenix from another computer** (a telnet server, then ssh): needs pseudo-terminals, a kernel step
   of its own. *Recommended:* the client first; the server after it is in daily use, home network only, off by
   default.
2. **Colours on the stock A1200:** BBS art needs 16 colours; at the stock A1200's 8 it loses its shading. *Recommended:*
   keep 8 as the default, and telnet says once at connect "8 colours: `colours 16` in /etc/console shows art fully
   (40 KB more chip RAM)".
3. **The window size:** the full 80 x 32 told to the board, or an 80 x 25 window as on a PC? *Recommended:* 80 x 32
   (fast scrolling, scrollback, boards use the rows); a 25-row window later only if a board you use needs it.
4. **A phone book** (`telnet NAME` from a short list: name, address, port, PC or Amiga), like CTerm's and SyncTERM's?
   *Recommended:* yes, a plain text file in your home directory, after the downloads work.
5. **Transfers:** *Recommended:* Zmodem now; Xmodem, Ymodem and Hydra only if a board you use needs them.
6. **The default mode** with no phone book entry: *Recommended:* Amiga board (Topaz), as your boards are; `-p` for a
   PC board.
7. **Other Amiga fonts** (MicroKnight, P0T-NOoDLE, mO'sOul): their only ready source has a licence that does not
   allow shipping them with Zenix. *Recommended:* Topaz (the machine's own) only; ask the fonts' authors later if you
   miss one.

## 12. For the main session (not owner questions)

- **The feature halt:** D19 (approved by the owner) names telnet; `CON_FONT`/`CON_RULES` and the `term.c`/`gfx.c`
  changes are new kernel interfaces in service of it, so the owner's approval of this note is their approval.
- **Review depth:** two independent reviews per focus were made of revision 1 (section 13); the console change is a
  kernel change but not a scheduler, loader or update change. A second look at revision 2 before step 1 is the main
  session's call.
- **The order** of telnet against the approved serial terminal for the owner's own goal is the roadmap's, unchanged.

## 13. Reviews

Revision 1 (5dea7e3) had four independent read-only reviews, two per focus: protocol and BBS reality (one of them
checked the four facts written from memory against SyncTERM's source, git.kernel.org, `kbd.c` and the TrueSchool
repository), and architecture, frozen interfaces, size and the tty layer. Every finding is worked in above.

**Protocol and BBS reality.** *Blocker (one review; should fix in the other)* - section 6: 256-byte writes do not
bound netd's delay (equal nice levels switch only at the 50 ms slice end, plus a write piece: ~90 ms against an
89 ms ring; a piece is not a time bound): telnet at nice 1, then a kernel yield point, then a larger ring (6).
*Should fix* - LF is always CR LF on the console, where xterm, ANSI.SYS and SyncTERM move down only (`CONR_LFONLY`);
no local echo in the BBS modes; BINARY per direction; without BINARY, transfers start with `-e`; the autostart header
and the bytes after it go to the program; resuming a `.part` blindly splices two files (ZCRC first); answer the Amiga
`CSI 0 SP q`; the TrueSchool licence is not simply "GPL with a font exception" (GPL-FE with no version against CC
BY-NC-SA 3.0 in the files; its Topaz is the ROM's); SyncTERM's default music mode is BANSI (`CSI |` and `CSI N`), a
music string ends at the first byte outside the music set (not after 512 bytes); pty numbers must not be reused while
named, the slave's inode must not be 1, the master's read at hang-up, the pty owner set by `login`, SIGHUP and SIGCONT
to every group on the pty and the shell's jobs. *Minor* - SGR 6 and 25 and `CSI ? 33 l` with iCE; the font switch's
defaults, slots 1-3 and switching the whole mode; SUB dropped; NAWS big-endian with 255 doubled; no negotiation before
the server's IAC on other ports; TERMINAL-TYPE `ANSI` for Amiga boards a deliberate difference from CTerm; the CGA
palette in SGR order; `ESC[?7l` ignored by the console; `font_8x8.c` came from a DOS code-page file; Ctrl-X cancel
sends ten CANs and BSs, ZSKIP for an identical file; Help is Insert to boards.

**Facts checked against sources.** SyncTERM `CSI ? 33 h` is iCE (cterm.adoc 982-986); music forms, terminator SO,
default BANSI, `CSI = n M` (cterm.adoc 676-689, 3260-3299; cterm.c 1865; bbslist.c 1008); font select `CSI Ps1 ; Ps2
SP D`, defaults 0;0, Amiga fonts 37-42 (cterm.adoc 406-570). Linux `font_8x8.c`: `// SPDX-License-Identifier:
GPL-2.0`, 256 glyphs in CP437 order. Ctrl-]: Ctrl+Alt+raw key 0x1B gives 0x1D on the `sv` keymap (`kbd.c` 76-97,
180-199; read, not run). TrueSchool fonts: "GPL-FE" without a version, files' metadata CC BY-NC-SA 3.0 (4.2).

**Architecture, frozen interfaces, size, the tty layer.** *Blocker (one review; should fix in the other)* - the ops
targeted the caller's `ctty` (wrong for `telnet > log`, a background job, root on another console, and a pty later)
and their arguments were too vague to freeze: a descriptor in the op word, console only (-ENOTTY), the caller's own or
root's, foreground only, exact arguments, PSF1 checked as untrusted input, `CON_RULES` with a mask that can only ask
(4.5); and telnet's fallback when they are refused (3.5). *Should fix* - one font per console needs a "shown font"
pointer, a 0-255 index, a rebuild at a depth change, the palette in `build_cop` and at a console switch (4.2, 4.3);
"panics keep Topaz" was false (`term_log` writes into the shown console: `gfx_panic` resets font and rules); the font
and the byte grid (redraw in the current font, the screen cleared before a drop, last setter wins); the colour rules
act when a cell is stored, with a bright flag, so the scrollback stays right; UTF-8 in block 3 (CON_FONT frozen as an
8-bit mode, PSF1 mode 0 and height 8 only); the pty and autologin trap and the list of code that assumes a console;
the session leader after D18's step-aside; downloads with a leading dot; name and resume rules in Zenix's side of the
I/O table; `zmodem.c` needs an MIT SPDX line; section 2's facts (SIGHUP already sent to stopped children; Topaz's
table is 7 KB). *Minor* - `CONR_BRIGHT` reverses D14's answer 2 for opted-in consoles, said; the images (not on the
RAM floppy; the demo floppy's 35-45 KB); telnet nearer 16-22 KB; directories (`user/xfer/`, the font source beside its
tool, `tools/bbs-sim.py` beside `slip-host.py`, `source-tree.md`); command options not frozen; items for the main
session (section 12). Both architecture reviews judged approach A sound and found no change to an existing frozen
meaning.

Revision 2 changes no part of the overall shape (one telnet process filtering a stream the console draws, a
per-console font and rules loaded on demand, Zmodem in `rz`/`sz`, ptys later as their own step).

Last checked against the code: 2026-10-27 (main at f03180e).

## Owner decisions (2026-10-27)

"Yes to all" for section 12's questions: (1) the client first, the server later (home network only, off by default);
(2) 8 colours stay the default on the stock A1200, telnet mentions `colours 16` once; (3) the board is told 80x32;
(4) a phone book, after downloads work; (5) Zmodem only, others if a board needs them; (6) Amiga-board mode by default,
`-p` for PC boards; (7) Topaz from the ROM only.
