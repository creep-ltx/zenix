# D20: telnet, the BBS terminal and pseudo-terminals (design note, revision 1)

Status: **design, revision 1, not reviewed, nothing built.** D19 section 10 step 11: "telnet (its own note:
pseudo-terminals, SIGHUP, the BBS terminal)". Roadmap block 2: `telnet` "made for BBSes, the owner's daily use: ANSI
colours, the PC character set and Amiga fonts, the BBS window size, downloads". The short version is DESIGN.md D20.
Facts about today's code are *read* from main at f03180e (2026-10-27); facts about boards and terminals are *read*
from the knowledge base (`bbs/`, `amiga/ansi-art-*`, `amiga/file-transfers-*`, `go/zmodem-and-telnet.md`) and from
CTerm's source (the owner's own AmigaOS terminal, MIT, used on real boards); sizes are *estimates* unless marked.

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
  region scrolls by blitter, the whole-screen scroll a Copper pointer bump, 200 lines of scrollback. Glyphs are the
  ROM's Topaz 8 for bytes 32-255 (Latin-1; `gfx.c glyphs[224][8]`), bytes below 32 are never drawn, wrapping is
  deferred (VT style), `ESC[2J` does not move the cursor, SGR 1 is bold (pixel smear) and never a brighter colour,
  SGR 5 (blink) is ignored, `ESC[6n` answers into the asking console's input. The palette is Zenix's own (`gfx.c
  gfx_palette`: yellow 0xBB2 where the PC has brown, a lighter blue).
- **The depth:** 16 colours on AGA with fast RAM (the PiStorm A1200), **8 on the stock A1200** (2 MB chip RAM, no
  fast RAM; bright foregrounds are the colour plus bold, bright black is grey, bright backgrounds are not shown),
  `/etc/console` chooses 2, 4, 8 or 16 at the next boot (D14 section 11).
- **The keyboard** (`kbd.c`): AmigaOS's Swedish keymap `s` (Latin-1: å ä ö as 0xE5 0xE4 0xF6); in raw mode Backspace
  is 0x7F, Del `ESC[3~`, Help `ESC[2~`, arrows `ESC[A` (`ESC O A` under DECCKM), F1-F10 xterm's. Amiga+F1..F10 switch
  consoles and never reach a program.
- **The tty** (`tty.c`): one per console (`NTTY` = 11, index 0 unused), line discipline in the kernel, `TTY_RAW` and
  `TTY_NOSIG`, the foreground a process group (D16), a process's controlling tty `ctty` is always a console number.
  `tty_write` draws in 256-byte pieces and runs `kwork` between them: drawing 4 KB takes seconds on a 14 MHz 68020
  from chip RAM, and the kernel is never preempted, so **no other process runs while a long write draws**.
- **The network** (D19 steps 0-8): BSD sockets, `poll` over sockets and ttys (D16), DNS through netd, SLIP over
  `/dev/ttyS0` for every Amiga (the serial port is the network's while it runs: no `cu`), the A2065 in FS-UAE, GENET
  for the PiStorm at step 9. netd's small profile (the stock A1200) holds 8 TCP ends.
- **`cu`** (D12): two processes (no `poll` then), `~.` ends it, keys raw and unchanged. No file transfer.
- **Signals** (D16): SIGHUP and SIGWINCH are reserved, never generated; no sessions; controlling terminals are only
  the consoles.

## 3. The telnet client

### 3.1 The protocol (RFC 854, option negotiation RFC 855)

- **One process, one loop:** `poll` on the socket and standard input (D16), no second process as `cu` needs. Bytes
  from the socket go through the telnet decoder (IAC commands out, `IAC IAC` -> 0xFF, `CR NUL` -> CR outside binary
  mode), then the terminal filter (section 4.4), then to standard output. Keys go through the key translation (3.3),
  then the encoder (0xFF doubled, CR -> `CR NUL` outside binary mode) to the socket.
- **Options, with RFC 1143's "Q method"** so a misbehaving server can never make the two sides loop (CTerm does the
  same, *read* `net.c`):

| Option | Ours (WILL) | Theirs (DO) | Why |
|---|---|---|---|
| BINARY 0 (RFC 856) | asked | asked | 8-bit characters and file transfers without `CR NUL` folding; refused is fine (the decoder folds) |
| ECHO 1 (RFC 857) | refused | accepted | the board echoes; if it will not, telnet echoes locally (NVT default) |
| SGA 3 (RFC 858) | accepted | asked | character at a time, no GA |
| TERMINAL-TYPE 24 (RFC 1091) | accepted | - | `ANSI` for the BBS modes (what CTerm sends to PC boards), `XTERM-16COLOR` in Unix mode (D14's `TERM`); a second SEND gets the same name |
| NAWS 31 (RFC 1073) | asked | - | the window size, at once when agreed (3.2) |
| everything else (LINEMODE 34, NEW-ENVIRON 39, TSPEED, LFLOW, STATUS, TIMING-MARK ...) | refused (WONT) | refused (DONT) | not needed for a board or a Unix login |

- **Subnegotiation** is collected into a 64-byte buffer (TTYPE SEND and NAWS are a few bytes); a longer one is dropped
  whole, never written past. IAC commands other than SB/SE and the four verbs (NOP, GA, AYT, BRK ...) are ignored.
- **A port that does not speak telnet:** every board's telnet port does; `telnet -r` (raw) turns the decoder and
  encoder off for a plain TCP service (as `nc`). Without `-r`, telnet always decodes IAC and sends its own WILL/DO
  requests at once (CTerm does too).
- **Connect and disconnect:** `telnet HOST [PORT]` (port 23 by default; a name through `getaddrinfo`, D19's
  `SOCK_RESOLVE`). Until connected the tty is in its normal mode, so ^C ends a slow lookup or connect (both wait
  interruptibly, D19 4.3). Connected: "Connected to HOST. Escape key is Ctrl-]." and the tty goes `TTY_RAW |
  TTY_NOSIG`. The other side closing ends telnet with "Connection closed by HOST." and the tty and console restored.
  Exit status 0 after a normal close, 1 when the connection failed, 2 for wrong arguments.

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
| Return (CR) | CR (CR NUL outside binary) | the same | the same |
| Arrows, F1-F10, Help | as given (`ESC[A` ...) | as given | as given |
| å ä ö Å Ä Ö and other Latin-1 | CP437 (0x86 0x84 0x94 0x8F 0x8E 0x99; no CP437 form: not sent, the bell) | as given (Latin-1) | UTF-8 (two bytes) |
| Ctrl-C, Ctrl-Z, every other control | as given (to the board) | as given | as given |
| Ctrl-] | the escape key (3.4) | | |

The Swedish keymap's dead keys are already spacing characters (`kbd.c`), so nothing composes. Characters only the Alt
layer gives (`[ ] { } \ | @`) arrive as ASCII and need nothing.

### 3.4 The escape key

**Ctrl-]** (BSD and Linux telnet's; `-e CHAR` changes it, `-e none` turns it off). On the Swedish keymap `]` is on
the Alt layer, so it is Ctrl with Alt and that key: the keyboard test must check that `kbd.c`'s "Ctrl and a character
with bit 6" rule gives 0x1D there (*read*, not tried). It opens a one-line local prompt at the bottom row, `telnet>`:
`q` close and quit, `z` suspend (the console restored, then SIGTSTP to itself: the shell's job control; `fg` sets it
all up again), `d` receive files now (for a board that starts Zmodem without its header, and later X/Ymodem), `u
FILE...` send files, `p`/`a`/`x` switch to the PC, Amiga or Unix mode, Ctrl-] sends the escape key itself, Return
goes back. `cu`'s `~.` is not taken: one way out, the one every telnet has, and a board's hotkeys never meet a line
start rule.

### 3.5 The three modes

| | **PC board** (`-p`) | **Amiga board** (`-a`, the default) | **Unix** (`-x`) |
|---|---|---|---|
| Characters shown | CP437 through the console's PC font (4.2) | Latin-1, the console's own Topaz | Latin-1; UTF-8 from the host is decoded and shown as Latin-1, anything else as `?` (until roadmap block 3's UTF-8 console) |
| Console rules (4.3) | all six | all but the PC's control-character glyphs | none |
| TERMINAL-TYPE | `ANSI` | `ANSI` | `XTERM-16COLOR` |
| Filter (4.4) | music, FF | music, FF, 0x9B as CSI, the Amiga console's own sequences | none |

**The default is the Amiga board** (the owner's daily use, and the console's native look); the board can switch to the
PC font itself with SyncTERM's font code (4.4), and a phone book remembers each board's mode (section 8, owner
question 4). No guessing from the bytes in revision 1: Charcoal's learned weights tell Amiga from PC art 98% of the
time on whole files (*read*, `amiga/ansi-art-real-world-traps.md`), but a live stream switching fonts mid-screen on a
wrong guess redraws everything wrongly; a later revision may add it as an option once measured on real boards.

## 4. The BBS terminal

### 4.1 Where the screen work happens

| | **A. The console draws; telnet filters the stream** | B. telnet is a full terminal emulator | C. A display server |
|---|---|---|---|
| CPU per byte | one parse (the console's, -O2) plus a light filter | two parses: telnet's screen model, then the console's | not built (D4, roadmap block 5) |
| Glyphs (CP437) | the console must have them anyway | the same: a user program cannot draw (no framebuffer, D4) | - |
| Scrollback, console switches, `ESC[6n` | as every console program has them | must be rebuilt in telnet | - |
| Size | a small kernel addition, a small program | a second emulator (CTerm's `vt.c` is ~3,000 lines) | - |
| Other users | `cu`, a future telnet server's sessions, `less` on a saved `.ans` | only telnet | - |

**Choice: A.** The console gets what only it can do (a second font, six rules that change how bytes are drawn); telnet
does what is a rewrite of the stream (protocol, music strings, the Amiga console's own codes). Golden path: the one
parse that matters is the kernel's fast path, and nothing is drawn twice.

### 4.2 The PC font (the font question)

- **The glyphs:** a 256-glyph 8 x 8 CP437 font. Source: **Linux's `lib/fonts/font_8x8.c`** (VGA 8 x 8, CP437 order,
  GPL-2.0 like the kernel: D9 allows it; *read* from memory of the file, checked when imported). Not VileR's
  Ultimate Oldschool PC Font Pack (CC BY-SA 4.0 is not GPL-2 compatible) and not a font from a ROM image. 8 x 8 is the
  console's cell; PC art was drawn for 8 x 16 VGA, so it looks as it does in the PC's 50-line mode (IBM VGA50,
  SAUCE's name for it), the honest price of 80 x 32 with one font height.
- **The file:** `/usr/share/consolefonts/cp437-8x8.psf`, **PSF1** (Linux's console font format: `36 04`, mode 0,
  height 8, 2,048 bytes of glyphs; 2,052 bytes), built from the C source by a host tool, on every root image and in
  the base package. A standard format, so block 3's "PC and other bitmap fonts for the console" loads the same way.
- **Loaded on demand, per console, by the program that uses it** (golden path: nothing for machines that never call a
  PC board): `SYS_console` gets a new op, `CON_FONT` (value: a pointer to the PSF1 data and its size; 0 drops it).
  It applies to **the caller's own console** (its `ctty`), any user who is on it; the kernel copies the 2,048 bytes into
  a block of its own and, with more than one plane, builds that font's "four bytes a line" table (8 KB, as D14's
  `rglyph`). The console then draws every byte through that font. **Dropped when the caller ends** (as `tty_release`
  frees a raw mode), on `CON_FONT 0`, and never shared: another console, another user's session, the kernel log
  and panics keep Topaz. Memory: 2 KB, plus 8 KB at 2-4 planes, only while a PC board is shown or open.
- **Why not built in:** 2-10 KB in every machine's kernel for a few users' boards, and the RAM floppy is short
  (section 6). **Why not loaded by `init` at boot:** the same cost for everyone. **Why not root only:** telnet runs as
  the user; a per-console font affects only the caller's own screen.
- **Topaz for Amiga boards is already there** (the ROM's Topaz 8, Latin-1). The Amiga BBS fonts of the scene
  (MicroKnight, P0T-NOoDLE, mO'sOul: TrueSchool's, which SyncTERM ships; 8-row originals recoverable from their 16-row
  files) load the same way later, after their licence (GPL with a font exception, which version not checked) is.

### 4.3 Six console rules for boards

A second new `SYS_console` op, **`CON_RULES`** (value: bits; the caller's own console; owned and dropped like the
font; unknown bits -EINVAL so later bits keep their meaning). Each is a few instructions in `term.c`, tested only where
the console already branches (bytes below 32, the last column, SGR, `J`), so plain consoles pay nothing measurable:

| Bit | Rule | Why (*read*: knowledge base) |
|---|---|---|
| `CONR_PCCTRL` 1 | bytes 0x01-0x1F that are not BEL, BS, HT, LF, CR, ESC (and CAN/SUB inside a sequence) and 0x7F are drawn as glyphs; NUL is ignored | CP437 art draws with ☺ ♥ ► ◄ ↑ ↓ ⌂ (PC page rule, `ansi-art-fonts-and-parsing.md`) |
| `CONR_WRAPNOW` 2 | writing the last column wraps at once | ANSI.SYS and the Amiga console wrap at once; art made for them has no line end after a full row and may move the cursor right after it |
| `CONR_CLRHOME` 4 | `ESC[2J` also homes the cursor | ANSI.SYS and BBS terminals do; xterm does not; art that starts with `ESC[2J` alone lands wherever the cursor was (`screens-variants-utf8-wide.md`) |
| `CONR_BRIGHT` 8 | SGR 1 makes the foreground bright (colour + 8) and is not drawn bold; 22 and 0 undo it | ANSI-BBS's "bold" is the high-intensity colour (ESC[1;30m is the dark grey of shading); bold smear spoils block art (the BBX web terminal's rule for Amiga ANSI too) |
| `CONR_ICE` 16 | SGR 5 makes the background bright (iCE colours) instead of being ignored | iCE art; off by default (SyncTERM's default is blink, which the console cannot show: ignored as today), on by `-i` or the board's `CSI ? 33 h` (SyncTERM's code for it: *read from memory*, to check against cterm.adoc when built) |
| `CONR_CGAPAL` 32 | the console uses the CGA/VGA palette while it is shown (brown 0xA50, blue 0x00A, the greys 0x555/0xAAA) | art is drawn and checked in that palette (PabloDraw, Moebius, SyncTERM); Zenix's own palette was chosen for Topaz text. The Copper list's colours change at the console switch, as the depth does today |

**What the depth means for boards** (said by telnet once at connect when it is less than 16): **16 colours** show
ANSI art as drawn. **8 colours** (the stock A1200's default): a bright foreground is shown as the colour plus bold
(`CONR_BRIGHT` cannot do better without the planes), bright black is grey, iCE backgrounds are the plain colour:
art loses its shading. **4 and 2 colours** fold the colours by warmth (D14 section 11): readable menus, not art.
Owner question 2: `colours 16` on the stock A1200 costs 40 KB more chip RAM (160 KB of screen instead of 120).

### 4.4 The filter in telnet (stream rewrites only, no screen model)

A small state machine that tracks only whether it is inside an escape sequence; it never models the screen:

- **ANSI music, swallowed safely:** `ESC [ |` (SyncTERM's own) and `ESC [ N` (BananaCom's) up to SO (0x0E), and at
  most 512 bytes, so a missing SO cannot swallow the session. `ESC [ M` stays ECMA-48's "delete line" (SyncTERM's
  default treats only `ESC [ |` as music); `-m` makes `ESC [ M` followed by a music letter music too, for boards that
  use it. Played on Paula later (roadmap block 6), not now.
- **Form feed** (0x0C, the BBS clear screen) -> `ESC[2J ESC[H` in both BBS modes.
- **SyncTERM's font switch** `CSI 0 ; n SP D`: n = 0 (CP437) loads the PC font and the PC rules, n = 40-42 and
  37-39 (the Amiga fonts in SyncTERM's numbering, *read* `amiga/ansi-art-fonts-and-parsing.md`) go back to Topaz and
  the Amiga rules; other numbers are swallowed.
- **Amiga mode:** the 8-bit CSI 0x9B becomes `ESC [`; the Amiga console's own sequences that would mean something else
  on the console are rewritten or dropped (`CSI n u` with a number is the Amiga's line length, not "restore the
  cursor": dropped; `CSI 0 SP p` / `CSI SP p` are the Amiga's cursor off/on: rewritten to `CSI ? 25 l/h`; `CSI n
  t/x/y` draw nothing and the console already swallows them). *Read*: CTerm `vt.c` and `ansi-art-fonts-and-parsing.md`.
- **SAUCE is not needed:** boards send screens without it; a record that slips through shows as a line of text, as on
  every terminal. SUB (0x1A) is not drawn (DOS's end of file).
- **Device attributes (`ESC[c`)** are not answered (neither does the console today), so boards that probe for
  SyncTERM (`CSI = 67;84;...c`) never send its extensions; boards detect ANSI with `ESC[6n`, which the console
  answers. Revisited if a real board waits long for it.
- **Unix mode:** UTF-8 decoded to Latin-1 (`?` for the rest), nothing else.

### 4.5 Amiga boards (AmiExpress and friends)

AmiExpress asks "ANSI, RIP or No graphics" and sends ANSI in Latin-1 for Topaz, CR LF line ends over telnet, `~`
codes already expanded on the board's side (*read*, `amiga/amiexpress-caller-strings.md`); its pause prompt erases
itself with `ESC[1A ESC[K`; lines per screen from NAWS. Everything it sends is ECMA-48 plus the few Amiga console codes
of 4.4, drawn by the console's own Topaz: the Amiga mode needs no font load and costs nothing beyond telnet itself.
Its transfer protocols are Zmodem (the common one), Xmodem, Ymodem and Hydra (section 5).

## 5. Downloads and uploads

- **Zmodem first** (the protocol Amiga and PC boards use most), in **two small programs, `rz` and `sz`** (one file,
  two names, as `ifup` is `ifconfig`), reading and writing their standard input and output as lrzsz's do. So the same
  programs serve telnet now, `cu` later, and a telnet or ssh session *into* Zenix (`sz file` sends from Zenix). Loaded
  only while a transfer runs (golden path).
- **The engine: CTerm's `zmodem.c`** (the owner's own code, MIT, about 1,000 lines, verified both ways against
  lrzsz 0.12.20 with 0 B / 3,000 B / all byte values / 1 MB, `-8`, `-e`, and a line that drops and garbles bytes;
  *read*, `amiga/file-transfers-zmodem-ymodem-xmodem.md`), through its own table of I/O calls, so it builds for the
  PC test and for Zenix unchanged. lrzsz itself (GPL-2+, bigger, 1998) stays the test peer.
- **Start:** telnet looks for `** ZDLE B 0 0` (the board sends: start `rz`) and `** ZDLE B 0 1` (the board waits:
  ask "send which files?" and start `sz`) in the raw bytes before the telnet decoder, as CTerm does (the header has
  no 0xFF, so escaping cannot hide it). It spawns the program with two pipes, relays socket <-> pipes through the
  decoder and encoder (BINARY asked at connect; outside binary the decoder still gives the right bytes), shows a
  one-line progress on the bottom row from the program's standard error, and hands the bytes after the session back to
  the screen (the board's prompt that came with the `OO`). Five CAN (Ctrl-X) from the keyboard or the board cancel.
- **Where files go:** the directory telnet was started in (`rz`'s Unix rule; `-d DIR` to choose), said in the
  banner. Names are cleaned (no `/`, no `..`, no control characters, at most 255 bytes); **a file is written as
  `NAME.part` and renamed when whole**; an existing `NAME` is never overwritten (the new one is `NAME.1`, `.2` ...);
  a `NAME.part` shorter than the offered file is resumed (`ZRPOS`, CTerm's verified rule). A full disk ends the
  transfer with the reason, keeping the part. On the RAM floppy the directory is a RAM disk: the page says so.
- **The Zmodem traps** in the knowledge base are the engine's tests (`go/zmodem-and-telnet.md`): CR LF after a hex
  header, ZDLE escapes and ZRUB0/1, ESCCTL including CR, nothing after ZSKIP, five CANs, ZFIN with a file open, the
  OO after ZFIN, binary mode or the CR NUL folding eats file bytes.
- **Xmodem and Ymodem later** (they cannot be detected: the board waits silently; the escape menu's `d`), **Hydra
  later** (AmiExpress's bidirectional protocol; CTerm carries HydraCom's code). Owner question 5.

## 6. Size, memory and speed

| Part | Size (*estimates*, -Os) |
|---|---|
| Kernel: `CON_FONT`, `CON_RULES`, the per-console font and palette, the six rules, release at exit | 1.2-1.8 KB code, ~20 bytes per console |
| `telnet` (protocol, filter, keys, escape menu, libz and its socket calls) | 14-18 KB |
| `rz`/`sz` (one program) | 14-18 KB, plus CTerm's ~15 KB of Zmodem state while a transfer runs |
| `cp437-8x8.psf` | 2,052 bytes on disk; 2 KB + 8 KB (2-4 planes) while used |

- **The RAM floppy:** the kernel has 26,728 bytes left on `zenix-ram.adf` but **2,152 on the mount tests' floppy**
  (D19 section 14): this step's kernel growth must first take 8 KB from `RAMDISK_MOUNT_K`, as D19 already says.
- **Memory on the stock A1200** (2 MB chip, SLIP, 8 colours): 1,159 KB free with SLIP up (D19, *measured* in FS-UAE),
  minus telnet (~25-30 KB with its buffers) and, on a PC board, the font (10 KB): about **1,120 KB free while
  calling a board**; 40 KB less at 16 colours. The PiStorm (fast RAM, Ethernet at step 9) has room to spare.
- **Speed: drawing is the limit, not the network.** A 79-character line takes about 11 ms to draw on an A1200's chip
  RAM (architecture.md section 11, FS-UAE and the PiStorm, not a real 68020): roughly 7 KB/s of text at one plane and
  10-15% less at 3-4 planes (D14's estimate), under SLIP's 11.5 KB/s, so a 6 KB ANSI screen takes about a second on
  the stock A1200. **To be measured on the real stock A1200 and the 060**, never Emu68.
- **SLIP and long writes (a trap found while designing):** the kernel draws a write without letting any other
  process run, and SLIP's receive ring holds 1 KB, 89 ms of the line (D19 5.3). A telnet that wrote 4 KB at once would
  keep netd from the ring for over half a second and lose bytes (TCP sends them again, slowly). So **telnet writes
  to the screen at most 256 bytes per call** (about 35 ms of drawing), and netd, woken by the serial bytes, runs at the
  next return to user mode when it is ready and telnet's slice is spent; the losses are counted (`ifconfig`'s "bad"
  and lost bytes) in the A1200 test. A larger receive ring stays D19's own open point (step 5 choice 11).

## 7. Pseudo-terminals, SIGHUP and the telnet server

### 7.1 What needs them

**The client does not:** it uses the console's tty as any full-screen program does. A **telnet server** on Zenix
(logging in to Zenix over the network), later an **ssh server**, `script`, a terminal multiplexer and a ported
`expect` do: a program that is a terminal for another program. The remote control the roadmap plans can run
commands over pipes; an interactive shell over it needs a pty too.

**Choice: the client now; ptys, sessions, SIGHUP, SIGWINCH and `telnetd` later, as one step of their own** (owner
question 1), after the client is in daily use, with this section as its design and a review of its own then (a kernel
change: the tty layer). The design is fixed here so the client step builds nothing that must change.

### 7.2 Design (for the later step)

- **Names, as Linux and POSIX:** `/dev/ptmx` opens the master of a free pty; the slave is `/dev/pts/N`, both in
  `open_device` like `/dev/ttyN` (no devpts filesystem). `fstat` of a master gives N in `st_ino`, as a console gives
  its number today, so libz's `ptsname` needs no system call; `posix_openpt`, `grantpt` (nothing to do), `unlockpt`
  (nothing) and `openpty` are libz. BSD's `ptyp0`/`ttyp0` pairs are not taken (a program searches for a free name, and
  each needs ownership rules of its own).
- **Who may open:** anyone may open `/dev/ptmx`; `/dev/pts/N` only root and the uid that opened its master (Zenix's
  devices have no inodes to `chown`); `telnetd` (root) opens both and starts `login` on the slave.
- **One tty layer:** the line discipline (`tty.c`: cooked lines, echo, ^C/^Z to the foreground group, raw, nosig) is
  shared; what differs is where a tty's output goes (a console: `term_write`; a pty: a ring the master reads) and where
  its input comes from (`kbd_work`; the master's writes). `struct tty` gains an output ring and a kind; ttys 1-10 stay
  the consoles, ptys take numbers above them (`NPTY` 8, allocated at open: ~3.5 KB each with the two rings), and
  `ctty` (a byte) names either. `login`'s `/etc/autologin` test (console 1 by `st_ino`) never matches a pty.
- **Controlling terminal and sessions:** the spawn rule stays ("the tty of fd 0 at spawn, else the parent's"), so
  `telnetd` spawning `login` with the slave as fd 0-2 gives the session its terminal. A tty records its **session
  leader** (the first process spawned onto it from another tty: `login`, then the shell it hands to, D18). `setsid`
  in libz makes the caller its own group (D16) and, when its fd 0 is a tty nobody leads, that tty's leader: enough
  for the ported servers that call it; a full POSIX session id is not added (D16's "process groups only").
- **Hang-up, SIGHUP:** the master's last close hangs the slave up: SIGHUP then SIGCONT to its session leader and its
  foreground group (POSIX's rule for a modem's carrier); reads on the slave give 0 (end of file), writes -EIO. The
  shell, on SIGHUP, sends SIGHUP to its jobs and ends (bash's rule), so nothing is left running after a dropped
  connection. Consoles never hang up (no carrier); `cu` and the serial port still have no carrier detect.
- **Window size, SIGWINCH:** each tty gets a size (a console's is the screen's); a master sets its slave's size (from
  NAWS in `telnetd`), which sends SIGWINCH to the slave's foreground group; `SYS_ttyinfo` keeps its frozen meaning
  ("this process's tty's size") and answers a pty's. Setting needs one new call (TIOCSWINSZ/TIOCGWINSZ and
  TIOCSCTTY's subset through one `SYS_ttyctl (fd, op, arg)`, its number claimed then; libz's `ioctl` maps the Linux
  names).
- **`telnetd`:** the server side of section 3's protocol (the options mirrored: WILL ECHO, WILL SGA, DO TTYPE, DO
  NAWS), one process per connection spawning `login` on a pty and relaying; off by default; root's `/etc/` file to turn
  it on. How a TCP listener is started at boot is not answered by D11 (its services start on a message to a port
  name, not on a connection): decided in that step (an init-started daemon as `ifup -a` is, or inetd's model).
  Unencrypted: for the home network until ssh; said in its page.
- **Cost (later, *estimate*):** 2-3 KB of kernel code, ~3.5 KB per open pty, `telnetd` ~12 KB per connection plus
  the session.

## 8. Tests

### 8.1 On the PC

- **A BBS simulator** (`tools/host/bbs-sim.py`, no root, in the user and network namespace `slip-host.py` and
  `eth-test.sh` already use): a telnet server that negotiates (and records what the client asked and answered:
  BINARY, SGA, ECHO, TTYPE's name, NAWS's size, no loops), then on command sends screens: every CP437 byte 1-255 in a
  grid, colours with and without SGR 1, iCE blink, `ESC[2J` without `ESC[H`, a full 80-column row followed by a cursor
  move, cursor codes and `ESC[6n` (its answer checked), music strings (with and without SO, and an unterminated one),
  FF, SyncTERM's font switch, an Amiga screen (Latin-1, 0x9B CSI, LF-only lines, `CSI n u`), 0xFF doubled inside text,
  and **a Zmodem upload by the real `lsz`** (and a download to `lrz`), files compared by checksum. **The test art is
  made for the test** (generated byte grids and a small hand-drawn screen in the repository); real art packs are
  copyrighted and stay out of the public repository (the owner's own `~/Amiga/BBS` files may be used locally, never
  committed).
- **telnet's core on the host:** the decoder, encoder, Q-method negotiation, the filter and the key translation as
  plain C without system calls (as netd's core), built for Linux and run against the simulator; **fuzzed** (IAC and SB
  sequences, music strings, 0x9B) one run at a time with a memory cap (this machine's rule).
- **The Zmodem engine** with CTerm's own PC tests against lrzsz (both ways, the sizes and the noisy line).
- **The console rules and the PC font** in `tools/term-test.sh` (the host model of `term.c`): each rule's cells, the
  font's glyph index, the palette switch at a console switch, the release when the owner ends; the existing fuzz then
  with every rule bit set.

### 8.2 In FS-UAE

- **Over the A2065** (D19 step 8: the serial port stays free, so the test rig types keys and reads the console's copy
  on the serial line as today): `telnet 10.0.2.2 PORT` to the simulator through slirp; the negotiation record; key
  translation (Swedish letters typed on the emulated keyboard arrive as CP437 or Latin-1 or UTF-8 by mode; Backspace
  as BS; Ctrl-C reaches the board); the escape menu (`q`, `z` then `fg`); the connection closed by the board; a
  download and an upload compared byte for byte, `.part` and the no-overwrite rule, a cancel; `NAME.part` resumed.
- **Screenshots** (`tools/shot.py`, the cage rig) at 1, 3 and 4 planes: the CP437 grid compared with the font's own
  bitmaps, the CGA palette, iCE, bright, wrap-now; a console switch away and back while a PC board is shown (the other
  console in Topaz and Zenix's palette); after telnet ends, the console in Topaz again.
- **Over SLIP** (scripted, as `slip-test.sh`: the serial port is the network's): a screen of 32 KB through telnet on
  the stock A1200 machine, the bytes lost on the ring counted.
- **Memory** with `free` and `ps` while a board is shown (the matrix's 2 MB and 1 MB machines).

### 8.3 On the real machines (the owner's)

The stock A1200 over SLIP and the PiStorm over GENET (after D19 step 9): the owner's own boards (an Amiga board, a
PC board), screens and a Zmodem download each; the drawing speed, the SLIP losses while a screen draws, the time for
a 6 KB screen, at 8 and 16 colours, labelled by machine.

## 9. Build order

Each step small, merged with its tests and the full suite. Steps that touch `kernel/kernel.h`, `syscall.c`,
`term.c`, `gfx.c`, the `Makefile` (`UAPPS`, images), `user/usys.h` or `libz` are **the main session's** (CLAUDE.md
"Sessions"); a side session may take only the host-only parts marked *side*, with this note's names and formats.

0. *side:* `tools/host/bbs-sim.py` with its own self-test; the PSF1 font built from Linux's `font_8x8.c` by a host tool
   (`tools/mkpsf.py`, the C source kept with its licence header in `fonts/`); telnet's core (`net/telnet/`: decoder,
   encoder, options, filter, keys; plain C) with host tests and the fuzz harness; the Zmodem engine (`net/xfer/`,
   CTerm's `zmodem.c` and its I/O table) with its host tests against lrzsz.
1. *main:* the console: `CON_FONT` and `CON_RULES` (numbers and bits claimed in CLAUDE.md first; frozen once merged),
   the per-console font, rules and palette, their release at exit, `termtest` pages for the PC font and the rules;
   `term-test.sh` and the screenshot tests. The mount tests' RAM disk shrunk first if the kernel needs it.
2. *main:* `telnet` in the Unix mode: connect, options, NAWS, TTYPE, the escape key and menu, close; its page,
   `commands.md`, `features.md`; tests against the simulator over the A2065 and over SLIP.
3. *main:* the BBS modes: the filter, the PC font load, the rules, key translation, the 256-byte writes; screenshots.
4. *main:* `rz`/`sz` and telnet's autostart, relay and progress line; the transfer tests.
5. *main, if the owner wants it (question 4):* the phone book.
6. *owner:* the real machines (8.3).
7. *later, its own step and review:* ptys, session leaders, SIGHUP, SIGWINCH, `SYS_ttyctl`, `telnetd` (section 7).

## 10. Frozen once built (CLAUDE.md's list gets them in the build step)

`SYS_console`'s ops `CON_FONT` and `CON_RULES` (append-only numbers after `CON_COLOURS` 1) with their arguments and the
`CONR_*` bits; the font file format (PSF1, a standard) and its path; `rz`/`sz`'s standard input and output contract
(lrzsz's); telnet's options as a command (`-p -a -x -r -e -d -i -m`); later the pty names `/dev/ptmx` and `/dev/pts/N`,
`st_ino` of a master, `SYS_ttyctl`'s ops (Linux's TIOC names and values) and the SIGHUP and SIGWINCH rules. Nothing
here changes an existing frozen meaning: `SYS_ttyinfo` keeps "this process's tty's size", the consoles' `st_ino` stays
their number, `TTY_RAW`/`TTY_NOSIG` are unchanged.

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
   PC board, and boards that send SyncTERM's font switch change it themselves.

Last checked against the code: 2026-10-27 (main at f03180e).
