# D62. The screen's settings: `screenprefs`, one engine with three faces

The partner, 2026-11-27 (decisions.md "Screen-mode settings, started"): "Start the screen modes prefs. It must work
both in console and in desktop to set the screen mode and things." Approved 2026-11-23 ("Screen settings and the
mouse"): one settings program for the console's and the desktop's screen modes and sizes, taking over conprefs and
deskprefs with one system's conventions. The shape (agreed with the partner in the main session): one program over one
engine, with three faces, as Linux and AmigaOS do it. This is the note, revision 1, and what was built (branch screen).

## 1. In plain words

- **`screenprefs`** sets everything about the screen: the video standard (PAL or NTSC), the console's mode, colours
  and Unicode font, the desktop's mode, depth, font and colours (its theme), and a graphics card's size.
- **It looks like the place it runs in.** Under the desktop it opens a window (AmigaOS's preference editors' look,
  mouseprefs' code). On a console it draws a full-screen form in the console's own terms, worked with the keys. From
  a script it is a command (`screenprefs --list`, `screenprefs console hires-lace`, `screenprefs desktop superhires`),
  as `xrandr` is on Linux. One program and one set of rules behind all three.
- **Test, Use, Save, Cancel**, as before. A new mode or video standard asks to be kept: if nothing answers within 10
  seconds (a monitor that cannot show it stays dark), the old mode comes back by itself.
- **conprefs and deskprefs** are other names of screenprefs now (the old names keep working, with their options);
  their settings are read once from the old files and moved over by the first Save.

## 2. How others do it (read, for the shape only)

- **Linux:** the kernel owns the modes (KMS; the text consoles' `fbset`/`setfont`), and the faces are separate
  programs over it: `xrandr` for scripts, a Displays panel in the desktop, which applies a mode and reverts after
  some seconds unless "Keep changes" is pressed.
- **AmigaOS:** one ScreenMode editor lists the display database's modes (PAL:, NTSC:, DblPAL:, Productivity...,
  each with its sizes and depths) and writes `ENV:`/`ENVARC:` (Use, Save); its Test opens a test screen for some
  seconds. The Overscan and Font editors are separate programs.
- **Taken:** the engine-and-faces split, one settings file the boot reads, Use for the session and Save for good, and
  the timed fall-back. Not taken: a display database (Zenix's modes are few and known: section 4).

## 3. The kernel: `CON_MODE` grows (500 bytes)

The console's mode switch exists (D28 22: hires or interlaced, the screen and the consoles' grids made again, SIGWINCH).
What it lacked is the video standard and a way to ask what the chips are. `SYS_console` op 11 `CON_MODE` grows at its
end (kernel/kernel.h, frozen in docs/frozen-interfaces.md, claimed in docs/claims.md):

| a2 | meaning |
|---|---|
| `CONM_HIRES` 0, `CONM_LACE` 1, `CONM_ASK` 0xFF | as before: the answer is the mode, 0 or 1 |
| a mode \| `CONM_PAL` 0x10 or \| `CONM_NTSC` 0x20 | the mode and the video standard (root); the answer is the mode \| the standard's bit; `-ENODEV` the other standard on an OCS Agnus, `-EINVAL` both bits |
| `CONM_INFO` 0xFE | any process: the mode \| the standard's bit \| `CONMI_AGA` 0x100 (Lisa) \| `CONMI_SHRES` 0x200 (an ECS Denise or Lisa: super-hires) \| `CONMI_BEAM` 0x400 (an ECS or AGA Agnus: it switches PAL and NTSC) \| `CONMI_NTSCX` 0x800 (an NTSC Agnus: the crystal) |

**The switch** is the existing one (`gfx.c` `screen()`): the new ring and Copper lists first (for the new number of
rows: 32 or 25, 64 or 50 interlaced), the consoles' new grids (`term_regrid`), then with interrupts masked
`BEAMCON0` (0x20 PAL, 0 NTSC: only bit 5, the standard beam; AHRM "BEAMCON0", knowledge base demoscene/aga-specifics.md
3.7), the lines of a field (256 or 200), the long field's last line (312 or 262, interlace's field wait), the ring
and `COP1LC`; the text moves into the new grids, SIGWINCH to each console's foreground. The Agnus's id is VPOSR's bits
14-8 (bit 5 set: ECS or AGA, which have BEAMCON0); Denise's id the low byte of $DFF07C (0xFC ECS, 0xF8 Lisa).
`CON_GRAB`'s frame size follows (640 x 200 after a switch to NTSC), so the desktop started next builds on it. The
standard is the system's: one beam shows the console and the desktop, so it is set where the kernel owns the screen
(the console, init at boot); while a display server holds it, `CON_MODE` is `-EBUSY` as before.

Sizes: gfx 9,005 -> 9,479 bytes (`build_cop` takes the standard's lines, the chips' answer, the wait for the beam, the standard again after ZINE), syscall +16, zine +4;
`kernel.bin` 235,008 -> 235,504, the mount tests' floppy 1,536 -> 1,040 bytes left (800 kept).

**Not in the kernel:** the console's columns. Its cells are 80 wide (`COLS`, the grids, `STRIDE`): super-hires (160
columns) or lores (40) would be a new console, not a mode; a later step if wanted. The 31 kHz modes (DblPAL,
Productivity, Multiscan) need the programmable beam and a ring whose wrap fits a 115-clock line (knowledge base
amiga/fuzix-on-amiga.md "Screen modes and 640x480 at 31kHz": done for Fuzix, verified in Amiberry; FS-UAE cuts them
with two or more planes, so no test here could check them): section 12.

## 4. The modes per machine

| Machine | Video | Console | Desktop (chipset) | Card |
|---|---|---|---|---|
| OCS (A500-style Agnus) | its crystal's only | hires 80 x 32 (NTSC 25), hires-lace 80 x 64 (50); 2-8 colours | hires, hires-lace; 8 colours | - |
| ECS (A3000, A600) | PAL or NTSC | the same | hires, hires-lace; 8 colours | - |
| AGA (A1200, A4000) | PAL or NTSC | the same, 2-16 colours | hires, hires-lace, super-hires 1280 x 256 (NTSC 200), super-hires-lace 1280 x 512 (400); 8 or 16 colours | - |
| With a Picasso96 card (zenixboot RTG) | as above | as above | the card's size and format | the size asked for at the next start |

- **Depth on the desktop:** 3 or 4 planes (8 or 16 colours). Fewer would need themes made for 4 or 2 pens (zdesk maps
  16 pens to 8 with `map8`; there is no map to 4): not offered. OCS and ECS keep 3 (4 hires planes at the 16-bit
  fetch take every bus slot of the picture's lines, D38 6.1).
- **Super-hires is AGA's only here:** at the 64-bit fetch (FMODE 3) 4 planes of 1280 pixels cost what 8 hires planes
  do (80 of a line's slots); an ECS Denise shows super-hires with 2 planes only, which the 8- and 16-pen themes do not
  fit. The chipset's drag of a whole window in playfield 2 (D38 "Dragging with the chips") is hires-only: in
  super-hires a drag is the outline or the CPU's, as on OCS.
- **Chip RAM:** super-hires 160 KB (4 planes; 120 with 3), super-hires-lace 320 KB; a screen bigger than hires is
  taken only with 512 KB more free (D38's rule for interlace), else hires and a line in the log.

## 5. displayd

- **`DPY_MODE`** (request 15) grows: modes `DPYM_SHRES` 3 and `DPYM_SHRES_LACE` 4 (AGA, else -EINVAL); a[1] the depth
  (0 as it is; 3, or 4 on AGA; -EINVAL another), and with a[1] not 0 the answer is the mode | the depth << 8; a[1]
  `DPYMD_ASK` 0xFF only asks both. Old clients pass a[1] 0 and get the mode as before. libdisplay: `dpy_mode2`.
- **`EV_SCREEN`'s `x`** is the screen's depth now (0 from an older server); libdisplay's `dpy_depth` follows it, and
  zdesk restyles (its pens and gadget pictures) when it changes.
- **The Copper list:** BPLCON0 `0x0240` (SHRES) instead of `0x8200` (HIRES), the same fetch window and FMODE 3; the
  fixed part always has 16 colour moves, so a depth chosen later finds its colours.
- **The pointer** in super-hires moves in the screen's own pixels (D61's `struct umouse`: 1280 wide, the sprite at
  `x >> 1`), its vertical gain a quarter of the horizontal (a super-hires pixel is a quarter as wide as a line is
  tall), so a move covers the same distance on the glass both ways. D55's screens still get the chipset's units.
- **The file at the start:** `/etc/screen`'s desktop lines, else (none there) the older `/etc/zdesk/screen`.

## 6. The settings files

One format, one reader (`display/lib/screenfile.c`, displayd's and screenprefs'; zdesk and init read the words they
need themselves, for their size). `setting value` lines, `#` comments, frozen in docs/frozen-interfaces.md:

```
video auto|pal|ntsc                     # one beam: the console's and the desktop's standard (auto: the machine's)
console hires|hires-lace
console-colours auto|2|4|8|16
console-font auto|none|NAME             # /usr/share/consolefonts/NAME.zcf
desktop hires|hires-lace|superhires|superhires-lace
desktop-depth auto|3|4
desktop-font NAME SIZE|builtin
rtg front|WIDTHxHEIGHT[xBITS]           # the card's size at the next start (BITS 16, 24, 32)
```

- **`/etc/screen`** is the system's (root's Save): every line. init reads `video`, `console`, `console-colours`,
  `console-font` at every start; displayd the `desktop` lines (the desktop's default and dlogin's screen).
- **`~/.config/screen/screen`** is a user's: the `desktop` lines. zdesk applies it when the user's session starts
  and puts back what it found at Log out (D38 "Preferences"). The console is every console's, so its lines are
  root's only (another user sees them and is told).
- **The older files** (conprefs': `/etc/console`'s `colours`, `font`, `mode`; deskprefs': `/etc/zdesk/screen` and
  `~/.config/zdesk/screen`, `mode` and `font`) still count wherever the new file has no line of that kind: init,
  displayd and zdesk fall back to them, so an updated system keeps its settings untouched. The shipped `/etc/screen`
  therefore has its settings as comments only. screenprefs reads the old lines, says once where they came from, and
  its first Save writes them into the new file, takes `colours`, `font` and `mode` out of `/etc/console` (its
  `login` line and comments kept) and removes the old desktop file.
- **`/var/run/console`** (D28 22) stays the console's Use for the session (init removes it at boot), with a
  `video` line grown at its end.
- The theme stays zdesk's file (`~/.config/zdesk/theme`, `theme.use`), as deskprefs had it.

## 7. RTG: the card's size (zenixboot `RTGMODE`)

D43 section 13 question 2: `zenixboot RTG` takes the front Picasso96 screen as AmigaOS left it. Now `zenixboot RTG
RTGMODE=1280x960x16` (WIDTHxHEIGHT, optionally xBITS: 16, 24 or 32; 16 when left out; `RTGMODE` implies `RTG`) first
asks Picasso96 for its best mode near that size and depth (`p96BestModeIDTagList`), opens a screen of it
(`p96OpenScreenTagList`), and then takes the front screen as before; no such mode, or a screen that cannot be opened,
is said, and the front screen is taken as it is. Without `RTGMODE` nothing changes, so an old boot line works with a
new zenixboot and a new boot line is refused by an old one (`ReadArgs`), never misread. zenixboot runs under AmigaOS
and reads no Zenix file, so screenprefs' "Card size" (`rtg` in `/etc/screen`) is the size the user asks for and the
line to put in the boot script; it counts from the next start, said every time it is set. zenixboot is copied by hand
on the partner's card (D43 section 14): this one is optional.

## 8. The program

`display/prefs/screenprefs.c`, about 55 KB on the disk (deskprefs 23 KB and conprefs 18 KB are gone; it links the
window code, the keys parser and the screen file's reader), cold: -Os, only in memory while it runs.

- **The face:** the window when the desktop runs and standard input is not one of the ten consoles (a terminal window
  on the desktop); the form on a console (or anywhere the desktop is not running: telnet, the serial line). `-W` and
  `-T` choose: the Zenix menu's line is `screenprefs -W` (zdesk's programs inherit its console as standard input). As `deskprefs` it is the window, as `conprefs` the form, with their old options (`-v`, `-q`
  and conprefs' `-q` line).
- **Each face applies its own screen's rows:** the form the console's and the video standard, the window the
  desktop's; the other rows are shown, kept by Save and counted from that screen's next start (said in the message
  line). Applying the console's from the desktop could not work anyway: while the desktop holds the beam,
  `CON_MODE` is -EBUSY.
- **The rows:** Video, Console mode, Console colours, Console font, Desktop mode, Desktop depth, Desktop font, Desktop
  colours (the theme), and Card size when a card is in use or `/etc/screen` names one.
- **The fall-back:** after a Test, Use or Save that changed a mode, depth or standard, "Keep this mode?" with the
  seconds left; `y` or `k` (the window: Keep, the first button) keeps it, `n`, `r` or Esc (Back) and silence for 10
  seconds go back to the mode before, the rest of the choice kept. Save writes only a kept mode.
- **Keys:** one keys file, `~/.config/screenprefs/keys` (display/keys.h), `screenprefs -K` prints the defaults; the
  form and the window read the same actions (up, down, next, previous, test, use, save, cancel, help, keep, revert,
  redraw). The window is also clicked (Shift-click backwards), as mouseprefs.
- **The command line:** `--list` (`-l`) the modes this machine shows, one a line, the one in effect marked `*`;
  `-q` the settings in effect in one line; `video pal|ntsc|auto`, `console [MODE] [colours N] [font NAME]`,
  `desktop [MODE] [DEPTH] [font NAME SIZE|builtin]`, applied live where the screen is free (the console's when the
  kernel shows it, the desktop's when it runs), `-s` saved too, `-y` without the question (a script); asked at a
  terminal, an unanswered new mode goes back after 10 s and the status is 1. `-s rtg front|WxH[xBITS]` (save only).
  Every --help/--version, usage (status 2) and error line is the system's (one-system audit section 3).

### 8.1 Mode providers (the coordinator, 2026-11-27: the Pi's own graphics driver comes next)

The engine never asks the hardware for a list itself: each display's modes come from one **provider**, a small table
entry in screenprefs (`struct provider`: the display's word, `here()`, `modes()`), each mode a `struct smode`: the
provider's own number, the settings word (or none: a size `WxHxBITS`), width, height, bits a pixel or the planes the
desktop may have in it, and whether it counts now or from the next start. The faces' rows, `--list` and the files
only see these entries. Built in:

| Provider | Display word | Its modes | Live |
|---|---|---|---|
| the chipset | `desktop` (and the console's two, the kernel's) | from `CON_MODE`'s `CONM_INFO`: hires, hires-lace, and on AGA super-hires and super-hires-lace, depth 3 or 3-4 | yes (`DPY_MODE`) |
| a Picasso96 card | `rtg` | the sizes zenixboot may ask Picasso96 for (`RTGMODE`); the card's own size while it shows the desktop | at the next start |

**How the Pi's driver plugs in** (not built, the next work): one more entry in `providers[]`, its `here()` true
when the driver serves the desktop, its `modes()` the EDID's modes the driver reports (through a request to its
server, numbered when it is built), each a `WxHxBITS` entry marked live if the driver switches at run time. Its
setting is a line of `/etc/screen` of the same form (a display word and a mode), so `/etc/screen`'s format, the
faces and the scripts' commands stay as they are; only `screenfile.c` learns the new word, as it learned `rtg`.

## 9. conprefs and deskprefs

Decided here: **other names of screenprefs**, not removed. Their names are in scripts, in people's hands and in the
frozen list (`deskprefs [-v]`, `conprefs [-q]` and its line), and an alias costs two directory entries (as `gunzip`,
`more`). The program is one: `user/conprefs.c` and `display/prefs/deskprefs.c` are gone, their man pages say "another
name for screenprefs", docs/commands.md lists them on screenprefs' row, and the Zenix menu's Preferences line is
"Screen = screenprefs" beside "Mouse = mouseprefs".

## 10. Tests (FS-UAE, the stock A1200 with 4 MB fast; tools/screenprefs-test.sh, at stock speed in tools/check.sh)

- `cli`: `--list` and `-q` compared on the AGA PAL machine; conprefs -q's old line; usage and refusal; the video
  standard switched live (80 x 25, SIGWINCH, the grid read back), PAL again, interlace; the fall-back unanswered at
  the terminal (back after 10 s, status 1); `-s` writing `/etc/screen` with its comments kept and `/etc/console`'s old
  lines moved out; a reboot with init applying NTSC, interlace and 8 colours (80 x 50).
- `con`: the form typed on the serial line: Test of 8 colours and Cancel; the mode and Use, kept with `y`; the mode
  and Test left unanswered: back after 10 s; Save; screenshots of the form and of its question.
- `desk`: the window clicked under zdesk: interlace and Test kept, super-hires (1280 x 256) kept, 8 colours and Use,
  super-hires-lace unanswered (back after 10 s), Cancel; with the keys: Save to `/etc/screen`; a user's Save of the
  desktop to `~/.config/screen/screen` with the old `~/.config/zdesk/screen` moved; a reboot on the saved mode.
- `rtg`: rtg-test's pretend card: `--list` names it, the desktop's mode is the card's (a mode refused), `-s rtg`
  writes the size and says the zenixboot line; a size out of range refused.
- prefs-test (deskprefs' window layout and conprefs' screen) is retired: these four parts check the same buttons,
  files and reboots on the one program. The tests around it are run too: con-test, gcon-test, lace-test, dpy-test,
  zdesk-test, mouse-test, rtg-test, cmds2-test (conv.sh: --help and --version of every program, the links included).

## 11. As built (branch screen, 2026-11-27)

- **Kernel:** section 3 (`kernel/gfx.c`, `syscall.c`, `zine.c`, `kernel.h`).
- **displayd:** super-hires and the depth (section 5), `/etc/screen` read first; 85,424 -> 87,288 bytes on the disk.
- **zdesk:** the user's `~/.config/screen/screen` (else the older file) with `desktop-depth`, a depth change restyles
  it; 45,112 -> 45,948 bytes. dlogin (zdesk's reader linked in) 25,704 -> 25,744.
- **init:** `/etc/screen`'s console lines and `video` over `/etc/console`'s, read a line at a time (its stack is 4 KB),
  the first line not understood said as "init: FILE: WORD VALUE: not understood"; 12,984 -> 13,880 bytes (init is
  resident: 900 bytes more for every user).
- **screenprefs** 56,264 bytes on the disk; conprefs (18,416) and deskprefs (23,936) gone: 14 KB more on the disk
  than the two together (the keys parser, the screen file's reader and the form).
- **zenixboot** `RTGMODE` (section 7): 56,160 -> 57,156 bytes; built, run only through `CHECK`-less paths in FS-UAE
  (no Picasso96 there): **not run on a card**.
- **The shipped files:** `/etc/screen` (every setting as a comment, so an updated system's older files still
  count), `/etc/console` with only its `login` line, `/etc/zdesk/screen` gone, the Zenix menu's "Screen = screenprefs
  -W".

**The kernel's review** (one, independent, 2026-11-27, on the built code; taken): ZINE's way back set the machine's
own standard while the kernel's lists stayed NTSC (now `gfx_zine_back` sets the one shown again); a switch to NTSC
written while a PAL beam is past line 262 would run on to the counter's wrap, a frame without a vertical blank (now
written while the beam is above line 240, waited for at most 100 ms); the free-after-a-frame wait used PAL's E-clock
(now `eclock_hz`). Checked and right: the old values, the checks, the ids, the window's stop line for both standards
and both `DIW_V`s, NTSC interlace's field wait, the switch under one interrupt mask, the grids, `-EBUSY` while held.

**The code review** (one, at the end): section 14.

## 12. Later (not built)

- The 31 kHz modes on AGA (DblPAL 640 x 512 non-interlaced, Multiscan 640 x 480): the programmable beam in the
  kernel's and displayd's Copper lists, the ring's wrap in two steps (Fuzix's way); testable in Amiberry, not FS-UAE.
- A console of 160 or 40 columns (super-hires, lores), overscan rows.
- Fewer desktop colours (2 planes) with themes made for 4 pens; 256 colours on AGA (8 planes, the pens beyond 16).
- zenixboot reading the card's size from a file on its boot partition instead of its command line (Zenix would
  write that file with zpkg's boot-partition writer): needs the partner's yes, as it writes to an AmigaOS partition.

## 13. What the tests found while building

- **Super-hires's fetch window:** with hires's `DDFSTOP` `$C0` the picture was sheared line by line (half of each
  line fetched, the modulo then wrong): at the 64-bit fetch a super-hires unit of 64 pixels takes 8 colour clocks, so
  1280 pixels are 20 units from `$38`: `DDFSTOP` `$D0` (verified by the screenshot, FS-UAE AGA).
- **The face from zdesk's menu:** zdesk's programs inherit its console as standard input, so "standard input is a
  console" chose the form on the hidden console; the menu's line says `-W`.
- **A session's depth:** after a Use of 8 colours, a new window showed the file's "auto (16)"; the depth in effect is
  now the shown one when it differs from what auto gives.
- **The serial line's keys:** the test's typing trickles bytes, slower than the shared key reader's 30 ms for an
  escape sequence; the form's test uses Tab and Space (the keys file's single-byte keys) beside one arrow.
- FS-UAE switches PAL and NTSC by `BEAMCON0` live (the console 80 x 25 read back, the reboot's 80 x 50); a real ECS/AGA
  machine and monitor are the next check (a PAL-only TV may show nothing at 60 Hz: the fall-back is for that).

## 14. The code review (one, 2026-11-27, after building)

Taken (each fixed, the tests run again): zdesk's reader looked at a blank first line's unset word (`conf_screen`,
a crash at a session's start for a file starting with a comment); with the desktop holding the screen, the form's
Save and `-s console` stopped at the live step and saved nothing (now "kept for Save": saved, said, not applied;
screenprefs-test desk checks it); `/etc/console`'s older lines and the older desktop file were removed by any Save,
also one that wrote none of the lines replacing them (`-s video`: now only with their own rows; cli checks it);
the command line's saves wrote the theme file (now only the form's, which has the row); on a card, Save wrote the
chipset's desktop mode as hires over the file's own (now left out); a message longer than the window's line
overflowed its copy (bounded); the font list's 64 entries (a TrueType font takes 8) could drop the font in use (128
now); zdesk did not fall back to the older file when the new one had no desktop line (it does, as screenprefs);
a Use's colours were not shown as in effect (read from `/var/run/console` as the font); the tests' `after` matched
`state-5a` for `state-5` and a check without effect (both fixed). Left as designed: in super-hires the pointer's
speed counts the screen's own pixels (half as far on the glass at 100%, D61's rule); a mode word followed by junk
(`1280x720xfoo`) in a command is refused only where it does not parse as a number.
