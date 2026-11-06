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
  keyboard while the desktop is shown, and draws everything. A *desktop* (`zdesk`) is an ordinary program that decides
  where windows go, which one has the keyboard, what the frames look like, and runs the top bar. A tiling desktop
  later is just another program in `zdesk`'s place; the windows and the programs in them do not change.
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
  lives on one of the consoles, like X on Linux: you log in and type `zdesk`.
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

**X11's split, Intuition's drawing.** `displayd` owns the hardware and draws; `zdesk` is the window manager.

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

- **`zdesk` decides; `displayd` draws.** `zdesk` gives the server a *frame style* once (border widths, title-bar height,
  the colours of an active and an inactive window, the gadget pictures, the title font, the shadow). The server draws
  every frame from it, so a frame is redrawn during a drag without a message to `zdesk`. A tiling desktop sets a style
  with a one-pixel border and no title bar.
- **Placement, stacking, focus, minimise, the meaning of every gadget: `zdesk`.** A new window is a *request* the server
  passes to `zdesk`; `zdesk` answers where it goes and how big. A click on a frame goes to `zdesk`, which may answer
  "move it", "resize it", "close it" (the program gets a close request; it decides), "minimise it" (the server hides
  it; `zdesk` lists it in the bar).
- **Drags run in the server.** On "move it", the server follows the mouse itself until the button is released and tells
  `zdesk` the end: no message per mouse step. Outline or whole window (section 6.3), as the frame style says.
- **Keys:** `zdesk` names the key combinations it wants (its shortcuts, from its configuration file); the server sends
  those to `zdesk` and every other key to the window with the focus. Amiga+F1..F10 stay with the kernel (console
  switches) and never reach the desktop.
- **Without a desktop running** (not started yet, or crashed) the server still works: new windows are placed one below
  the other, a click raises and focuses, frames use a built-in plain style. Starting `zdesk` again takes the windows
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

- **One port, `display`**, created by `displayd` (a service, D11, started on demand by the first `zdesk` or program).
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
| `PALETTE` | the screen's colours (read only; a look sets them through `zdesk`) |

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

| Window manager (only `zdesk`) | What |
|---|---|
| `WM_HELLO` | become the window manager (one at a time; the session's user or root) |
| `WM_STYLE` | the frame style (3.3) |
| `WM_PLACE`, `WM_STACK`, `WM_FOCUS`, `WM_SHOW`/`WM_HIDE` | answer a new window's request, raise or lower, give the keyboard, minimise or a workspace |
| `WM_GRAB` | the key combinations `zdesk` handles |
| `WM_DRAG` | start a move or resize run by the server (outline or whole), reports the end |
| events to `zdesk` | a window wants to appear, a click on a frame, a grabbed key, a window ended |

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
| `zdesk` (window manager, bar, menu, icons, config) | 25-35 KB | icons' images: 2-4 KB |
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
5. **`zdesk`:** move, resize, raise, close, minimise; click to focus; the **top bar** (a menu under the Zenix name with
   the programs and About/Log out, a list of the windows with the minimised ones dimmed, memory and a clock); a few
   **desktop icons** that start programs (Shell, Files, Prefs as listed in its config; Files starts Ahiru in a terminal
   until a graphical file view exists); the **About window**; **keyboard shortcuts**, all configurable in
   `/etc/zdesk/keys` (a user's own file overrides it): Amiga+Return a terminal, Amiga+Q close, Amiga+M minimise,
   Amiga+Tab next window, Amiga+Space the menu, Amiga+arrows move the focused window, Amiga+Shift+arrows resize it
   (Amiga+F1..F10 remain the consoles). The look chosen from section 8, with its 16- and 8-colour palettes.
6. **`term`:** the terminal window (section 5).

**Starting it:** log in on a console and type `zdesk` (it starts `displayd` if needed). Leaving `zdesk` (the menu's Log
out, or Ctrl-C at its console) closes the desktop and gives the console back.

**Tests:** the clipping, the hidden pieces and the protocol's checks on the host (as `term-test.sh` tests the console);
screenshots in FS-UAE compared with expected pixels (as `colour-test.sh`); a fuzz of drawing batches (no crash, no
write outside the screen). Speed only on the real A1200 (the stock one and the 1260), written down with the date.

**Later, not in the first step:** workspaces and the i3-style tiling desktop; a graphical file view (Ahiru-style);
viewers (pictures, text); a preferences program (looks, keys, screen mode); TrueType fonts; the clipboard and drag and
drop; menus of each program in the bar; a type-to-search launcher (FuzzyFind); 640x512 and RTG screens; sound events;
the demo floppy's desktop edition (approved 2026-11-04: D25 revision 3, Part 1). Each its own approval.

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
palette for `zdesk`, so the ones not chosen stay cheap to offer later as themes.

## 9. Questions for the owner (each with my default)

1. **Which look?** Answered 2026-11-04: **(a) the default, (b) and (e) as themes** ("A as default, B and E as themes").
2. **How does the desktop start?** Default: typed `zdesk` after logging in on a console (like `startx`); starting it at
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
9. **Names:** `displayd` (the server), `zdesk` (the floating desktop), `term` (the terminal window). Default: these.
10. **OCS/ECS and chip-only machines:** default: the chosen look in 8 colours; look (c) as the choice for the slowest.

---
Revision 1, 2026-11-04: written for the owner's choice; proposed, nothing built, not reviewed. The blitter times are
computed from the Hardware Reference Manual (3rd edition, "Blitter Speed"), the other figures are estimates.

## Decided 2026-11-04 (revision 2): the bar and workspaces

**The bar** (owner: "Will it work as a workbench bar, a macOS bar, i3 bar? How would we design it?"; the design below
put to the owner and approved with "go ahead and start building"): one bar, drawn by `zdesk`, in three zones, each
made of modules named in `/etc/zdesk/bar` (a user's `~/.config/zdesk/bar` wins):
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
first step. They belong to `zdesk`, not to a layout, so the tiling desktop later uses the same. Nine workspaces, Left
Amiga + 1..9 to switch, Left Amiga + Shift + 1..9 to move the active window there (all in `/etc/zdesk/keys`, Amiga +
F1..F10 stay the console switches). A window on a hidden workspace gives up its kept off-screen copy (chip RAM) and is
redrawn when shown again (about 0.1 s a window on a 14 MHz 68020, an estimate). Later, not in the first step: a
workspace as an Amiga-style screen of its own (its own mode and palette, dragged down to see the one behind).

## Decided 2026-11-04 (revision 3): a graphical login

Owner: "It would be nice to have the ability to let Zenix boot and present the user with a graphical login screen,
and when entering the password the user is put into the desktop, just like a modern Linux machine. Just because the
hardware is from the 90's the design or functionality of zenix doesn't have to be."

So, after `zdesk` works (stage 7 of the first step): a login manager, `dlogin` (as GDM, LightDM or SDDM), a D11
service that starts `displayd` on one console at boot and shows a login screen in the chosen theme: the computer's
name, the user's name, the password (never echoed), the clock, and a choice of session (the floating desktop now,
the tiling one later, or a text console). It checks the password the way `login` does (one code, the shadow file's
rules, the same delays after a wrong password) and then starts the session as that user, never as root; logging out
brings the login screen back. The text consoles stay on Amiga + F1..F10 as now. Whether a machine boots to the
graphical login or to the text login is one line in `/etc` (the default: text until the desktop is proven on real
hardware, then graphical; the installer asks). Details, as for the rest of D38, are decided in the code's review: who
owns `displayd` between sessions, the greeter's own user, how the hand-over is guarded.

## Decided 2026-11-04 (revision 4): the name

Owner: "Zenix Desktop is a good and clear name. zdesk/ztile are good commands. It's decided." The desktop is
**Zenix Desktop**; the floating window manager's program is `zdesk` (it was `desk` above), the tiling one later
`ztile`; their settings live in `/etc/zdesk/` (and `~/.config/zdesk/`), the tiling one's in `/etc/ztile/`.
`displayd`, `term` and `dlogin` keep their names.

## The bar it must clear (owner 2026-11-04)

"Remember that Workbench 1.3 didn't have artifacts and felt snappy on an A500. Just saying. I have a 060/50." So:
no visible artifact ever (no clear-then-draw, no flash, nothing drawn twice, nothing left behind), only what changed is
drawn, and every interaction (the pointer, a menu, a raise, a drag, typing in term) answers within the frame on a
68060 and feels as quick as Workbench 1.3 on an A500 on the stock A1200. Measured on the real machine; a finding of
either kind is a bug, fixed before new desktop features.


## The A1200's first findings (2026-11-04, release 2026110403)

Zenix Desktop ran on the owner's A1200 with the Apollo 1260 (68060 at 50 MHz, the data cache on, AGA, PAL). The
owner's partner: "this is amazing". What they found, and what was done (branch desk; verified in FS-UAE, not yet on
the A1200 again):

- **The pointer lagged and the bottom right was hard to reach.** The kernel now reads the mouse's counters in the
  vertical blank and moves sprite 0 there, before sprite DMA reads it (it waited for the 100 Hz tick before), places
  it to the hires pixel on AGA (SPRxCTL SH1: the hot spot reaches column 639), and accelerates as AmigaOS's Input
  prefs do (`CON_MOUSE`; zdesk's `/etc/zdesk/mouse`: none, low (the default: the counts beyond 4 in a frame doubled),
  high (beyond 2, tripled)).
- **term could not be made narrower.** The console's cell model takes its width in use at run time (`WCOLS`, the
  console's own stays 80): term is 20 to 80 columns, and the program gets SIGWINCH with the new width.
- **term was slow:** `time ls -laR /` (about 5,000 lines, the owner's AmigaOS partitions mounted) took 12.8 s on the
  text console (4.36 s to /dev/null) and 2 min 51.8 s in term (4.38 s to /dev/null): term's drawing cost about 167 s
  against the console's 8.4 s. Every line scrolled the window with a copy of the whole inside, and every glyph was
  drawn by a per-pixel loop. Now term shows output as xterm's jump scroll does (the cells change at once, the window
  shows them at most every 30 ms while the output streams: one copy for all the lines scrolled, then the cells that
  changed), a copy of whole words is one blit of every plane (B to D, half the bus slots), and a glyph on an 8-pixel
  column is a byte a plane a line, and a run of blanks is a fill. The PiStorm (a far faster CPU) gave the same
  minutes (2 min 43 s; console 10.3 s, /dev/null 1.46 s): about 33 ms a line, the whole window's blitter copy. And the
  CPU's part: term's cell model moved its grid on every scroll with libz's byte-by-byte memmove (now long words, as
  the kernel's). term now reads the pty in big pieces (a tick's nap after a small read while output streams, so a
  writer of a line a write does not wake it a line at a time) and works through them 256 bytes at a time, showing
  what changed between pieces at most a quarter of the time (never still for more than 0.4 s). The target: within
  about 1.5 times the console. `tools/zdesk-draw-test.sh`, FS-UAE's stock A1200 with 4 MB fast (emulator figures):
  `time seq 1 2000` 8.8 s on the console, in term 20.9 s before, 6.6 s now (21 showings).
- **A menu hid the windows beside it** (the whole band from the bar to the menu's bottom, across the screen): a
  `WK_BAR` window was always made the screen's width. A bar window now has the width asked (0: the screen's).
- **The menu flickered** when the highlight moved: it was cleared and drawn whole. Now the two items that change are
  drawn, every pixel once, and the menu is drawn whole once, when it opens.
- **The workbench theme's close gadget covered the title's first letter**: titles now start after the left gadgets
  and a gap and end before the right ones (a centred one too).
- **The About window's body showed as a pink and green checker in the flat theme** (plain grey in workbench). Not
  found in FS-UAE; open (see the hand-back).
- The bar said 68040 on the 68060 (ctop and About "68040+FPU" on the PiStorm): AttnFlags' 68060 bit is set only by
  AmigaOS's 68060.library. The kernel now sets it on a CPU with a PCR, and `ZINE_INFO` says Emu68 (`machine`): zdesk
  shows "68060" or "Emu68 68040". The bar's memory reads as `free -h` does ("fast 1.7G", not "fast 1801.7M").
- ctop showed 0.0% for every process: the kernel does not count CPU time per process yet (noted; not the desktop's).

And the owner's bar above: so a pass over displayd and zdesk for drawing that is not minimal - the bar is drawn
left to right with every pixel once (no clearing first) and a changed status block only redraws its letters; an
icon's selection redraws its two labels, not the desktop; a frame's title bar is drawn beside the title, not under
it. The server counts what it draws (`DPY_STAT`: blits, pixels, glyphs, batches, damage painted, copies onto the
screen) and the tests hold those numbers small.

## As built: dlogin (revision 3, 2026-11-04)

The details revision 3 left to the review, as built (branch desk, verified in FS-UAE by `tools/dlogin-test.sh`):

- **How it starts:** not a D11 service (a service has no console, and a port's first call is not a boot) but
  console 1's login: `/etc/console`'s `login graphical` makes init start `/sbin/dlogin --init` there instead of
  `login --init` (text stays the default; no line, or no `/sbin/dlogin`, is the text login). dlogin is then console
  1's session leader, so the display server takes that console; without a display (or with a desktop already up on
  another console: `DPY_SESSION` answers -EBUSY to another console's caller) it hands the console to the text login
  (`login --init`, handed to init as login hands its shell). init starts the text login itself when dlogin cannot be
  started or ends three times within 30 s of starting (until the next boot).
- **Who owns displayd between sessions:** dlogin. It stays connected during a session (displayd ends only with its
  last client), so the screen never goes back to text between the greeter and the desktop.
- **The greeter's user:** root (it must start other users' sessions). It shows only its own window, reads only the
  keys of its own window, and starts nothing but login.
- **The hand-over's guard:** `DPY_SESSION` (root only) names the uid whose programs may connect; on every change
  the clients of any other non-root uid are dropped, so a program left from the last session cannot draw a fake
  greeter. dlogin leaves the manager's role (`DPY_WM_BYE`) and closes its window before the session's zdesk starts,
  so zdesk's Log out never touches dlogin.
- **The session:** `login -f -s PROGRAM NAME` (root only), so login's code sets it up; root cannot log in on the
  greeter, whatever the password (a text console is root's). The password check is login's (`pw_auth`: an unknown
  name takes as long as a wrong password of a new hash, a second's wait after a wrong one). The last session's
  programs are cut off from the screen, not ended (and without an MMU memory is readable by any program). The Text console session closes the display and runs the
  user's shell on console 1; its end brings the greeter back.
- Not yet: autologin, a user list, shutdown and reboot buttons, a session chosen per user; the clock is UTC.

## The A1200's finding of 2026-11-05: a dragged window tore

The owner's PiStorm A1200 (the demo floppy; Emu68 says 68040, so zdesk's `drag auto` moved the whole window): the
window showed at two places at once mid-move, its top at the new place and its bottom at the old, the text smeared.
And Ahiru, Sumi and ctop did not follow a terminal window's resize. What was found and done (branch drag, verified in
FS-UAE, not yet on the A1200):

- **The cause:** a move painted the damage (the old and the new place together) straight onto the screen on view, back
  to front in several passes (the background, the frames, the insides from their kept copies), and before that took a
  fresh copy of the moved window's inside (chip RAM allocated and cleared, the inside copied out, by the CPU when its
  column was not on a 16-pixel word) and freed it after. That took more than a frame, so the raster showed whatever
  was half done when it passed: a torn frame, on any CPU, since the blitter's speed sets the time. A move was made for
  every mouse event, however far behind the drawing was.
- **Now (`display/server/draw.c`, `win.c`, `displayd.c`):** a whole window's drag draws into a second screen in chip
  RAM (a copy of the screen taken when the drag starts: 80 KB at 4 planes, freed at its end; every drawing call is the
  same, the screen's pointer is swapped) and notes the rectangles it drew. At most one move a pass of the server's
  loop, to where the pointer is by then (the mouse's moves since are merged). At the end of the pass what changed is
  copied onto the screen on view: one blit of whole words, B to D, top to bottom, started where the beam can neither
  overtake it nor come round to meet it before it ends: after the beam left the band when the copy fits in a frame,
  else (a copy slower than the beam) just after the beam passed the band's top. The copy's speed is measured from the
  shows themselves (about 1 us a word of one plane in FS-UAE's A1200 with the 4-plane display, against the HRM's
  0.56 without contention). The moved window keeps its inside's copy for the whole drag. A part the move uncovers that
  a program draws itself (the desktop's icons, a simple refresh window) is shown together with that program's
  drawing, or after 60 ms. Without chip RAM for the second screen the drag is an outline. `DPY_STAT` counts the drag's
  moves, shows, their times, the time waited for the beam, and shows the server's own check finds late for it.
- **Measured** (`tools/drag-test.sh`, FS-UAE's stock A1200 with 4 MB fast, a 400 x 140 window over another,
  dragged for about five seconds; emulator figures, not the real machine's): before, 99 moves, 119.6 ms each, 86
  blits and 124,135 pixels a move, 2 of 14 screenshots torn; now, 120 moves, 75.9 ms drawing each (88 blits, 68,116
  pixels), 2.8 shows a move of 6.3 ms each and 1.2 ms waiting for the beam on average, 0 of 14 torn, 0 shows late.
  Whole drag stays slow on a 14 MHz 68020 (about 10 moves a second for that window), so `drag auto` keeps the outline
  there (question 3's default); `drag whole` in a theme asks for it anyway. displayd grew by 4,124 bytes of code
  (39,188 -> 43,312), and takes 80 KB of chip RAM while a whole window is dragged.
- **Resize in the text-UI programs:** the library (`ports/tui/zenix/core.c`) now notes SIGWINCH (a handler without
  SA_RESTART, so a wait ends), reads the size again and gives upstream's TE_RESIZE, which Ahiru, Sumi and ctop already
  answer for CTerm on AmigaOS; the AmigaOS layer's `Wait()` answers the terminal's bit for it. `tuitest` checks it on
  a pty.


## Dragging with the chips (2026-11-06, branch hwdrag)

The owner on the PiStorm A1200 with the demo floppy, after the tear-free drag above: "Dragging windows is painfully
slow and I can not go public like that." Copying a window costs blitter time in proportion to its size, every move;
the goal is a drag that feels instant on the stock A1200 (68EC020 at 14 MHz, the floor), with no tearing. So the
window is moved by the display hardware, not copied.

**The design (AGA):**
- **On drag start the window is lifted into playfield 2** (AGA's dual playfield: 4 + 4 planes in hires, playfield 2
  in front). Its picture (frame, shadow and inside) is drawn once into a bitmap of its own in chip RAM, every line of
  every plane followed by 88 zero bytes: the display fetches 88 bytes a plane a line (below), so the bytes around the
  window always read as zero, which playfield 2 shows as transparent. From then on the window is "not shown" for the
  painter: the area it covered is painted once (the desktop, icons and windows behind it) under playfield 2, so no
  pixel visibly changes. Its program's drawing meanwhile goes to its kept copy.
- **Each move rewrites a few Copper words; no blit.** Vertical place: the Copper waits for the line above the window
  and points the even planes (BPL2/4/6/8PT) at the bitmap, with BPL2MOD the bitmap's line minus 88; under the window it
  points them at 88 zero bytes with BPL2MOD -88 (the same zero line every line). Horizontal place, the window always
  on an 8-pixel step (D38 3.2): the pointer moves in 64-pixel steps (the 64-bit fetch needs 8-byte aligned pointers)
  and BPLCON1's playfield-2 delay the rest, 0..56 hires pixels. The delay needs one more fetch unit on the left: the
  fetch starts at DDFSTRT $28 (one 64-pixel unit before the usual $38), so 11 units a line, and playfield 1 (the
  desktop) has its pointers 8 bytes earlier and its modulo 8 less, so it shows exactly as before.
- **Never torn:** the part of the Copper list that changes is double-buffered. The fixed part ends with
  `COP2LC = list; COPJMP2`; a move writes the list not in use, then changes one word (COP2LCL; both lists in one
  aligned 512-byte block, so the high word is the same). The Copper reads that word at the top of a frame, so every
  frame shows the window wholly at its old place or wholly at its new one. A move waits, if it must, until the frame
  after the last change has begun (one move a frame at most).
- **Colours:** playfield 2 takes colours 32..47 (BPLCON3's PF2OF = 32, bank 1; the pointer's sprite colours stay at
  16..31), loaded with the same 16 colours. Pen 0 of a playfield is transparent, so a window with pixels in pen 0 has
  them re-coloured in its lifted copy to a pen it does not use, whose playfield-2 colour is pen 0's. A window that uses
  all 16 pens (and pen 0) is dragged the old way.
- **While dragged it is in front of everything** but the pointer, the bar too (playfield 2 is in front of the whole
  screen), and its own program's drawing shows at the drop (it goes to the kept copy meanwhile).
- **On drop:** the window is placed at the new spot (painted under playfield 2, which shows the same picture there),
  then the Copper turns playfield 2 off at the next frame, and the bitmap is freed.

**The cost:** chip RAM while dragging: (window bytes + 88) x 4 planes x lines, rounded to 8 bytes: a 400 x 140
window about 81 KB, the screen's own 80 KB being the old drag's cost. Display DMA while dragging: 8 planes at the
64-bit fetch, 11 units a line, 88 of a line's 227 slots (40 without), a few more slots taken from the CPU on a
chip-only machine for the drag's length. At the start: the window drawn into its bitmap once (a few blits), the area
it leaves painted once; a pen-0 check of its pixels by the CPU (the full count of pens only when pen 0 is used).
Each move: one of the two lists written (about 37 Copper instructions), one word flipped, no blits, no pixels.

**Fallbacks:** OCS/ECS (no 8 planes), a resize, no chip RAM for the bitmap, a window with no kept copy (simple
refresh, partly hidden), or all 16 pens used: the earlier ways (the outline, or the whole window drawn off the screen
and shown timed to the beam), as the frame style's `drag` setting says. On AGA a move is always the chips' drag when
it can be: the `drag` setting now chooses only the fallback and the resize.

**Measured** (`tools/drag-test.sh`, FS-UAE's stock A1200 with 4 MB fast, the same 400 x 140 window over another
dragged for about five seconds; emulator figures, not the real machine's): the tear-free drag of 2026-11-05 took per
move 87.9 blits, 68,119 pixels, 75.8 ms drawing and 2.8 shows of 6.2 ms (120 moves); the chips' drag takes 0.46 ms of
the server's time a move (writing the list, and any wait for the frame), and for the whole drag 52 blits (lifting and
dropping) against 10,549: 0 of 14 screenshots torn, held before the drop the window shows exactly where the drop leaves
it, and a pen-0 patch dragged over another window shows pen 0's colour. The moves are as many as the mouse gives (one a
frame at most), on any CPU. displayd grew by 3,418 bytes of code (43,312 -> 46,730). Verified in FS-UAE, not yet on
the A1200.

**On the A1200 (the owner, PiStorm, release 2026110602, 2026-11-06):** "man it's fast!!! Wow." But while dragging,
left of the window on every line it covers, playfield 2 showed stripes of old pixels; after the drop all was right. The
cause was the blitter, not the display: the window's bitmap (its bytes plus 88 a plane line) is cleared by one blit a
plane, and BLTSIZE has 6 bits for the width (0 meaning 64 words). A window (with its frame and shadow) wider than
about 320 pixels makes the bitmap's line wider than 64 words, so the width spilled into the height: only the first few
words of each line were cleared; from about 260 to 320 pixels (exactly 64 words) all was cleared and one line more,
past the bitmap's end into whatever chip RAM followed. FS-UAE's chip RAM starts at zero, so the
uncleared padding read as zero there; a real Amiga's holds old pictures. Now a fill wider than 64 words is done in
pieces of 64 (draw.c `blit_rect`), and every blit writes the width as `W & 63`. The DDFSTRT `$28`, the playfield-2
delay and the modulos were right on the real chips (the window and the desktop around it showed true). `drag-test`
now runs `gcontest dirty` first (the free chip RAM filled with 0xFF, as a real machine's holds data), and checks the
whole screen held before the drop against the one after it: before the fix 33,307 pixels differed, after it none.
displayd +150 bytes.

## Windows stay on the screen (2026-11-06, branch clamp)

The owner on the A1200: "If I drag a window and drop it with its border on the top bar, when I let go the window's
title bar is behind the bar and I can't grab and move it again. Maybe hard borders that windows can't go past? So every
window stays on screen." Now a framed window (normal or dialog) can never be where its title bar cannot be grabbed:

- **The rule (revised 2026-11-06, below):** the whole frame stays on the screen, both ways: its top never above the
  bottom of a bar across the top of the screen (a shown `WK_BAR` as wide as the screen; a menu, narrower, is no
  edge), its bottom never below the screen's bottom (or a full-width bar along it), and across wholly on the screen
  (the shadow may go past the right and bottom edges). A window taller than the room below the bar has its top at
  the bar and its bottom past the screen's; one wider than the screen keeps its left edge at the first column the
  8-pixel step allows (column 0 when the left border is a multiple of 8, else the next: 7 for the 1-pixel borders).
  A window is moved the least to meet it. The pointer may go on; the window stops at the edge, as on Workbench, and
  follows again when the pointer comes back past where it stopped.
- **Where it lives: displayd** (`display/server/win.c`, `onscreen`), not zdesk: the server runs every drag (D38 3.3)
  and is the one place every position passes, whoever asks: the manager's `DPY_WM_PLACE`, the server's own placing
  without a manager, `DPY_WM_SHOW` of a window never placed, each step of every drag (the chips' drag, the whole window
  drawn off the screen, the outline) and its drop, a resize by the frame (its width and height stop at the screen's
  right and bottom edges unless the window's least size does not fit, which then moves it), a new frame style
  (`DPY_WM_STYLE`), and a bar placed or shown (every other window put back inside the new edges). zdesk's own placing
  (below the bar, on the screen) stays as it was; the server's rule is the guarantee for any manager and client.
- **Verified in FS-UAE:** `dpy-test` (a window asking for 900,400 opens at 535,244; placed at -300,-50 and 600,250 it
  stays at 7,0 and 431,244), `drag-test` (a 400 x 140 window dragged far up and left by the chips stops at 7,11 under
  the bar, with all 16 pens dragged far right and down by the off-screen drawing at 231,244; held at the edge each shows
  where the drop leaves it), `zdesk-test` (with the outline, far up and left: dropped at 7,11, its title bar below the
  bar; then far right and down at 431,244). Not yet on the A1200. displayd grew by 882 bytes of code (46,880 -> 47,762).

**On the A1200 (the owner, PiStorm, release 2026110605, 2026-11-06):** "Window title bar can't go outside the screen
up/down, left/right. But the rest of the window can": a terminal dragged down until only its title bar showed at the
bottom. The owner's wish was "every window stays on screen", so now the whole frame does (the rule above; it was the
title bar alone): `onscreen` stops the frame's top at the screen's bottom less the whole frame's height, the same
for every placing and drag. Verified in FS-UAE: `dpy-test` (900,400 asked: 535,203; placed at 600,250: 431,163, the
frame's bottom on the screen's), `drag-test` (16 pens, far right and down: 231,103), `zdesk-test` (the outline, far
right and down: 431,163). displayd +14 bytes of code (47,762 -> 47,776).

And a photo taken while dragging showed the title bar about 5 pixels out of line with the body below it, lined up
again after the drop. Read from the code: the lifted bitmap is one picture, the frame and the inside drawn into it at
the same offset (`lift_start`: the frame at column `loff`, the inside copied at `loff` plus the left border, as on
the screen), the inside's 8-pixel column and the frame's come from the same `fx`, and playfield 2 has one pointer
and one delay for all its lines (BPLCON1 written once a frame at the list's top, its bits as the AGA notes give them:
PF2H0 35 ns); nothing in it differs between the title bar's lines and the body's. A moving window is at a new place
every frame (5 pixels is about one move), so a camera's rolling shutter or a monitor's scaler that catches the top
of one frame and the rest of the next shows exactly that step; a still drag (held) and every screenshot showed none.
`drag-test` now checks it: in each of the 14 screenshots of the drag the window's frame and inside are pixel for
pixel those after the drop at the place they show (`dpy-test.py moved`; the pointer's box left out, and each pixel
may be a blend of its neighbours, as the screenshot's scaling samples a moved picture at another phase: a title bar
shifted by 2 pixels against the body is caught, tried on an altered screenshot). If the A1200
shows the step when the window is held still mid-drag, it is the chips', not the camera's: say so.

## Screen modes: hires and hires interlaced (2026-11-06, branch modes)

The owner (2026-11-06): "We need to be able to select screen modes... at least PAL hires and interlaced." Section 6.1
planned 640 x 512 as "a later setting"; this is it. Built on branch modes; the kernel is unchanged (its text console
keeps its mode: the desktop's screen is displayd's own Copper list after `CON_GRAB`).

**The modes and the setting.** `/etc/zdesk/screen`, one line `mode hires|hires-lace|auto` (no file, no line or
`auto`: hires, the mode that never flickers):

| Mode | PAL | NTSC | Lines a field | Chip RAM, the screen (AGA 4 planes / OCS-ECS 3) |
|---|---|---|---|---|
| `hires` | 640 x 256 | 640 x 200 | all | 80 KB / 60 KB (PAL), 63 / 47 (NTSC) |
| `hires-lace` | 640 x 512 | 640 x 400 | every other | 160 KB / 120 KB (PAL), 125 / 94 (NTSC) |

PAL or NTSC is never chosen: displayd builds on the console's height, which the kernel set at boot from the Agnus/Alice
id (256 or 200 lines; CLAUDE.md's rule), and interlace doubles it. displayd reads the file when it starts (also for
dlogin's screen: the file is in zdesk's folder because it is the desktop's setting, and displayd is the one program
that acts on it).

**Changed while the desktop runs: `DPY_MODE`** (protocol request 15, any client of the desktop's session; the answer
is the mode now). displayd allocates the new screen (refused with -ENOMEM, the screen as it was, when chip RAM is
short by the rule below), tells every client `EV_SCREEN` (event 10: the new width and height, the mode; libdisplay's
`dpy_w` and `dpy_h` follow it), gives up the kept copies (they are copies of the old look; the programs draw again on
`EV_EXPOSE`), gives the desktop's window the new size (`EV_RESIZE`), shortens a window taller than the room (in its
steps, `EV_RESIZE`), keeps every framed window on the new screen by the rule of "Windows stay on the screen", paints
all of it into the new screen while the old one is still shown, then points the Copper at it (one frame) and frees
the old one. Refused with -EBUSY during a drag or while another console is shown. `DPYM_ASK` only asks. dpytest's
`mode` command drives it in the tests.

**Test, Use, Save, Cancel (the owner 2026-11-06: "test/use/save/cancel options", AmigaOS's preferences editors).** The
preferences program (the next step) maps them onto the file and the request:
- **Test:** `DPY_MODE` with the chosen mode; the file is not written. The program remembers the mode it found
  (`DPYM_ASK`) for Cancel.
- **Use:** the same request (if Test did not already make it), the file not written: the mode lasts until displayd
  ends (the session's Log out, or the next boot under dlogin), as AmigaOS's Use lasts until the reboot.
- **Save:** the request, and the line written to `/etc/zdesk/screen` (root's file: the program asks for root's
  rights as `passwd` does, or writes the user's `~/.config/zdesk/screen` once displayd reads one; today the one file).
- **Cancel:** `DPY_MODE` with the remembered mode (nothing if it never changed), the file untouched.
A failed request (-ENOMEM, -EBUSY) is shown and the mode stays; the program never writes the file for a mode that
failed.

**The text console the same way (designed here, not built: the coordinator schedules it).** Today `/etc/console`
(colours, font, login) is read by init at every start and applied with `CON_COLOURS` and the font's load; there is no
mode. A live mode and font switch for Test/Use/Cancel needs one kernel call, `SYS_console` `CON_MODE` (root; the
console's lines: hires or interlaced, and the font: Topaz or a PSF/ZCF already loaded), doing what `gfx_init` does for
the size: the kernel's screen allocated again (40-160 KB, or 320 KB for 16 colours interlaced), its Copper list's
plane pointers, modulos and BPLCON0's LACE (two field lists, the same line-312 wait as here), `trows` (32 or 64 PAL,
25 or 50 NTSC: `TROWS_MAX` and the cells' grids grow to 64 rows, 3 x 64 x 80 = 15 KB a console instead of 7.5 KB, the
ten consoles' grids allocated as they are shown), every console's text drawn again, and `SIGWINCH` to each console's
foreground group. An estimate of its size: 1.0-1.5 KB of kernel code (the lists and the re-allocation; the drawing is
there already) and 7.5 KB of data a used console for the taller grids, against `tools/ksize-budget`'s 92 bytes left
today: the budget must grow first, which is the owner's call. `/etc/console` would gain `mode hires|hires-lace|auto`,
Save writing it; Test, Use and Cancel the call.

**Interlace on the chips (HRM "Interlaced bitplanes"; AGA the same).** BPLCON0's LACE bit (2): the display alternates a
long field (313 lines PAL, 263 NTSC) and a short one (312, 262), each showing every other line. The planes are
interleaved, so the screen in memory is simply 512 lines; each field fetches one line in two: the modulos grow by one
whole line of all planes (`stride`), and the short field's plane pointers start one line (`stride` bytes) further in
(the long field shows lines 0, 2, 4..., HRM figure 2-1). So there are two Copper lists, one a field:
- **Which list runs is kept right by the Copper itself**, with no interrupt and no LOF read: each field's list ends
  with `COP1LC = the long field's list`, then waits for line 312 (PAL; 262 NTSC) and there writes `COP1LC = the short
  field's list`. Only a long field has that line, so after a long field the short list runs next, after a short one
  the long list. A start out of step (the kernel's `gfx_hand` points COP1LC at one list) is right after one field.
  (AmigaOS points the two lists at each other and re-checks LOF in the vertical blank, aos31-study/graphics-and-layers;
  the line-312 wait needs neither.)
- **The fixed part** (palette, sprites, FMODE) is written twice (`set_palette` writes both); each copy ends with its
  field's `COP2LC` and `COPJMP2`. **The changing part** (planes, and playfield 2 while dragged by the chips) is four
  lists: two (the double buffer of "Dragging with the chips") for each field, in one aligned 1 KB block, and a change
  writes both fields' new lists, then flips the two `COP2LCL` words. 512 bytes more chip RAM than hires.

**The pointer.** Sprite DMA fetches a sprite line on the frame's lines, so in both fields the same picture is shown at
the same place: the 16-line arrow covers 32 lace lines, the same size on the glass as in hires (as AmigaOS's pointer in
an interlaced screen without its own laced pointer image). The kernel's pointer stays in frame lines (0..255 PAL, 0..199
NTSC: the kernel unchanged); displayd doubles the vertical: screen line = 2 x the kernel's. **The hot spot** (the
arrow's tip, its first line) is then on lines 0, 2, ... 510; the last line, 511, is under the tip's second half, so a
click at the bottom edge hits line 510, which every window and the desktop include. **The mouse's speed:** a count is
one frame line, two lace lines: the pointer moves as far on the glass as in hires (the acceleration of
`/etc/zdesk/mouse` the same).

**The drag in playfield 2 (AGA, "Dragging with the chips"), in interlace.** The lifted window's bitmap is one picture
of its lines; each field shows its half: in a field's list the window starts at the first screen line at or below the
window's top with the field's parity (line `y` is on frame line `y / 2` of field `y & 1`), at that line of the bitmap,
and playfield 2's modulo grows by one bitmap line (two lines a fetch, less the 88). Its end and the zero line likewise.
So both fields' pointers are written at every move, and the frame shows the window wholly at its old or its new place
in each field (a move written between the two fields of one frame shows in the second only: never a torn field, at
worst one field (20 ms) of the window at the old place on alternate lines, as any interlaced picture changing).

**The whole window's drag off the screen (OCS/ECS, and the fallbacks).** Its show is timed to the beam in frame
lines: a band of screen lines `y0..y1` is on frame lines `y0 / 2 .. (y1 + 1) / 2` of each field, a frame line of the
band holds two screen lines to copy. The second screen is 160 KB (120 OCS/ECS) instead of 80; without it the drag is
the outline, as before.

**The blitter's height.** BLTSIZE holds at most 1,024 lines, and a copy of every plane at once (interleaved) is lines x
planes: 512 x 4 = 2,048 for a whole lace screen. Such a block is done in pieces of at most 1,024 (from the bottom when
it copies descending), in hires too (where the screen's 1,024 fitted exactly).

**Chip RAM, and what falls back.** Interlace costs: the screen twice (+80 KB AGA, +60 OCS/ECS), the whole drag's second
screen twice when it runs (160 KB), the chips' drag bitmap the window's lines (a window as tall on the glass has twice
the lines: a 400 x 280 lace window about 162 KB while dragged), and a window as tall on the glass keeps a copy twice the
size. The kept copies' budget stays 256 KB (past it simple refresh, as before), so fewer windows are smart refresh.
On the minimum (2 MB chip RAM, 4 MB fast) that leaves well over 1.5 MB of chip RAM. **The fallback:** displayd takes
interlace only when the chip RAM free at its start is at least the lace screen plus 512 KB (the drags, the copies,
sound, ZINE, disk buffers); else, and when the screen's allocation fails, it starts in hires and says why on its
standard error (the serial log): "displayd: interlace needs N KB of chip RAM, M KB free: hires". On a 2 MB chip-only
A1200 that is about 1.1 MB free with one login: interlace fits there too (best effort, as the rest).

**Text, fonts and the clients.** The same 8 x 8 Topaz and supplement: in 512 lines a line of text is half as tall on
the glass, and twice as many fit (64 rows on the screen), as on Workbench at 640 x 512 with Topaz 8. A taller font
(8 x 16, or Topaz 11 from a file) is a theme's choice once the server loads fonts (TrueType later, section 5), not
this step. The screen's size reaches the clients in `DPY_HELLO`'s answer (`dpy_w`, `dpy_h`), as before; zdesk had
640 x 256 written in (its placing, zoom, the desktop window, the bar; also wrong on NTSC's 200 lines) and now uses
`dpy_w` and `dpy_h`. `term`'s rows grow from 4..29 to 4..32 where the screen is taller: the console's cell model has
32 rows (`TROWS_MAX` in kernel.h, the kernel's); a terminal taller than 32 rows needs that model bigger, a later step.
Icons and gadgets are the same pictures: half as tall on the glass in interlace.

**Flicker.** Interlace on a 15 kHz monitor or a TV flickers (each line is lit 25 times a second on PAL, 30 NTSC),
worst on a thin horizontal line or a one-line pattern. Through a flicker fixer or a scandoubler (the Indivision AGA,
an RGB-to-HDMI) both fields are woven into one steady picture. Which the owner uses is not assumed: hires stays the
default, and the themes' one-pixel lines are as they are (a theme for interlace on a TV would use two-line lines).

**Not in this step:** the text console's mode (above: designed, not built); the preferences program; a user's own
`~/.config/zdesk/screen`; super-hires (1280 wide), productivity, RTG; a laced pointer image (to the line).

**Tests (FS-UAE):** `tools/lace-test.sh`: the desktop in `hires-lace` on the stock A1200 with 4 MB fast (screenshots of
the desktop, a term, the pointer at the bottom edge, `DPY_MODE` to hires and back, checked pixel by pixel against
the places the server reports), PAL and NTSC (FS-UAE's NTSC A1200: 640 x 400 and 640 x 200); `drag-test lace`: the
whole drag test (the chips' drag held mid-way, the edges, the 16-pen drag drawn off the screen) on the interlaced
screen.
