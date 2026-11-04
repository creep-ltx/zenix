# D38: the desktop (design note, revision 1)

Roadmap block 5. Written 2026-11-04 for the owner to choose a look and answer the questions in section 9 **before any
code is written**. Nothing here is built; no number is claimed (the first step claims its own, `docs/claims.md`).
Not yet reviewed: the first step is a deep core change (the screen and the keyboard leave the kernel's hands while the
desktop is shown), so it gets two independent reviews once the owner has answered.

## 1. In plain words

**What the owner asked for** (ROADMAP block 5, 2026-11-03/04): a desktop before the first public release, starting
small: "a small minimal desktop with floating windows. Something classic but modern." And: "it's a Unix-like system.
Several desktop choices are expected."

**What this note proposes:**
- **Two programs, as on Linux.** A *display server* (`displayd`) owns the screen, the blitter, the pointer and the
  keyboard while the desktop is shown, and draws everything. A *desktop* (`desk`) is an ordinary program that decides
  where windows go, which one has the keyboard, what the frames look like, and runs the top bar. A tiling desktop
  later is just another program in `desk`'s place; the windows and the programs in them do not change.
- **Drawing is done by the server, on the screen itself, the Amiga way.** Programs send drawing orders in batches; the
  server draws them with the blitter, clipped to what is visible, and keeps the hidden parts of windows in spare chip
  RAM, so moving or raising a window is a few blitter copies and never makes a program draw again. This is what
  Intuition does on AmigaOS and why it feels fast on a 7 MHz machine; the Linux-style "every window has its own
  picture, the server pastes them together" costs too much on a 14 MHz 68020 (section 3).
- **16 colours on the AGA screen, 640x256** (4 bitplanes, the console's own depth today), 8 on OCS/ECS, and the
  two-colour look (c) at one bitplane for the slowest machines.
- **The first step** (section 7): the server, a floating desktop with move, resize, raise, close and minimise, a top bar
  with a menu, a window list and a clock, a few desktop icons, a terminal window running the shell, configurable
  keys, an About window. Everything else (workspaces, the tiling desktop, a graphical file view, TrueType fonts,
  preferences, the clipboard) comes after, on the same protocol.
- **The text consoles stay** exactly as they are: Amiga+F1..F10 still shows them, each with its login. The desktop
  lives on one of the consoles, like X on Linux: you log in and type `desk`.
- **Four looks to choose from**, drawn as real Amiga pixels (section 8). The favourite so far is (a), flat with a hint
  of depth.

## 2. What exists today (read from the code and the notes)

- **The display is in the kernel** (`kernel/gfx.c`, `kernel/term.c`, architecture.md section 11 "Display"): ten text
  consoles, 640 pixels wide, 256 lines on PAL (200 on NTSC), interleaved bitplanes, the Copper moves the plane
  pointers to scroll, the blitter scrolls regions and fills, the CPU draws glyphs a byte per plane. Depth by machine
  (D14, D18): AGA with fast RAM 4 planes at the 64-bit fetch (FMODE 3: 16 colours for the DMA cost of one plane);
  OCS/ECS and chip-only 3 planes; the setting `/etc/console colours N`.
- **Fonts:** Topaz 8 grabbed from the ROM at boot (never shipped), the 8x8 supplement `zenix-8x8.zcf` (D28: box
  drawing, CP437 symbols, stand-ins), PC fonts as PSF for telnet (D20).
- **Input:** the keyboard's raw codes go through the kernel's keymap (`sv`, `us`) into the shown console's tty;
  Amiga+F1..F10 switch consoles. The mouse is polled at 100 Hz (`SYS_input`); there is no pointer yet.
- **Messages** (architecture.md section 8, DESIGN D1): named ports, synchronous calls that pass a pointer (no
  copies: the server reads the client's memory in place), `replyrecv`, priority inheritance (D10), cancel marks
  (D23), events and `poll` on ports (D16). A server must never block calling a client.
- **Not built yet, and needed:** pseudo-terminals (designed in D20 section 7.2, "a step of their own, two reviews"),
  `SIGWINCH` (reserved in D16), shared libraries (D13 stage 2, decided later).

## 3. The split: who does what

### 3.1 Three ways it has been done

| | **X11** | **Wayland** | **AmigaOS Intuition** |
|---|---|---|---|
| Who owns the screen | the X server | the compositor (the server *is* the window manager) | Intuition + graphics.library (layers) |
| Who draws window contents | the server, from the client's requests | the client, into its own buffer | the program itself, through graphics.library, straight into the screen's bitplanes, clipped by layers |
| Hidden parts of windows | lost; the client gets *Expose* and redraws (or the server keeps "backing store") | every window is a whole buffer; nothing is ever lost | *smart refresh*: the hidden pieces are kept in off-screen bitmaps; *simple refresh*: lost, the program redraws |
| Frames and title bars | a separate window manager process draws them | the client (or the compositor) | Intuition |
| Placement, focus | the window manager | the compositor | Intuition (fixed rules) |
| Changing the desktop | swap the window manager | swap the whole compositor (and with it the server) | not possible (Workbench replacements change icons, not windows) |

### 3.2 The choice

**X11's split, Intuition's drawing.** `displayd` owns the hardware and draws; `desk` is the window manager.

- **Not Wayland's composition.** Composition means every window draws into its own picture and the server copies the
  visible parts to the screen: every change is drawn twice, and every window needs a whole picture in chip RAM (the
  blitter only reaches chip RAM). On a 14 MHz 68020 the second copy is the cost that matters: a full 4-plane screen
  copy is 23 ms of blitter time (section 6), more than a frame. And swapping the desktop would mean swapping the
  server, which is what the owner's "several desktop choices" should not require.
- **Not AmigaOS's "programs draw on the screen themselves".** It is the fastest (no message per drawing), and Zenix
  has one address space too, but: the blitter would be shared by every program (a program stopped by the scheduler
  half-way through setting the blitter's registers spoils the next one's blit; AmigaOS avoids that with
  `OwnBlitter`/`LockLayers`, and a program that dies holding them freezes the screen). With no MMU, Zenix's edge is
  that a crashing program takes nothing else down; a dead client of `displayd` leaves no lock behind.
- **So: the server draws, the client sends orders in batches.** The client library collects drawing orders in a buffer
  in the client's own memory and sends one message for the whole batch (at the end of an event's handling, or when
  the buffer is full). The server reads the buffer in place, as `fsd` reads a `write()`'s data: no copy. One message
  round trip per batch, not per rectangle.
- **Smart refresh by default, simple refresh as the fallback.** The server keeps the hidden pieces of each window in
  chip RAM (Intuition's smart refresh), so raising, lowering and moving never ask a program to draw again. When chip
  RAM is short (a budget, section 6) a window becomes simple refresh: its hidden parts are dropped and the program gets
  an *expose* event naming the rectangles to draw again. Every client must handle expose (the library makes that easy:
  section 5), so the fallback always works.
- **Window contents start on an 8-pixel column.** A window's inside moves in steps of 8 pixels sideways (1/80 of the
  screen; up and down in single lines). Text is then always byte-aligned: the server draws glyphs with the CPU a byte
  per plane, the console's fast path (about 266 instructions a character at 3 planes, architecture.md section 11),
  and window copies need no shifting. The frame around it can be any width.

### 3.3 Who draws frames, who decides

- **`desk` decides; `displayd` draws.** `desk` gives the server a *frame style* once (border widths, title-bar height,
  the colours of an active and an inactive window, the gadget pictures, the title font, the shadow). The server draws
  every frame from it, so a frame is redrawn during a drag without a message to `desk`. A tiling desktop sets a style
  with a one-pixel border and no title bar.
- **Placement, stacking, focus, minimise, the meaning of every gadget: `desk`.** A new window is a *request* the server
  passes to `desk`; `desk` answers where it goes and how big. A click on a frame goes to `desk`, which may answer
  "move it", "resize it", "close it" (the program gets a close request; it decides), "minimise it" (the server hides
  it; `desk` lists it in the bar).
- **Drags run in the server.** On "move it", the server follows the mouse itself until the button is released and tells
  `desk` the end: no message per mouse step. Outline or whole window (section 6.3), as the frame style says.
- **Keys:** `desk` names the key combinations it wants (its shortcuts, from its configuration file); the server sends
  those to `desk` and every other key to the window with the focus. Amiga+F1..F10 stay with the kernel (console
  switches) and never reach the desktop.
- **Without a desktop running** (not started yet, or crashed) the server still works: new windows are placed one below
  the other, a click raises and focuses, frames use a built-in plain style. Starting `desk` again takes the windows
  over, as an X window manager does.
- **A second desktop** (the i3-style tiling one) is another program speaking the same window-manager messages: it
  places windows in a grid, sets the thin style, hides the windows of other workspaces, runs its own bar. Programs
  never know which desktop is running. Only one window manager at a time (the server refuses a second).

### 3.4 The kernel's part

The kernel keeps the text consoles and gives one console's screen to `displayd`, as Linux gives a console to X:
- **A console in graphics mode.** `displayd` asks for the console it was started on; while that console is shown,
  the Copper runs `displayd`'s Copper list and its bitplanes, the kernel draws nothing and keys go to `displayd`.
- **Switching away waits for the server's yes** (Linux's `VT_PROCESS`). On Amiga+F*n* the kernel asks `displayd` to let
  go; `displayd` finishes the blit in progress, stops using the blitter and answers; then the kernel shows the text
  console. Back again, the kernel tells `displayd`, which may use the blitter again (its bitplanes were untouched, so
  nothing needs redrawing). No answer within 1 s: the kernel switches anyway and logs it; a server that ignores this
  is a bug, not a hang.
- **Input:** while the desktop is shown, `kbd_work` puts each key into a ring `displayd` reads (raw code, qualifiers,
  and the characters the keymap gives, so the Swedish keymap stays in one place), and the mouse's movement and
  buttons likewise; an event bit wakes `displayd` (D16), as `netd` is woken.
- **The pointer** is hardware sprite 0, so it costs no screen colours (on AGA the sprites' colours are moved outside the
  16 with `BPLCON4`) and no drawing. The kernel moves it with the mouse on its 100 Hz poll, two words in chip RAM, so
  the pointer never freezes while `displayd` or a disk transfer is busy. *For the review:* this writes in an
  interrupt, which the code rules allow only for "take the byte"; it is two position words, not drawing, but it needs
  the reviewers' yes, else it moves in `kwork` (at most a tick later, frozen during a long system call).
- **Chip RAM** for the screen and the hidden pieces is allocated by `displayd` (`SYS_memalloc` with chip RAM), so it is
  freed when the server ends; the kernel takes its console screen back.

## 4. The protocol

### 4.1 Shape

- **One port, `display`**, created by `displayd` (a service, D11, started on demand by the first `desk` or program).
- **Client to server:** synchronous calls (the existing `sys_call`), each answering at once; the server never waits
  for a client.
- **Server to client:** events go into a ring in the client's own memory (registered at hello) and set an event bit
  the client waits on with `poll` (D16's `POLLPORT` family); the client never misses one, and a slow client never
  blocks the server (a full ring sets an "events lost" flag; the client then asks for its windows' state again, and
  mouse motion is merged first).
- **Versioned:** the first message, `HELLO`, carries the protocol version and the client's name; the server answers
  its own version. Messages only grow at the end and never change meaning (frozen-interfaces.md, once built).
- **Numbers:** message types `DPY_*` on port `display`, claimed in `docs/claims.md` in the first step's first commit.

### 4.2 Messages (version 1, names only; sizes and numbers in the first step)

| From the program | What |
|---|---|
| `HELLO` | version, name, the event ring |
| `WIN_NEW` / `WIN_SET` / `WIN_CLOSE` | create a window (wanted size in pixels or in cells of a font, minimum and maximum size, title, kind: normal, dialog, bar, desktop), change title or wanted size, destroy |
| `DRAW` | one batch of drawing orders in the program's memory (below), for one window |
| `FONT` | open a font by name and size: answers its cell size and an id |
| `IMAGE` | register a picture in the program's memory (planar or 4-bit chunky) to be drawn by id |
| `PALETTE` | the screen's colours (read only; a look sets them through `desk`) |

**Drawing orders** inside a `DRAW` batch, each a few words: fill a rectangle; a line across or down; text (a font, two
colours, UTF-8 at a pixel or a cell; bold, underline and inverse as the console does them); copy a rectangle inside
the window (scrolling); draw an image or part of one; set a clip rectangle. Coordinates are window-relative; the
server clips everything to the window and to what is visible, and checks every length and offset (untrusted, as a
disk block).

| Events (server to program) | What |
|---|---|
| `KEY` | raw code, qualifiers, the characters |
| `BUTTON` / `MOTION` | window-relative position; motion only while asked for (a button held, or the window asked) |
| `RESIZE` | the new inside size (in cells too, for a terminal) |
| `EXPOSE` | rectangles to draw again (simple refresh, a resize, a lost hidden piece) |
| `FOCUS` | in or out (a terminal draws its cursor hollow) |
| `CLOSE` / `MINIMISED` | the close gadget was used (the program decides); shown or hidden |

| Window manager (only `desk`) | What |
|---|---|
| `WM_HELLO` | become the window manager (one at a time; the session's user or root) |
| `WM_STYLE` | the frame style (3.3) |
| `WM_PLACE`, `WM_STACK`, `WM_FOCUS`, `WM_SHOW`/`WM_HIDE` | answer a new window's request, raise or lower, give the keyboard, minimise or a workspace |
| `WM_GRAB` | the key combinations `desk` handles |
| `WM_DRAG` | start a move or resize run by the server (outline or whole), reports the end |
| events to `desk` | a window wants to appear, a click on a frame, a grabbed key, a window ended |

**Later, on the same port:** the clipboard (text first), drag and drop, an icon for a minimised window, more screen
modes. None is in version 1.

## 5. The client library and the terminal

- **`libdisplay`** (header `display.h`): open the display, open windows, queue drawing orders, `dpy_flush`, wait for
  events (`dpy_next`, or its port in the program's own `poll` loop). Small (estimate 4-6 KB of code linked into each
  program), static first, a shared library when D13 stage 2 exists. It keeps nothing hidden: a program that wants
  expose to be easy keeps its own model and draws it from scratch on expose (the terminal keeps its grid anyway).
  Widgets (buttons, lists) are a later, separate library; the first step needs only the About window's one button,
  drawn by hand.
- **`term`, the terminal window.** Opens a window sized in cells (80x24 by default, the user's resize changes the
  cells and sends `SIGWINCH`), opens a pseudo-terminal, starts the user's shell on it, and runs the same ANSI/VT
  parser as the console. **One parser, two users:** the kernel's `term.c` escape parser and cell model are split from
  its drawing so the same source is built into the kernel and into `term` (D13's rule: no copies); `term` draws its
  cells through `libdisplay` (runs of one colour pair as one text order). Scrollback 200 lines as the console's.
  Colours: the 16 ANSI colours map to the look's palette by the nearest colour (as the console maps xterm's 256); in
  the two-colour look colour shows as bold. Fonts: Topaz 8 and the ZCF supplement, the console's, held once in the
  server; TrueType sizes later (rendered once into bitmap fonts and cached, ROADMAP block 5).
- **Pseudo-terminals come first**: D20 section 7.2 is their design (two reviews, a deep core change: the tty layer).
  `term` cannot exist without them, and `telnetd`/`sshd` later reuse them.
- **The console stays for text mode.** Nothing of the desktop goes into the text consoles (owner 2026-10-23).
  Full-screen text programs (Sumi, Maki, Ahiru, `ctop`) run in `term` as on a console.

## 6. Memory and speed on the minimum machine

The minimum (owner 2026-11-04): a stock A1200, 68EC020 at 14 MHz, AGA, 2 MB chip RAM, 4 MB fast RAM. **All sizes
and times below are estimates** (sizes guessed from comparable Zenix programs; times from the Hardware Reference
Manual's blitter formula, "n x H x W / 7.09" microseconds on PAL, without bus contention). Measured on the real
A1200 in the first step, never on an emulator.

### 6.1 Depth

| Depth | Colours | Chip RAM for 640x256 | Display DMA | Use |
|---|---|---|---|---|
| 1 plane | 2 | 20 KB | 40 slots a line | look (c); any machine |
| 3 planes (OCS/ECS) | 8 | 60 KB | half the CPU's slots in visible lines | OCS/ECS: each look has an 8-colour palette too |
| **4 planes, FMODE 3 (AGA)** | **16** | **80 KB** | **40 slots a line, as one plane** | **the default on AGA** (D14's reasoning) |
| 8 planes (AGA) | 256 | 160 KB | 80 slots a line | not now: twice the blitter work for every move |

16 colours are the sweet spot: the same DMA cost as one plane on AGA, enough for a look and the terminal's colours
together (8 for the look, 8 near the ANSI colours; bright ANSI colours map to the nearest), and every blit is 4 planes
rather than 8. 640x512 interlaced (160 KB, flicker on a TV-type monitor) is a later setting; NTSC's 640x200 must fit
every layout (the desk places windows by the screen's real height).

### 6.2 Memory (estimates)

| Part | Fast RAM | Chip RAM |
|---|---|---|
| `displayd`: code (drawing, clipping, fonts, protocol) | 28-36 KB | - |
| `displayd`: data (windows, clip lists, fonts' tables) | 10-20 KB | - |
| the screen (4 planes), Copper list, sprite | - | 81 KB |
| hidden pieces of windows (smart refresh), a budget | - | up to 256 KB (then simple refresh) |
| `desk` (window manager, bar, menu, icons, config) | 25-35 KB | icons' images: 2-4 KB |
| `term`, one window (code shared by D13 when two run) | 18-24 KB code, 8 KB grid, 32 KB scrollback once used | - |
| a pseudo-terminal | 3.5 KB (D20 7.2) | - |
| `libdisplay` in each program | 4-6 KB | - |
| **A desktop with two terminals** | **about 180-230 KB** | **about 90-340 KB** |

The 4 MB fast RAM holds all of it with room to spare; chip RAM keeps 1.6 MB or more for the rest (ZINE, sound, disk
buffers). On a 2 MB chip-only A1200 (best effort) the desktop runs at 3 planes or look (c)'s one plane, with a smaller
hidden-pieces budget.

### 6.3 Speed (estimates from the formula; to be measured)

| Action | Blitter work | Time |
|---|---|---|
| copy a terminal window of 50x14 cells (408x130 pixels, 4 planes) | 13,520 words, A->D, 4 ticks | 7.6 ms (11.4 ms with masked edges) |
| scroll that terminal one line | 10,400 words | 5.9 ms |
| copy the whole screen, 4 planes | 40,960 words | 23 ms |
| the same at 1 plane (look c) | 10,240 words | 5.8 ms |
| redraw that terminal's text with the CPU (no smart refresh) | - | about 100 ms (the console's 11 ms a 79-character line, emulator and PiStorm figures) |

- **Raising or moving a window** is a few copies of that size: one or two frames (20-40 ms) for a mid-size window on the
  stock A1200, at once on the 060. The CPU runs on while the blitter works.
- **Text is the slow part,** which is why hidden pieces are kept: a window brought to the front is copied back, not
  drawn again.
- **Dragging:** on a 68020/68030 the server drags an outline (XOR lines of the frame: almost free) and moves the window
  once at the release, as Workbench does; on a 68040/060 and the PiStorm it moves the whole window as it drags. A
  setting picks either; the default by CPU is decided by measuring on the real A1200 (question 3).
- **Damage, not redraw:** the server keeps a list of dirty rectangles per frame and touches only those; a clock
  ticking in the bar is one small text order.

## 7. The first step

**Built in this order**, each part merged with its tests before the next:
1. **Pseudo-terminals** (D20 7.2, with its reviews).
2. **The kernel's graphics console:** a console handed to `displayd`, the switch with its answer, the input ring,
   the pointer sprite. Two reviews (it touches the display, the keyboard and the tick).
3. **`displayd`:** the screen, windows with clipping, smart and simple refresh, drawing orders, fonts (Topaz from the
   ROM and the ZCF supplement), frames from a style, drags, the fallback without a window manager, protocol version 1.
4. **`libdisplay`.**
5. **`desk`:** move, resize, raise, close, minimise; click to focus; the **top bar** (a menu under the Zenix name with
   the programs and About/Log out, a list of the windows with the minimised ones dimmed, memory and a clock); a few
   **desktop icons** that start programs (Shell, Files, Prefs as listed in its config; Files starts Ahiru in a terminal
   until a graphical file view exists); the **About window**; **keyboard shortcuts**, all configurable in
   `/etc/desk/keys` (a user's own file overrides it): Amiga+Return a terminal, Amiga+Q close, Amiga+M minimise,
   Amiga+Tab next window, Amiga+Space the menu, Amiga+arrows move the focused window, Amiga+Shift+arrows resize it
   (Amiga+F1..F10 remain the consoles). The look chosen from section 8, with its 16- and 8-colour palettes.
6. **`term`:** the terminal window (section 5).

**Starting it:** log in on a console and type `desk` (it starts `displayd` if needed). Leaving `desk` (the menu's Log
out, or Ctrl-C at its console) closes the desktop and gives the console back.

**Tests:** the clipping, the hidden pieces and the protocol's checks on the host (as `term-test.sh` tests the console);
screenshots in FS-UAE compared with expected pixels (as `colour-test.sh`); a fuzz of drawing batches (no crash, no
write outside the screen). Speed only on the real A1200 (the stock one and the 1260), written down with the date.

**Later, not in the first step:** workspaces and the i3-style tiling desktop; a graphical file view (Ahiru-style);
viewers (pictures, text); a preferences program (looks, keys, screen mode); TrueType fonts; the clipboard and drag and
drop; menus of each program in the bar; a type-to-search launcher (FuzzyFind); 640x512 and RTG screens; sound events;
the demo floppy's desktop edition (D25, postponed). Each its own approval.

## 8. The looks to choose from

Each picture is the same scene drawn as the Amiga would show it: 640x256 pixels in at most 16 colours, with an 8x8
Topaz-style font, the pixels shown twice as tall as wide (as on the screen) and doubled for viewing. A terminal running
the shell is the active window; behind it a file list (the later file view) and a small About window; the top bar
with the menu, the window list (`sumi` dimmed: minimised) and the clock; four desktop icons; the pointer on a title bar.

| Look | Picture | In one line |
|---|---|---|
| **(a) Flat, with a hint of depth** | [a-flat.png](desktop-looks/a-flat.png) | Plain colours on a slate desktop, a thin dark border and a one-pixel shadow, a blue title on the active window: modern and calm, the favourite so far. |
| **(b) Workbench, cleaner** | [b-workbench.png](desktop-looks/b-workbench.png) | Workbench 3's grey and blue with raised edges, the close gadget on the left and the depth gadget on the right, without the stripes and clutter. |
| **(c) Two-tone** | [c-mono.png](desktop-looks/c-mono.png) | Black and white like the first Macintosh and the Atari ST: a dithered desktop, a striped active title, a drop shadow; runs at one bitplane, so four times less drawing work. |
| **(d) Dark neon** | [d-cyber.png](desktop-looks/d-cyber.png) | Near-black with a faint grid, a neon magenta title and a cyan border with a glow on the active window, the text still plain and readable. |
| **(e) Platinum** (asked for 2026-11-04: "What about a OS7-8-9 look?") | [e-platinum.png](desktop-looks/e-platinum.png) | Mac OS 8-9's Platinum in Amiga pixels: light grey bevelled windows, a pinstriped active title centred, the close box left, zoom and collapse right, a dotted blue desktop, the default button ringed. |

The pictures at the real 640x256 size (pixel for pixel what the Amiga shows) are next to them, `*-native.png`. They are
made by `tools/desktop-mockups.py` (Pillow), which reads a free re-creation of the Topaz font (TrueSchool's
`Topaz_a1200`, GPL with the font exception) given on its command line, never the Kickstart ROM's Topaz and never a
copy in the repository; without it the PC font in `tools/font_8x8.c` is used. Every look is a frame style and a
palette for `desk`, so the ones not chosen stay cheap to offer later as themes.

## 9. Questions for the owner (each with my default)

1. **Which look?** Answered 2026-11-04: **(a) the default, (b) and (e) as themes** ("A as default, B and E as themes").
2. **How does the desktop start?** Default: typed `desk` after logging in on a console (like `startx`); starting it at
   boot is a later setting.
3. **Dragging on the stock A1200: the outline or the whole window?** Default: the outline on 68020/030, the whole window
   on 68040/060 and the PiStorm; a setting changes it.
4. **Close gadget left (Amiga) or right (modern)?** Default: as the look draws it ((a) and (d) right, (b) and (c) left).
5. **Menus: a left click on the bar, or the right mouse button as on Workbench?** Default: both: a left click opens
   one; holding the right button shows them the Amiga way.
6. **The shortcut key: the left Amiga key?** Default: yes (Amiga+Return a terminal, and section 7's list), all
   configurable; Amiga+F1..F10 stay the console switches.
7. **Minimised windows: in the top bar's window list, or as icons on the desktop (Workbench's iconify)?** Default: the
   bar, as in the pictures.
8. **Desktop icons: a few fixed starters, as in the pictures?** Default: yes, listed in a config file; no file icons
   (`.info`) on the desktop for now.
9. **Names:** `displayd` (the server), `desk` (the floating desktop), `term` (the terminal window). Default: these.
10. **OCS/ECS and chip-only machines:** default: the chosen look in 8 colours; look (c) as the choice for the slowest.

---
Revision 1, 2026-11-04: written for the owner's choice; proposed, nothing built, not reviewed. The blitter times are
computed from the Hardware Reference Manual (3rd edition, "Blitter Speed"), the other figures are estimates.

## Decided 2026-11-04 (revision 2): the bar and workspaces

**The bar** (owner: "Will it work as a workbench bar, a macOS bar, i3 bar? How would we design it?"; the design below
put to the owner and approved with "go ahead and start building"): one bar, drawn by `desk`, in three zones, each
made of modules named in `/etc/desk/bar` (a user's `~/.config/desk/bar` wins):
- **left, menus:** the Zenix menu, then the active program's menus. Programs send their menus to `displayd` (a menu
  message in the protocol); the bar draws them. Opened by a click (the Mac's way), by holding the right mouse button
  (Workbench's way), or both (the default). Theme (b) defaults to the right button and Workbench's look of the screen
  title bar, theme (e) to the click.
- **middle:** a window list (the default; minimised windows dimmed) or the workspaces' numbers (i3's way).
- **right, status blocks:** clock, memory, network, CPU built in; any block may instead be a command whose one line of
  output is shown, run every N seconds (i3blocks' idea), N 10 s by default and never under 1 s, so a 68020 is not
  kept busy.
A theme ships its own default bar file; the modules and the ways of opening menus are the same in every theme.

**Workspaces** (owner: "Why can't the floating desktop have workspaces?"): the floating desktop has them from the
first step. They belong to `desk`, not to a layout, so the tiling desktop later uses the same. Nine workspaces, Left
Amiga + 1..9 to switch, Left Amiga + Shift + 1..9 to move the active window there (all in `/etc/desk/keys`, Amiga +
F1..F10 stay the console switches). A window on a hidden workspace gives up its kept off-screen copy (chip RAM) and is
redrawn when shown again (about 0.1 s a window on a 14 MHz 68020, an estimate). Later, not in the first step: a
workspace as an Amiga-style screen of its own (its own mode and palette, dragged down to see the one behind).

## Decided 2026-11-04 (revision 3): a graphical login

Owner: "It would be nice to have the ability to let Zenix boot and present the user with a graphical login screen,
and when entering the password the user is put into the desktop, just like a modern Linux machine. Just because the
hardware is from the 90's the design or functionality of zenix doesn't have to be."

So, after `desk` works (stage 7 of the first step): a login manager, `dlogin` (as GDM, LightDM or SDDM), a D11
service that starts `displayd` on one console at boot and shows a login screen in the chosen theme: the computer's
name, the user's name, the password (never echoed), the clock, and a choice of session (the floating desktop now,
the tiling one later, or a text console). It checks the password the way `login` does (one code, the shadow file's
rules, the same delays after a wrong password) and then starts the session as that user, never as root; logging out
brings the login screen back. The text consoles stay on Amiga + F1..F10 as now. Whether a machine boots to the
graphical login or to the text login is one line in `/etc` (the default: text until the desktop is proven on real
hardware, then graphical; the installer asks). Details, as for the rest of D38, are decided in the code's review: who
owns `displayd` between sessions, the greeter's own user, how the hand-over is guarded.

